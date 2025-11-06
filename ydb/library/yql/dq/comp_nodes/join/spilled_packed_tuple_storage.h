#pragma once
#include <ydb/library/yql/dq/comp_nodes/hash_join_utils/layout_converter_common.h>

namespace NKikimr::NMiniKQL {
struct TBucket {
    bool IsSpilled() const {
        return SpilledPages.has_value();
    }
    TPackResult BuildingPage;
    std::optional<TMKQLVector<ISpiller::TKey>> SpilledPages;
    TMKQLVector<TPackResult> InMemoryPages;
    TPackResult DetatchBuildingPage() {
        TPackResult res = std::move(BuildingPage);
        BuildingPage.NTuples = 0;
        return res;
    }
};

NYql::TChunkedBuffer Serialize(TPackResult&& result) {
    NYql::TChunkedBuffer buff{};
    buff.Append(TStringBuilder() << result.NTuples);
    buff.Append(TString{reinterpret_cast<const char*>(result.PackedTuples.data()), result.PackedTuples.size()});
    buff.Append(TString{reinterpret_cast<const char*>(result.Overflow.data()), result.Overflow.size()});
    return buff;
}


using TBuckets = TMKQLVector<TBucket>;   

struct BlobIdAndBucketIndex {
    NThreading::TFuture<ISpiller::TKey> BlobId;
    int BucketIndex;
};


struct TPackedTupleStorageSettings {
    int Buckets;
    int BucketSizeBytes;
};
constexpr TPackedTupleStorageSettings RuntimeStorageSettings{.Buckets = 128, .BucketSizeBytes = (1<<19)};


template<TPackedTupleStorageSettings Settings>
class TSpilledPackedTupleStorage {
    std::optional<int> FindInMemoryBucketWithMostPages() const {
        std::optional<int> resIndex;
        for(int index = 0; index < std::ssize(Buckets_); ++index) {
            if (!Buckets_[index].IsSpilled() && !Buckets_[index].InMemoryPages.empty()) {
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
    TSpilledPackedTupleStorage(ISpiller::TPtr spiller, const NPackedTuple::TTupleLayout* layout)
    : Spiller_(spiller)
    , Layout_(layout) 
    {}
    // template<typename MemoryIntensiveOp> 
    auto MakeSpillUntilPoint(auto memoryIntensiveOp, std::predicate auto dontHaveEnoughMemoryPred) {
        using SpillResult = std::optional<std::invoke_result_t<decltype(memoryIntensiveOp)>>;
        using vtype = SpillResult::value_type;
        return [=] () -> SpillResult {
            EFetchResult res = this->SpillWhile(dontHaveEnoughMemoryPred());
            if (res ==EFetchResult::Yield){
                return std::nullopt;
            } else {
                // if con
                return std::make_optional<SpillResult::value_type>(memoryIntensiveOp());
            }
        };

    }
    void AddRow(TSingleTuple tuple) {
        ui32 hash = NPackedTuple::Hash(tuple.PackedData);
        MKQL_ENSURE(std::popcount(Buckets_.size() - 1) == std::bit_width(std::size(Buckets_)) - 1, "size of buckets should be power of two");
        int bucketIndex = hash & (std::size(Buckets_) - 1);
        TBucket& thisBucket = Buckets_[bucketIndex];
        AppendTupleTo(Layout_, tuple, thisBucket.BuildingPage);
        if (thisBucket.BuildingPage.AllocatedBytes() > Settings.BucketSizeBytes) {
            thisBucket.InMemoryPages.push_back(thisBucket.DetatchBuildingPage());
            thisBucket.InMemoryPages.back().PackedTuples.shrink_to_fit();
            thisBucket.InMemoryPages.back().Overflow.shrink_to_fit();
        }
        // return bucketIndex;
    }
    [[nodiscard]] EFetchResult SpillWhile(std::predicate auto condition)  {
        while(condition()) {
            if (SpillingPages_.has_value()) {
                for(auto& future: *SpillingPages_) {
                    if (!future.BlobId.IsReady()){
                        return Wait();
                    }
                }
                for(auto& future: *SpillingPages_) {
                    MKQL_ENSURE(future.BlobId.IsReady(), "no blocking wait");
                    MKQL_ENSURE(Buckets_[future.BucketIndex].IsSpilled(), "spilled page from in memory bucket?");
                    Buckets_[future.BucketIndex].SpilledPages->push_back(future.BlobId.ExtractValueSync());
                }
                SpillingPages_ = std::nullopt;
            } else {
                constexpr int kPagesSpillingAtTime = 3;
                while (std::accumulate(Buckets_.begin(), Buckets_.end(), 0, [&](int pages, const TBucket& bucket) {
                    return pages + bucket.IsSpilled() ? std::ssize(bucket.InMemoryPages) : 0;
                }) < kPagesSpillingAtTime) {
                    std::optional<int> bucketIndex = FindInMemoryBucketWithMostPages();
                    if (!bucketIndex) {
                        MKQL_ENSURE(false, "unimplemented"); // we can not spill much and do not have memory. spilling smaller chunks is not implemented currently. 
                    }
                    Buckets_[*bucketIndex].SpilledPages.emplace();
                }
                SpillingPages_.emplace();
                // TMKQLVector<BlobIdAndBucketIndex> spilledPages;
                int totalSpillingPages = kPagesSpillingAtTime;
                for(int index = 0; index < std::ssize(Buckets_); ++index) {
                    auto& bucket = Buckets_[index];
                    while (bucket.IsSpilled() && !bucket.InMemoryPages.empty() && totalSpillingPages != 0) {
                        totalSpillingPages--;
                        SpillingPages_->push_back({.BlobId = Spiller_->Put(Serialize(std::move(bucket.InMemoryPages.back()))), .BucketIndex = index});
                        bucket.InMemoryPages.pop_back();
                    }
                }
                MKQL_ENSURE(totalSpillingPages == 0, "not enough pages for spilling?");
            }
        }
        MKQL_ENSURE(!condition(), "sanitiy check");
    }
    TBuckets GetBuckets() {
        MKQL_ENSURE(!SpillingPages_.has_value(), "spilling process should stop when extracting buckets");
        return std::move(Buckets_);
    }
    TBuckets Buckets_;
    ISpiller::TPtr Spiller_;
    std::optional<TMKQLVector<BlobIdAndBucketIndex>> SpillingPages_;
    const NPackedTuple::TTupleLayout* Layout_;
};
}