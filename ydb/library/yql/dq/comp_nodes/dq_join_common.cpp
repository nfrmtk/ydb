#include "dq_join_common.h"
#include <yql/essentials/minikql/mkql_node_cast.h>

namespace NKikimr::NMiniKQL {

TKeyTypes KeyTypesFromColumns(const std::vector<TType*>& types, const std::vector<ui32>& keyIndexes) {
    TKeyTypes kt;
    for (auto typeIndex : keyIndexes) {
        const TType* type = types[typeIndex];
        MKQL_ENSURE(type->IsData(), "exepected data type");
        kt.push_back(std::pair{*AS_TYPE(TDataType, type)->GetDataSlot(), false});
    }
    return kt;
}

IBlockLayoutConverter::TPackResult Flatten(std::vector<IBlockLayoutConverter::TPackResult> tuples, const NPackedTuple::TTupleLayout* layout) {
    IBlockLayoutConverter::TPackResult flattened;
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

    int tupleSize = layout->TotalRowSize;
    for (const IBlockLayoutConverter::TPackResult& tupleBatch : tuples) {
        layout->Concat(flattened.PackedTuples, flattened.Overflow,
                            std::ssize(flattened.PackedTuples) / tupleSize, tupleBatch.PackedTuples.data(),
                            tupleBatch.Overflow.data(), tupleBatch.PackedTuples.size() / tupleSize,
                            tupleBatch.Overflow.size());
    }
    return flattened;
}


} // namespace NKikimr::NMiniKQL