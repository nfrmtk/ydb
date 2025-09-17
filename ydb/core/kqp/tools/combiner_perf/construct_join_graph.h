#pragma once
#include <ydb/library/yql/dq/comp_nodes/dq_program_builder.h>
#include <ydb/library/yql/dq/comp_nodes/ut/utils/dq_setup.h>

namespace NKikimr::NMiniKQL {
enum class ETestedJoinAlgo { kScalarGrace, kScalarMap, kBlockMap, kBlockHash, kScalarHash };

struct TJoinSourceData {
    TArrayRef<TType* const> ColumnTypes;
    TArrayRef<const ui32> KeyColumnIndexes;
    NYql::NUdf::TUnboxedValue ValuesList;
};

struct TInnerJoinDescription {
    TJoinSourceData LeftSource;
    TJoinSourceData RightSource;
    TDqSetup<false>* Setup;
};

bool IsBlockJoin(ETestedJoinAlgo algo);

struct IJoinBenchHelper {
    virtual TVector<NYql::NUdf::TUnboxedValue> JoinedValues() const = 0;
    virtual int64_t RowCountFast() const = 0;
};


std::unique_ptr<IJoinBenchHelper> Construct(ETestedJoinAlgo algo, TInnerJoinDescription descr);


struct TConstructedJoinGraph {
    THolder<IComputationGraph> WideStream;
    THolder<IComputationGraph> RawJoin;
    int64_t(*SkipJoinValuesFn)(NYql::NUdf::TUnboxedValue);
};

TConstructedJoinGraph ConstructInnerJoinGraphStream(ETestedJoinAlgo algo, TInnerJoinDescription descr);

i32 ResultColumnCount(ETestedJoinAlgo algo, TInnerJoinDescription descr);
} // namespace NKikimr::NMiniKQL