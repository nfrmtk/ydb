#pragma once
#include "dq_join_state.h"
namespace NKikimr::NMiniKQL{

template<PackedTupleSource Source>
class StreamingBuildSide : public IJoinState{
public:
    StreamingBuildSide(NJoinTable::TNeumannJoinTable&& table, Source probe)
    NYql::NUdf::EFetchStatus MakeStepAndMutate([[maybe_unused]]TPtr &self) override{
        return NYql::NUdf::EFetchStatus::Finish;
    }



    Source Source 
};
}