#pragma once
#include <utility>
// #include hash_join_utils/layout_converter_common.h"
#include <ydb/library/yql/dq/comp_nodes/dq_hash_join_table.h>
#include <ydb/library/yql/dq/comp_nodes/hash_join_utils/layout_converter_common.h>
#include <ydb/library/yql/dq/comp_nodes/join/dq_join_state.h>
#include <ydb/library/yql/dq/comp_nodes/join/alloc_utils.h>
#include <ydb/library/yql/dq/comp_nodes/join/spilled_packed_tuple_storage.h>
#include <ydb/library/yql/dq/comp_nodes/type_utils.h>

namespace NKikimr::NMiniKQL {


template<PackedTupleSource Source, TPackedTupleStorageSettings Settings>
class FetchingBuildSide : public IJoinState {
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
                    return One{.Data = SpillingStorage_->GetBuckets()};
                }
                PackedTuples_ = std::move(GetPayload(rows));
            }
            auto res = SpillingStorage_->SpillWhile([]{
                return !MemoryPercentIsFree(30);
            });
            if (res == EFetchResult::Yield) {
                return Yield{};
            } else {
                for (int64_t tupleBeg = 0; tupleBeg < std::ssize(PackedTuples_->PackedTuples); tupleBeg += Layout_->TotalRowSize) {
                    SpillingStorage_->AddRow({.PackedData = &PackedTuples_->PackedTuples[tupleBeg*Layout_->TotalRowSize], .OverflowBegin = PackedTuples_->Overflow.data()});
                }
                PackedTuples_ = std::nullopt;
            }
            // FetchResult<TArrowRows> rows = FetchBlockAndWaitForEnoughMemory();
        }
        return Finish{};
    }

public:
    NYql::NUdf::EFetchStatus MakeStepAndMutate([[maybe_unused]]TPtr &self) override{
        FetchResult<TBuckets> res = FetchSide();
        // wrong
        MKQL_ENSURE(false, "unimplemented");
        auto& p = GetPayload(res);
        if (p.empty()) {
            MakeSource()
            auto sptr = std::make_unique<StreamingBuildSide<Source>>()
            self = sptr;
        }
        // NJoinTable::TNeumannJoinTable table()
        // auto newState = std::make_shared()  
        switch(AsStatus(res)){

        case NYql::NUdf::EFetchStatus::Ok:
        case NYql::NUdf::EFetchStatus::Finish:
            
        case NYql::NUdf::EFetchStatus::Yield:
          break;
        }
    }

    // JoinState Step(){

    // }
private:
    bool Finished_ = false;
    Source BuildValues_;
    // Source ProbeValues_;
    // BuildSource Values_;
    // ProbeSource ProbeValues_;
    TSpilledPackedTupleStorage<Settings>* SpillingStorage_;
    const NPackedTuple::TTupleLayout* Layout_;
    std::optional<TPackResult> PackedTuples_;
    // std::optional<class Types>

};

class TPackedTupleBucketStorage: public NNonCopyable::TMoveOnly {
public:
    TPackedTupleBucketStorage(Source source, TSpilledPackedTupleStorage<Settings>* packedTupleSpiller): Values_(std::move(source)), SpillingStorage_(packedTupleSpiller) {}

    
    EFetchResult Wait() {
        return EFetchResult::Yield;
    }

private:

};

}