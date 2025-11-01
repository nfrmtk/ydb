#include "dq_block_hash_join.h"

#include <yql/essentials/minikql/comp_nodes/mkql_blocks.h>
#include <yql/essentials/minikql/computation/mkql_block_builder.h>
#include <yql/essentials/minikql/computation/mkql_block_impl.h>
#include <yql/essentials/minikql/computation/mkql_block_reader.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders_codegen.h>
#include <yql/essentials/minikql/invoke_builtins/mkql_builtins.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/minikql/mkql_program_builder.h>

#include <algorithm>
#include <arrow/scalar.h>

#include "dq_join_common.h"

namespace NKikimr::NMiniKQL {

namespace {

using TDqJoinImplRenames = TDqRenames<ESide>;

struct TDqBlockJoinMetadata {
    TSides<TVector<TBlockType*>> InputTypes;
    TSides<TVector<int>> KeyColumns;
    TVector<TBlockType*> ResultItemTypes;
    TDqJoinImplRenames Renames;
};
TVector<arrow::Datum> ArrowFromUV(std::span<const NYql::NUdf::TUnboxedValue> UVs) {
    TVector<arrow::Datum> arrow;
    for (const auto& uv : UVs) {
        arrow.push_back(TArrowBlock::From(uv).GetDatum());
    }
    return arrow;
}
TVector<arrow::Datum> ArrowFromUV(const NYql::NUdf::TUnboxedValue* first, size_t size) {
    return ArrowFromUV({first, size});
}

using TArrowRows = TVector<arrow::Datum>;

template<TPackedTupleStorageSettings Settings>
class TBlockPackedTupleSource : public NNonCopyable::TMoveOnly {
  public:
    TBlockPackedTupleSource(TComputationContext& ctx, TSides<IComputationNode*> stream,
                            const TDqBlockJoinMetadata* meta,
                            TSides<std::unique_ptr<IBlockLayoutConverter>>& converters, ESide side)
        : Stream_(stream.SelectSide(side))
        , StreamValues_(Stream_->GetValue(ctx))
        , Buff_(meta->InputTypes.SelectSide(side).size())
        , ArrowBlockToInternalConverter_(converters.SelectSide(side).get())
    {}

    bool Finished() const {
        return Finished_;
    }

    int UserDataCols() const {
        return Buff_.size() - 1;
    }

    FetchResult<IBlockLayoutConverter::TPackResult> Fetch() {
        if (Finished()) {
            return Finish{};
        }
        if (!FetchedRows_.has_value()) {
            auto res = StreamValues_.WideFetch(Buff_.data(), Buff_.size());
            if (res != NYql::NUdf::EFetchStatus::Ok) {
                if (res == NYql::NUdf::EFetchStatus::Finish) {
                    Finished_ = true;
                    return Finish{};
                }
                return Yield{};
            }
            const size_t cols = UserDataCols();
            FetchedRows_ = ArrowFromUV({Buff_.data(), cols});
        }
        auto dontHaveMemoryToPack = [] {
            return !MemoryPercentIsFree(30);
        };
        EFetchResult res = Storage_->SpillWhile(dontHaveMemoryToPack);
        if (res == EFetchResult::Yield) {
            return Yield{};
        } else {
            IBlockLayoutConverter::TPackResult result;
            ArrowBlockToInternalConverter_->Pack(*FetchedRows_, result);
            FetchedRows_ = std::nullopt;
            return One{std::move(result)};
        }
    }

  private:

    bool Finished_ = false;
    IComputationNode* Stream_;
    NYql::NUdf::TUnboxedValue StreamValues_;
    TUnboxedValueVector Buff_;
    IBlockLayoutConverter* ArrowBlockToInternalConverter_;
    std::optional<TArrowRows> FetchedRows_;
    TBucketsPackedTupleStorage<Settings>* Storage_;
};


// IBlockLayoutConverter::TPackResult Parse( NYql::TChunkedBuffer&& buff) {
//     IBlockLayoutConverter::TPackResult res{};
//     res.NTuples = buff.
//     buff.Append(TStringBuilder() << result.NTuples);
//     buff.Append(TString{reinterpret_cast<const char*>(result.PackedTuples.data()), result.PackedTuples.size()});
//     buff.Append(TString{reinterpret_cast<const char*>(result.Overflow.data()), result.Overflow.size()});
//     return buff;
// }






bool AllInMemory(const TBuckets& buckets) {
    return !std::ranges::find(buckets, true, [](const TBucket& bucket){return bucket.IsSpilled;});
}

class TBlockPackResultSource {

};

struct TRenamesPackedTupleOutput : NNonCopyable::TMoveOnly {
    TRenamesPackedTupleOutput(const TDqBlockJoinMetadata* meta, TSides<IBlockLayoutConverter*> converters)
        : Renames_(&meta->Renames)
        , Converters_(converters)
    {}

