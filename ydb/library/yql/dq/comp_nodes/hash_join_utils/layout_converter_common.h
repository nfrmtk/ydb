#pragma once

#include <ydb/library/yql/dq/comp_nodes/hash_join_utils/tuple.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>

namespace NKikimr::NMiniKQL {

struct TSingleTuple {
    const ui8* PackedData;
    const ui8* OverflowBegin;
};


// Common types used by both IBlockLayoutConverter and IScalarLayoutConverter
struct TPackResult {
    std::vector<ui8, TMKQLAllocator<ui8>> PackedTuples;
    std::vector<ui8, TMKQLAllocator<ui8>> Overflow;
    int64_t NTuples{0};
    int64_t AllocatedBytes() const;
    void AppendTuple(TSingleTuple tuple, const NPackedTuple::TTupleLayout* layout);
};



TPackResult Flatten(TMKQLVector<TPackResult> tuples, const NPackedTuple::TTupleLayout* layout);


using TPackedTuple = std::vector<ui8, TMKQLAllocator<ui8>>;
using TOverflow = std::vector<ui8, TMKQLAllocator<ui8>>;

} // namespace NKikimr::NMiniKQL
