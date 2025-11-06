#pragma once
#include <variant>
#include <concepts>
// #include "dq_join_state_fetch_build.h"
#include <ydb/library/yql/dq/comp_nodes/hash_join_utils/layout_converter_common.h>
#include <ydb/library/yql/dq/comp_nodes/type_utils.h>
#include <yql/essentials/public/udf/udf_value.h>
#include <yql/essentials/minikql/computation/mkql_spiller.h>
namespace NKikimr::NMiniKQL{
// struct JoinFinished{};

// class FetchingBuildSide;

// class PartiallyStreamingProbeSide;

// class StreamingProbeSide;

// class StreamingBuckets;

// enum class EStepResult{
//     Finish,
//     Yield
// };

template<typename T>
concept PackedTupleSource = requires (T t) {
    {t.Fetch()} -> std::same_as<FetchResult<TPackResult>>;
    {std::as_const(t).Layout()} -> std::same_as<const NPackedTuple::TTupleLayout*>;
    {std::as_const(t).Finished()} -> std::same_as<bool>;
};

struct IPackedTupleSource {
    using TPtr = std::unique_ptr<IPackedTupleSource>;

    virtual FetchResult<TPackResult> Fetch() = 0;
    virtual const NPackedTuple::TTupleLayout* Layout() const = 0;
    virtual bool Finished() const = 0;

};

enum class EIsInMemory: bool {
    Spilled,
    InMemory,
};


IPackedTupleSource::TPtr MakePackedTupleSource(bool block, NYql::NUdf::TUnboxedValue values, const TMKQLVector<EIsInMemory>& memoryStatus);




struct IJoinState {
    using TPtr = std::unique_ptr<IJoinState>;
    virtual NYql::NUdf::EFetchStatus MakeStepAndMutate([[maybe_unused]]TPtr& self) = 0;
    virtual ~IJoinState() = default;
};


// concept 

class JoinState{
    IJoinState::TPtr State;
    ISpiller::TPtr Spiller;
public:
    NYql::NUdf::EFetchStatus Step();
};
}