    int Columns() const {
        return Renames_->size();
    }

    i64 SizeTuples() const {
        return Output_.NItems;
    }

    struct PackedTuplesData {
        std::vector<ui8, TMKQLAllocator<ui8>> PackedTuples;
        std::vector<ui8, TMKQLAllocator<ui8>> Overflow;
    };

    struct TuplePairs {
        i64 NItems = 0;
        TSides<PackedTuplesData> Data;
    };

    auto MakeConsumeFn() {
        return [this](TSides<NJoinTable::TNeumannJoinTable::Tuple> tuples) {
            ForEachSide([&](ESide side) {
                // AppendTupleTo(const NPackedTuple::TTupleLayout *layout, const ui8 *tuple, const IBlockLayoutConverter::TPackResult &from, IBlockLayoutConverter::TPackResult &to)
                Converters_.SelectSide(side)->GetTupleLayout()->TupleDeepCopy(
                    tuples.SelectSide(side).PackedData, tuples.SelectSide(side).OverflowBegin,
                    Output_.Data.SelectSide(side).PackedTuples, Output_.Data.SelectSide(side).Overflow);
            });
            Output_.NItems++;
        };
    }

    TVector<arrow::Datum> FlushAndApplyRenames() {
        TSides<TVector<arrow::Datum>> sides = Flush();
        TVector<arrow::Datum> renamed;
        for (auto rename : *Renames_) {
            renamed.push_back(sides.SelectSide(rename.Side)[rename.Index]);
        }
        return renamed;
    }

  private:
    TSides<TVector<arrow::Datum>> Flush() {
        TSides<TVector<arrow::Datum>> out;
        auto fillSide = [&](ESide side) {
            IBlockLayoutConverter::TPackResult res;

            res.NTuples = SizeTuples();
            res.PackedTuples = std::move(Output_.Data.SelectSide(side).PackedTuples);
            res.Overflow = std::move(Output_.Data.SelectSide(side).Overflow);
            Converters_.SelectSide(side)->Unpack(res, out.SelectSide(side));
        };
        fillSide(ESide::Build);
        fillSide(ESide::Probe);

        Output_.NItems = 0;
        return out;
    }

