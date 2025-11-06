#include "dq_join_state.h"  
namespace NKikimr::NMiniKQL{

NYql::NUdf::EFetchStatus JoinState::Step(){
    return State->MakeStepAndMutate(State);
}
}