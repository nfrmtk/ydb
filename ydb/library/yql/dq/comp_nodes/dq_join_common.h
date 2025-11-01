#pragma once
#include "dq_hash_join_table.h"
#include <vector>
#include <ydb/library/yql/dq/comp_nodes/hash_join_utils/block_layout_converter.h>
#include <yql/essentials/minikql/computation/mkql_block_reader.h>
#include <yql/essentials/minikql/computation/mkql_computation_node.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/minikql/mkql_program_builder.h>

namespace NKikimr::NMiniKQL {
struct TColumnsMetadata {
    std::vector<ui32> KeyColumns;
    std::vector<TType*> ColumnTypes;
};

template <typename T>
using 

using TPackResult = IBlockLayoutConverter::TPackResult;

void ForEachPackedTuple(const TPackResult& tuples, const NPackedTuple::TTupleLayout* layout , std::invocable<const ui8*> auto tupleCallback) {
    for(int index = 0; index < tuples.NTuples; ++index) {
        tupleCallback(&tuples.PackedTuples[index*layout->TotalRowSize]);
    }
}

enum class ESide { Probe, Build };

template <typename T> struct TSides {
    T Build;
    T Probe;

    T& SelectSide(ESide side) {
        return side == ESide::Build ? Build : Probe;
    }

    const T& SelectSide(ESide side) const {
        return side == ESide::Build ? Build : Probe;
    }
};


/*
  usage:
  instead of copy pasting code and changing "build" to "probe", use TSides and ForEachSide to call same code twice.
  example:

void f(int buildSize, int probeSize, bool buildRequired, bool probeRequired){
    use(buildSize + (buildRequired ? 0 : transform(buildSize)));
    use(probeSize + (probeRequired ? 0 : transform(probeSize)));
}

vs

void f(TSides<int> sizes, TSides<bool> required) {
    ForEachSide([&](ESide side){
        use(sizes.SelectSide(side) + (required.SelectSide(side) ? 0 : transform(sizes.SelectSide(side))));
    });
}
  
*/
void ForEachSide(std::invocable<ESide> auto fn) {
    fn(ESide::Build);
    fn(ESide::Probe);
}

struct TJoinMetadata {
    TColumnsMetadata Build;
    TColumnsMetadata Probe;
    TKeyTypes KeyTypes;
};

TKeyTypes KeyTypesFromColumns(const std::vector<TType*>& types, const std::vector<ui32>& keyIndexes);

template <EJoinKind Kind> struct TRenamedOutput {
    TRenamedOutput(TDqUserRenames renames, const std::vector<TType*>& leftColumnTypes,
                   const std::vector<TType*>& rightColumnTypes)
        : OutputBuffer()
        , NullTuples(std::max(leftColumnTypes.size(), rightColumnTypes.size()), NYql::NUdf::TUnboxedValuePod{})
        , Renames(std::move(renames))
    {}

    int TupleSize() const {
        return Renames.size();
    }

    int SizeTuples() const {
        MKQL_ENSURE(OutputBuffer.size() % TupleSize() == 0, "buffer contains tuple parts??");
        return OutputBuffer.size() / TupleSize();
    }

    std::vector<NUdf::TUnboxedValue> OutputBuffer;

    auto MakeConsumeFn() {
        return [&] {
            if constexpr (SemiOrOnlyJoin(Kind)) {
                return [&](NJoinTable::TTuple tuple) {
                    MKQL_ENSURE(tuple != nullptr, "null output row in semi/only join?");
                    for (int index = 0; index < std::ssize(Renames); ++index) {
                        auto thisRename = Renames[index];
                        OutputBuffer.push_back(tuple[thisRename.Index]);
                    }
                };
            } else {
                return [&](NJoinTable::TTuple probe, NJoinTable::TTuple build) {
                    if (!probe) { // todo: remove nullptr checks for some join types.
                        probe = NullTuples.data();
                    }

                    if (!build) {
                        build = NullTuples.data();
                    }
                    for (int index = 0; index < std::ssize(Renames); ++index) {
                        auto thisRename = Renames[index];
                        if (thisRename.Side == EJoinSide::kLeft) {
                            OutputBuffer.push_back(probe[thisRename.Index]);
                        } else {
                            OutputBuffer.push_back(build[thisRename.Index]);
                        }
                    }
                };
            }
        }();
    }

  private:
    const std::vector<NYql::NUdf::TUnboxedValue> NullTuples;
    const TDqUserRenames Renames;
};

// Some joins produce concatenation of 2 tuples, some produce one tuple(effectively)
template <typename Fun, typename Tuple>
concept JoinMatchFun = std::invocable<Fun, NJoinTable::TTuple> || std::invocable<Fun, TSides<Tuple>>;

TPackResult Flatten(std::vector<TPackResult> tuples);

template <typename Source, EJoinKind Kind> class TJoin : public TComputationValue<TJoin<Source, Kind>> {
    using TBase = TComputationValue<TJoin>;

  public:
    TJoin(TMemoryUsageInfo* memInfo, Source probe, Source build, TJoinMetadata meta, NUdf::TLoggerPtr logger,
          TString componentName)
        : TBase(memInfo)
        , Meta_(meta)
        , Logger_(logger)
        , LogComponent_(logger->RegisterComponent(componentName))
        , Build_(std::move(build))
        , Probe_(std::move(probe))
        , Table_(BuildSize(), TWideUnboxedEqual{Meta_.KeyTypes}, TWideUnboxedHasher{Meta_.KeyTypes},
                 NJoinTable::NeedToTrackUnusedRightTuples(Kind))
    {
        MKQL_ENSURE(BuildSize() == ProbeSize(), "unimplemented");
        MKQL_ENSURE(Kind != EJoinKind::Cross, "Unsupported join kind");
        UDF_LOG(Logger_, LogComponent_, NUdf::ELogLevel::Debug, "TScalarHashJoinState created");
    }

    const TJoinMetadata& Meta() const {
        return Meta_;
    }

    int ProbeSize() const {
        return Probe_.UserDataSize();
    }

    int BuildSize() const {
        return Build_.UserDataSize();
    }

    EFetchResult MatchRows(TComputationContext& ctx, auto consumeOneOrTwoTuples) {
        while (!Build_.Finished()) {
            auto res = Build_.ForEachRow(ctx, [&](auto tuple) { Table_.Add({tuple, tuple + Build_.UserDataSize()}); });
            switch (res) {
            case NYql::NUdf::EFetchStatus::Finish: {
                Table_.Build();
                break;
            }
            case NYql::NUdf::EFetchStatus::Yield: {
                return EFetchResult::Yield;
            }
            case NYql::NUdf::EFetchStatus::Ok: {
                break;
            }
            default:
                MKQL_ENSURE(false, "unreachable");
            }
        }
        if (!Probe_.Finished()) {
            auto result = Probe_.ForEachRow(ctx, [&](NJoinTable::TTuple probeTuple) {
                bool found = false;
                Table_.Lookup(probeTuple, [&](NJoinTable::TTuple matchedBuildTuple) {
                    if constexpr (ContainsRowsFromInnerJoin(Kind)) {
                        consumeOneOrTwoTuples(probeTuple, matchedBuildTuple);
                    }
                    found = true;
                });
                if (!found) {
                    if constexpr (Kind == EJoinKind::Exclusion || Kind == EJoinKind::Left || Kind == EJoinKind::Full) {
                        consumeOneOrTwoTuples(probeTuple, nullptr);
                    }
                    if constexpr (Kind == EJoinKind::LeftOnly) {
                        consumeOneOrTwoTuples(probeTuple);
                    }
                }
                if constexpr (Kind == EJoinKind::LeftSemi) {
                    if (found) {
                        consumeOneOrTwoTuples(probeTuple);
                    }
                }
            });
            switch (result) {
            case NYql::NUdf::EFetchStatus::Finish: {
                int consumedTotal = 0;
                if (Table_.UnusedTrackingOn()) {
                    if constexpr (Kind == EJoinKind::RightSemi) {
                        for (auto& v : Table_.MapView()) {
                            if (v.second.Used) {
                                for (NJoinTable::TTuple used : v.second.Tuples) {

                                    ++consumedTotal;
                                    consumeOneOrTwoTuples(used);
                                }
                            }
                        }
                    }
                    Table_.ForEachUnused([&](NJoinTable::TTuple unused) {
                        if constexpr (Kind == EJoinKind::RightOnly) {
                            ++consumedTotal;
                            consumeOneOrTwoTuples(unused);
                        }
                        if constexpr (Kind == EJoinKind::Exclusion || Kind == EJoinKind::Right ||
                                      Kind == EJoinKind::Full) {
                            ++consumedTotal;
                            consumeOneOrTwoTuples(nullptr, unused);
                        }
                    });
                }
                return consumedTotal == 0 ? EFetchResult::Finish : EFetchResult::One;
            }
            case NYql::NUdf::EFetchStatus::Yield: {
                return EFetchResult::Yield;
            }
            case NYql::NUdf::EFetchStatus::Ok: {
                return EFetchResult::One;
            }
            default:
                MKQL_ENSURE(false, "unreachable");
            }
        }
        return EFetchResult::Finish;
    }

  private:
    const TJoinMetadata Meta_;
    const NUdf::TLoggerPtr Logger_;
    const NUdf::TLogComponentId LogComponent_;

    Source Build_;
    Source Probe_;
    NJoinTable::TStdJoinTable Table_;
};

// сщтсузе 

int MemoryUsagePercent(int totalBytes) {
    return 100*totalBytes / TlsAllocState->GetLimit();
}

std::optional<int> GetMemoryUsageIfReachedLimit() {
    if (!TlsAllocState->GetMaximumLimitValueReached()) {
        return std::nullopt;
    }
    return std::make_optional<int>(MemoryUsagePercent(TlsAllocState->GetUsed()));
}

bool MemoryPercentIsFree(int freePercent) {
    std::optional<int> usedPercent = GetMemoryUsageIfReachedLimit();
    return usedPercent && (freePercent + *usedPercent < 100);
}


struct TBucket {
    bool IsSpilled = false;
    TPackResult BuildingPage;
    std::vector<ISpiller::TKey> SpilledPages;
    std::vector<TPackResult> InMemoryPages;
    TPackResult DetatchBuildingPage() {
        TPackResult res = std::move(BuildingPage);
        BuildingPage.NTuples = 0;
        return res;
    }
};

using TBuckets = std::vector<TBucket>;   

struct BlobIdAndBucketIndex {
    NThreading::TFuture<ISpiller::TKey> BlobId;
    int BucketIndex;
};

void AppendTupleTo(const NPackedTuple::TTupleLayout* layout, NJoinTable::TNeumannJoinTable::Tuple tuple, TPackResult& to) {
    layout->TupleDeepCopy(tuple.PackedData, tuple.OverflowBegin, to.PackedTuples, to.Overflow);
    to.NTuples++;
}



NYql::TChunkedBuffer Serialize(TPackResult&& result) {
    NYql::TChunkedBuffer buff{};
    buff.Append(TStringBuilder() << result.NTuples);
    buff.Append(TString{reinterpret_cast<const char*>(result.PackedTuples.data()), result.PackedTuples.size()});
    buff.Append(TString{reinterpret_cast<const char*>(result.Overflow.data()), result.Overflow.size()});
    return buff;
}

struct TPackedTupleStorageSettings {
    int Buckets;
    int BucketSizeBytes;
};


template<TPackedTupleStorageSettings Settings>
class TBucketsPackedTupleStorage {
    std::optional<int> FindInMemoryBucketWithMostPages() const {
        std::optional<int> resIndex;
        for(int index = 0; index < std::ssize(Buckets_); ++index) {
            if (!Buckets_[index].IsSpilled && !Buckets_[index].InMemoryPages.empty()) {
                if (resIndex == std::nullopt) {
                    resIndex = index;
                } else {
                    if (Buckets_[*resIndex].InMemoryPages.size() < Buckets_[index].InMemoryPages.size()) {
                        resIndex = index;
                    }
                }
            }
        }
        return resIndex;
    }
    EFetchResult Wait() {
        return EFetchResult::Yield;
    }
public:

    void AddRow(NJoinTable::TNeumannJoinTable::Tuple tuple) {
        ui32 hash = NPackedTuple::Hash(tuple.PackedData);
        MKQL_ENSURE(std::popcount(Buckets_.size() - 1) == std::bit_width(std::size(Buckets_)) - 1, "size of buckets should be power of two");
        int bucketIndex = hash & (std::size(Buckets_) - 1);
        TBucket& thisBucket = Buckets_[bucketIndex];
        AppendTupleTo(Layout_, tuple, thisBucket.BuildingPage);
        if (PackResultSizeBytes( thisBucket.BuildingPage) > Settings.BucketSizeBytes) {
            thisBucket.InMemoryPages.push_back(thisBucket.DetatchBuildingPage());
            thisBucket.InMemoryPages.back().PackedTuples.shrink_to_fit();
            thisBucket.InMemoryPages.back().Overflow.shrink_to_fit();
        }
        // return bucketIndex;
    }
    EFetchResult SpillWhile(std::predicate auto condition) {
        while(condition()) {
            if (SpillingPages_.has_value()) {
                for(auto& future: *SpillingPages_) {
                    if (!future.BlobId.IsReady()){
                        return Wait();
                    }
                }
                for(auto& future: *SpillingPages_) {
                    MKQL_ENSURE(future.BlobId.IsReady(), "no blocking wait");
                    Buckets_[future.BucketIndex].SpilledPages.push_back(future.BlobId.ExtractValueSync());
                }
                SpillingPages_ = std::nullopt;
            } else {
                constexpr int kPagesSpillingAtTime = 3;
                int inMemoryPages = 0;
                while ((inMemoryPages = std::accumulate(Buckets_.begin(), Buckets_.end(), 0, [&](int pages, const TBucket& bucket) {
                    return pages + bucket.IsSpilled ? std::ssize(bucket.InMemoryPages) : 0;
                })) < kPagesSpillingAtTime) {
                    std::optional<int> bucketIndex = FindInMemoryBucketWithMostPages();
                    if (!bucketIndex) {
                        MKQL_ENSURE(false, "unimplemented"); // we can not spill much and do not have memory. spilling smaller chunks is not implemented currently. 
                    }
                    Buckets_[*bucketIndex].IsSpilled = true;
                }
                SpillingPages_.emplace();
                // std::vector<BlobIdAndBucketIndex> spilledPages;
                int totalSpillingPages = kPagesSpillingAtTime;
                for(int index = 0; index < std::ssize(Buckets_); ++index) {
                    auto& bucket = Buckets_[index];
                    while (bucket.IsSpilled && !bucket.InMemoryPages.empty() && totalSpillingPages != 0) {
                        totalSpillingPages--;
                        SpillingPages_->push_back({.BlobId = Spiller_->Put(Serialize(std::move(bucket.InMemoryPages.back()))), .BucketIndex = index});
                        bucket.InMemoryPages.pop_back();
                    }
                }
                MKQL_ENSURE(totalSpillingPages == 0, "not enough pages for spilling?");
            }

        }
    }
    TBuckets GetBuckets() {
        MKQL_ENSURE(!SpillingPages_.has_value(), "spilling process should stop when extracting buckets");
        return std::move(Buckets_);
    }
    TBuckets Buckets_;
    ISpiller::TPtr Spiller_;
    std::optional<std::vector<BlobIdAndBucketIndex>> SpillingPages_;
    const NPackedTuple::TTupleLayout* Layout_;
};

template<TPackedTupleStorageSettings Settings, typename Source>
class TPackedTupleBucketStorage: public NNonCopyable::TMoveOnly {
public:
    TPackedTupleBucketStorage(Source source): Values_(std::move(source)) {}

    
    EFetchResult Wait() {
        return EFetchResult::Yield;
    }