    TuplePairs Output_;
    const TDqJoinImplRenames* Renames_;
    TSides<IBlockLayoutConverter*> Converters_;
};

template <EJoinKind Kind> class TBlockHashJoinWrapper : public TMutableComputationNode<TBlockHashJoinWrapper<Kind>> {
  private:
    using TBaseComputation = TMutableComputationNode<TBlockHashJoinWrapper>;

  public:
    TBlockHashJoinWrapper(TComputationMutables& mutables, TDqBlockJoinMetadata meta, TSides<IComputationNode*> streams)
        : TBaseComputation(mutables, EValueRepresentation::Boxed)
        , Meta_(std::make_unique<TDqBlockJoinMetadata>(meta))
        , Streams_(streams)
    {}

    NUdf::TUnboxedValuePod DoCalculate(TComputationContext& ctx) const {
        TTypeInfoHelper helper;
        TSides<std::unique_ptr<IBlockLayoutConverter>> layouts;
        ForEachSide([&](ESide side) {
            int userTypesWidth = std::ssize(Meta_->InputTypes.SelectSide(side)) - 1;
            TVector<TType*> userTypes;
            for (int index = 0; index < userTypesWidth; ++index) {
                userTypes.push_back(Meta_->InputTypes.SelectSide(side)[index]->GetItemType());
            }
            TVector<NPackedTuple::EColumnRole> roles(userTypesWidth, NPackedTuple::EColumnRole::Payload);
            for (int column : Meta_->KeyColumns.SelectSide(side)) {
                roles[column] = NPackedTuple::EColumnRole::Key;
            }
            layouts.SelectSide(side) = MakeBlockLayoutConverter(helper, userTypes, roles, &ctx.ArrowMemoryPool);
        });
        return ctx.HolderFactory.Create<TStreamValue>(ctx, Streams_, std::move(layouts), Meta_.get());
    }

  private:
    class TStreamValue : public TComputationValue<TStreamValue> {
        using TBase = TComputationValue<TStreamValue>;
        using JoinType = TJoinPackedTuples<TBlockPackedTupleSource>;

      public:
        TStreamValue(TMemoryUsageInfo* memInfo, TComputationContext& ctx, TSides<IComputationNode*> streams,
                     TSides<std::unique_ptr<IBlockLayoutConverter>> converters, const TDqBlockJoinMetadata* meta)
            : TBase(memInfo)
            , Meta_(meta)
            , Streams_(streams)
            , Converters_(std::move(converters))
            , Join_(TSides<TBlockPackedTupleSource>{.Build = {ctx, streams, meta, Converters_, ESide::Build},
                                                    .Probe = {ctx, streams, meta, Converters_, ESide::Probe}},
                    ctx.MakeLogger(), "BlockHashJoin",
                    TSides<const NPackedTuple::TTupleLayout*>{.Build = Converters_.Build->GetTupleLayout(),
                                                              .Probe = Converters_.Probe->GetTupleLayout()})
            , Ctx_(&ctx)
            , Output_(meta, {.Build = Converters_.Build.get(), .Probe = Converters_.Probe.get()})
        {
            ForEachSide([&](ESide side) {
                Values_.SelectSide(side) = Streams_.SelectSide(side)->GetValue(ctx);
            });

        }

        NUdf::EFetchStatus FlushTo(NUdf::TUnboxedValue* output) {
            MKQL_ENSURE(Output_.SizeTuples() != 0, "make sure we are flushing something, not empty set of tuples");
            i64 rows = Output_.SizeTuples();
            TVector<arrow::Datum> arrowOutput = Output_.FlushAndApplyRenames();
            for (int colIndex = 0; colIndex < Output_.Columns(); ++colIndex) {
                output[colIndex] = Ctx_->HolderFactory.CreateArrowBlock(std::move(arrowOutput[colIndex]));
            }
            output[Output_.Columns()] = Ctx_->HolderFactory.CreateArrowBlock(arrow::Datum(static_cast<uint64_t>(rows)));

            MKQL_ENSURE(Output_.SizeTuples() == 0, "something left after flush??");
            return NYql::NUdf::EFetchStatus::Ok;
        }

      private:
        NUdf::EFetchStatus WideFetch(NUdf::TUnboxedValue* output, ui32 width) override {
            size_t expectedSize = Meta_->Renames.size() + 1;
            MKQL_ENSURE(width == expectedSize,
                        Sprintf("runtime(%i) vs compile-time(%i) tuple width mismatch", width, expectedSize));
            if (std::holds_alternative<Finished>(State_)) {
                return NYql::NUdf::EFetchStatus::Finish;
            } else if (FetchingBuildSide* ptr = std::get_if<FetchingBuildSide>(&State_)) {
                FetchResult<TBuckets> res = ptr->Fetcher.FetchSide();
                switch (AsStatus(res)) {

                case NYql::NUdf::EFetchStatus::Ok: {
                    TBuckets& buckets = GetPayload(res);
                    if (AllInMemory(buckets)) {
                        std::vector<IBlockLayoutConverter::TPackResult> inMemoryPages;
                        for(auto& bucket: buckets) {
                            MKQL_ENSURE(!bucket.IsSpilled, "spilled bucket in happy path?");
                            MKQL_ENSURE(bucket.SpilledPages.empty(), "spilled pages bucket in happy path?");
                            inMemoryPages.push_back(bucket.DetatchBuildingPage());
                            std::move(bucket.InMemoryPages.begin(), bucket.InMemoryPages.end(), std::back_inserter(pages));
                        }
                        State_ = StreamingProbeSide{Flatten(std::move(inMemoryPages))};
                        return WideFetch(output, width);
                    } else {
                        // std::vector<
                        PartiallyStreamingProbeSide build{};
                        std::vector<TPackResult> inMemoryPages;
                        for(int index = 0; index < std::ssize(buckets); ) {
                            TBucket& bucket = buckets[index];
                            if (bucket.IsSpilled) {
                                build.SpilledBuildBuckets.emplace_back(std::move(bucket), index);
                            } else {
                                inMemoryPages.push_back(bucket.DetatchBuildingPage());
                                std::move(bucket.InMemoryPages.begin(), bucket.InMemoryPages.end(), std::back_inserter(inMemoryPages));
                            }
                        }
                        build.InMemoryBuildSide = Flatten(std::move(inMemoryPages));
                        State_ = std::move(build);
                        return WideFetch(output, width);
                    }
                }
                case NYql::NUdf::EFetchStatus::Finish:
                    MKQL_ENSURE(false, "unreachable");
                case NYql::NUdf::EFetchStatus::Yield:
                    return NYql::NUdf::EFetchStatus::Yield;
                  break;
                }
            } 
            auto var = BuildStorage_.FetchSide();
            switch (AsStatus(var)) {

            case NYql::NUdf::EFetchStatus::Ok:
                
            case NYql::NUdf::EFetchStatus::Finish:
            case NYql::NUdf::EFetchStatus::Yield:
              break;
            }
            while (Output_.SizeTuples() < Threshold_) {
                auto res = Join_.MatchRows(*Ctx_, Output_.MakeConsumeFn());
                switch (res) {
                case EFetchResult::Finish: {
                    if (Output_.SizeTuples() == 0) {
                        return NYql::NUdf::EFetchStatus::Finish;
                    }
                    Finished_ = true;
                    return FlushTo(output);
                }
                case EFetchResult::Yield:
                    return NYql::NUdf::EFetchStatus::Yield;
                case EFetchResult::One:
                    break;
                }
            }
            return FlushTo(output);
        }

      private:
        struct FetchingBuildSide {
            TPackedTupleBucketStorage<TPackedTupleStorageSettings{.Buckets = 128, .BucketSizeBytes = (1<<19)}, TBlockPackedTupleSource<TPackedTupleStorageSettings Settings>> Fetcher;
        };

        struct StreamingProbeSide {
            IBlockLayoutConverter::TPackResult InMemoryBuildSide;
        };

        struct SpilledBucket {
            TBucket Bucket;
            int BucketIndex;
        };
        struct PartiallyStreamingProbeSide {
            std::vector<SpilledBucket> SpilledBuildBuckets;
            IBlockLayoutConverter::TPackResult InMemoryBuildSide;
        };

        struct PairsOfSpilledBuckets {
            TSides<TBucket> SpilledPages;
            int BucketIndex;
        };

        struct StreamingBuckets {
            PairsOfSpilledBuckets SpilledBuildBuckets;
        };

        struct Finished{};

        std::variant<FetchingBuildSide, StreamingProbeSide, PartiallyStreamingProbeSide, StreamingBuckets, Finished> State_;
        const TDqBlockJoinMetadata* Meta_;
        TSides<IComputationNode*> Streams_;
        TSides<NYql::NUdf::TUnboxedValue> Values_;
        TSides<std::unique_ptr<IBlockLayoutConverter>> Converters_;
        // TBlockPackedTupleBucketStorage BuildStorage_;
        JoinType Join_;
        TComputationContext* Ctx_;
        TRenamesPackedTupleOutput Output_;
        const int Threshold_ = 10000;
        bool Finished_ = false;
    };

    void RegisterDependencies() const final {
        this->DependsOn(Streams_.Build);
        this->DependsOn(Streams_.Probe);
    }

    std::unique_ptr<const TDqBlockJoinMetadata> Meta_;
    TSides<IComputationNode*> Streams_;
};

} // namespace

IComputationNode* WrapDqBlockHashJoin(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 7, "Expected 7 args");
    TDqBlockJoinMetadata meta;

    const auto joinType = callable.GetType()->GetReturnType();
    MKQL_ENSURE(joinType->IsStream(), "Expected WideStream as a resulting stream");
    const auto joinStreamType = AS_TYPE(TStreamType, joinType);
    MKQL_ENSURE(joinStreamType->GetItemType()->IsMulti(), "Expected Multi as a resulting item type");
    const auto joinComponents = GetWideComponents(joinStreamType);
    MKQL_ENSURE(joinComponents.size() > 0, "Expected at least one column");
    for (auto* blockType : joinComponents) {
        MKQL_ENSURE(blockType->IsBlock(), "Expected block types as wide components of result stream");
        meta.ResultItemTypes.push_back(AS_TYPE(TBlockType, blockType));
    }

    const auto leftType = callable.GetInput(0).GetStaticType();
    MKQL_ENSURE(leftType->IsStream(), "Expected WideStream as a left stream");
    const auto leftStreamType = AS_TYPE(TStreamType, leftType);
    MKQL_ENSURE(leftStreamType->GetItemType()->IsMulti(), "Expected Multi as a left stream item type");
    const auto leftStreamComponents = GetWideComponents(leftStreamType);
    MKQL_ENSURE(leftStreamComponents.size() > 0, "Expected at least one column");
    for (auto* blockType : leftStreamComponents) {
        MKQL_ENSURE(blockType->IsBlock(), "Expected block types as wide components of left stream");
        meta.InputTypes.Probe.push_back(AS_TYPE(TBlockType, blockType));
    }