    FetchResult<TBuckets> FetchSide() {
        while (!Finished_) {
            if (!PackedTuples_.has_value()) {
                FetchResult<TPackResult> rows = Values_.Fetch();

                NYql::NUdf::EFetchStatus status = AsStatus(rows);
                // NYql::NUdf::EFetchStatus status = Values_.WideFetch(Buff_.data(), std::ssize(Buff_));
                if (status == NYql::NUdf::EFetchStatus::Yield) {
                    return Yield{};
                }
                if (status == NYql::NUdf::EFetchStatus::Finish) {
                    Finished_ = true;
                    return One{.Data = Storage_->GetBuckets()};
                }
                PackedTuples_ = std::move(GetPayload(rows));
            }
            auto res = Storage_->SpillWhile([]{
                return !MemoryPercentIsFree(30);
            });
            if (res == EFetchResult::Yield) {
                return Yield{};
            } else {
                for (int64_t tupleBeg = 0; tupleBeg < std::ssize(PackedTuples_->PackedTuples); tupleBeg += Layout_->TotalRowSize) {
                    Storage_->AddRow({.PackedData = &PackedTuples_->PackedTuples[tupleBeg*Layout_->TotalRowSize], .OverflowBegin = PackedTuples_->Overflow.data()});
                }
                PackedTuples_ = std::nullopt;
            }
            // FetchResult<TArrowRows> rows = FetchBlockAndWaitForEnoughMemory();
        }
        return Finish{};
    }
private:
    bool Finished_ = false;
    Source Values_;
    TBucketsPackedTupleStorage<TPackedTupleStorageSettings{.Buckets = 128, .BucketSizeBytes = (1<<19)}>* Storage_;
    const NPackedTuple::TTupleLayout* Layout_;
    std::optional<TPackResult> PackedTuples_;

};


template <typename Source> class TJoinPackedTuples {
  public:
    using TTable = NJoinTable::TNeumannJoinTable;

    TJoinPackedTuples(TSides<Source> sources, NUdf::TLoggerPtr logger, TString componentName,
                      TSides<const NPackedTuple::TTupleLayout*> layouts)
        : Logger_(logger)
        , LogComponent_(logger->RegisterComponent(componentName))
        , Sources_(std::move(sources))
        , Layouts_(layouts)
        , Table_(Layouts_.Build)
    {}

    TPackResult Flatten(std::vector<TPackResult> tuples) {
        TPackResult flattened;
        flattened.NTuples = std::accumulate(tuples.begin(), tuples.end(), i64{0},
                                            [](i64 summ, const auto& packRes) { return summ += packRes.NTuples; });

        i64 totalTuplesSize = std::accumulate(tuples.begin(), tuples.end(), i64{0}, [](i64 summ, const auto& packRes) {
            return summ += std::ssize(packRes.PackedTuples);
        });
        flattened.PackedTuples.reserve(totalTuplesSize);

        i64 totaOverflowlSize =
            std::accumulate(tuples.begin(), tuples.end(), i64{0},
                            [](i64 summ, const auto& packRes) { return summ += std::ssize(packRes.Overflow); });
        flattened.Overflow.reserve(totaOverflowlSize);

        int tupleSize = Layouts_.Build->TotalRowSize;
        for (const TPackResult& tupleBatch : tuples) {
            Layouts_.Build->Concat(flattened.PackedTuples, flattened.Overflow,
                                   std::ssize(flattened.PackedTuples) / tupleSize, tupleBatch.PackedTuples.data(),
                                   tupleBatch.Overflow.data(), tupleBatch.PackedTuples.size() / tupleSize,
                                   tupleBatch.Overflow.size());
        }
        return flattened;
    }

    EFetchResult MatchRows([[maybe_unused]] TComputationContext& ctx,
                           JoinMatchFun<TTable::Tuple> auto consumeOneOrTwoTuples) {
        while (!Sources_.Build.Finished()) {
            FetchResult<TPackResult> var = Sources_.Build.FetchRow();
            switch (AsStatus(var)) {
            case NYql::NUdf::EFetchStatus::Finish: {
                Table_.BuildWith(Flatten(BuildChunks_));
                break;
            }
            case NYql::NUdf::EFetchStatus::Yield: {
                return EFetchResult::Yield;
            }
            case NYql::NUdf::EFetchStatus::Ok: {
                auto& packResult = std::get<One<TPackResult>>(var);
                BuildChunks_.push_back(std::move(packResult.Data));
                break;
            }
            default:
                MKQL_ENSURE(false, "unreachable");
            }
        }
        if (Table_.Empty()) {
            return EFetchResult::Finish; // is it ok?
        }

        if (!Sources_.Probe.Finished()) {
            const FetchResult<TPackResult> var = Sources_.Probe.FetchRow();
            const NKikimr::NMiniKQL::EFetchResult resEnum = AsResult(var);

            if (resEnum == EFetchResult::One) {
                const TPackResult& thisPackResult =
                    std::get<One<TPackResult>>(var).Data;
                for (int index = 0; index < thisPackResult.NTuples; ++index) {
                    const ui8* thisRow = &thisPackResult.PackedTuples[index * Layouts_.Probe->TotalRowSize];
                    TTable::Tuple probeRow{thisRow, thisPackResult.Overflow.data()};
                    Table_.Lookup(probeRow, [&](TTable::Tuple matchedBuildRow) {
                        consumeOneOrTwoTuples(TSides<TTable::Tuple>{.Build = matchedBuildRow, .Probe = probeRow});
                    });
                }
            }

            return resEnum;
        }

        return EFetchResult::Finish;
    }

  private:
    const NUdf::TLoggerPtr Logger_;
    const NUdf::TLogComponentId LogComponent_;
    TSides<Source> Sources_;
    TSides<const NPackedTuple::TTupleLayout*> Layouts_;
    TTable Table_;
    TPackResult BuildData_;
    std::vector<TPackResult> BuildChunks_;
};

} // namespace NKikimr::NMiniKQL