    const auto rightType = callable.GetInput(1).GetStaticType();
    MKQL_ENSURE(rightType->IsStream(), "Expected WideStream as a right stream");
    const auto rightStreamType = AS_TYPE(TStreamType, rightType);
    MKQL_ENSURE(rightStreamType->GetItemType()->IsMulti(), "Expected Multi as a right stream item type");
    const auto rightStreamComponents = GetWideComponents(rightStreamType);
    MKQL_ENSURE(rightStreamComponents.size() > 0, "Expected at least one column");
    for (auto* blockType : rightStreamComponents) {
        MKQL_ENSURE(blockType->IsBlock(), "Expected block types as wide components of right stream");
        meta.InputTypes.Build.push_back(AS_TYPE(TBlockType, blockType));
    }
    const auto joinKindNode = callable.GetInput(2);
    const auto rawKind = AS_VALUE(TDataLiteral, joinKindNode)->AsValue().Get<ui32>();
    const auto joinKind = GetJoinKind(rawKind);

    const auto leftKeyColumnsLiteral = callable.GetInput(3);
    const auto leftKeyColumnsTuple = AS_VALUE(TTupleLiteral, leftKeyColumnsLiteral);
    for (ui32 i = 0; i < leftKeyColumnsTuple->GetValuesCount(); i++) {
        const auto item = AS_VALUE(TDataLiteral, leftKeyColumnsTuple->GetValue(i));
        meta.KeyColumns.Probe.emplace_back(item->AsValue().Get<ui32>());
    }

    const auto rightKeyColumnsLiteral = callable.GetInput(4);
    const auto rightKeyColumnsTuple = AS_VALUE(TTupleLiteral, rightKeyColumnsLiteral);
    for (ui32 i = 0; i < rightKeyColumnsTuple->GetValuesCount(); i++) {
        const auto item = AS_VALUE(TDataLiteral, rightKeyColumnsTuple->GetValue(i));
        meta.KeyColumns.Build.emplace_back(item->AsValue().Get<ui32>());
    }
    TDqUserRenames userRenames =
        FromGraceFormat(TGraceJoinRenames::FromRuntimeNodes(callable.GetInput(5), callable.GetInput(6)));

    MKQL_ENSURE(meta.KeyColumns.Build.size() == meta.KeyColumns.Probe.size(), "Key columns mismatch");

    const auto leftStream = LocateNode(ctx.NodeLocator, callable, 0);
    const auto rightStream = LocateNode(ctx.NodeLocator, callable, 1);
    MKQL_ENSURE(joinKind == EJoinKind::Inner, "Only inner is supported, see gh#26780 for details.");
    ValidateRenames(userRenames, joinKind, std::ssize(meta.InputTypes.Probe) - 1,
                    std::ssize(meta.InputTypes.Build) - 1);

    for (auto rename : userRenames) {
        ESide thisSide = [&] {
            if (rename.Side == EJoinSide::kLeft) {
                return ESide::Probe;
            } else {
                return ESide::Build;
            }
        }();
        meta.Renames.push_back({.Index = rename.Index, .Side = thisSide});
    }

    return new TBlockHashJoinWrapper<EJoinKind::Inner>(ctx.Mutables, meta, {.Build = rightStream, .Probe = leftStream});
}

} // namespace NKikimr::NMiniKQL
