#pragma once
#include "type_utils.h"
#include <util/string/printf.h>
#include <yql/essentials/minikql/comp_nodes/mkql_rh_hash.h>

namespace NKikimr::NMiniKQL::NJoinTable {

using TTuple = const NYql::NUdf::TUnboxedValue*;
using TSizedTuple = std::span<const NYql::NUdf::TUnboxedValue>;

template<typename Tuple>
struct TuplesWithSameJoinKey {
    std::vector<Tuple> Tuples;
    bool Used;
};

bool NeedToTrackUnusedRightTuples(EJoinKind kind);

bool NeedToTrackUnusedLeftTuples(EJoinKind kind);

class TStdUVJoinTable: NNonCopyable::TMoveOnly {

  public:
    TStdUVJoinTable(int tupleSize, NKikimr::NMiniKQL::TWideUnboxedEqual eq, NKikimr::NMiniKQL::TWideUnboxedHasher hash,
                  bool trackUnusedTuples)
        : TupleSize(tupleSize)
        , TrackUnusedTuples(trackUnusedTuples)
        , BuiltTable_(1, hash, eq)
    {}

    void Add(TSizedTuple tuple) {
        MKQL_ENSURE(BuiltTable_.empty(), "JoinTable is built already");
        MKQL_ENSURE(std::ssize(tuple) == TupleSize, Sprintf("tuple size promise(%i) vs actual(%i) mismatch", TupleSize, std::ssize(tuple)));
        for (int idx = 0; idx < TupleSize; ++idx) {
            Tuples.push_back(tuple[idx]);
        }
    }

    void Build() {
        MKQL_ENSURE(BuiltTable_.empty(), "JoinTable is built already");
        for (int index = 0; index < std::ssize(Tuples); index += TupleSize) {
            TTuple thisTuple = &Tuples[index];
            auto [it, ok] = BuiltTable_.emplace(
                thisTuple, TuplesWithSameJoinKey{.Tuples = std::vector{thisTuple}, .Used = !TrackUnusedTuples});
            if (!ok) {
                it->second.Tuples.emplace_back(thisTuple);
            }
        }
    }

    void Lookup(TTuple key, std::invocable<TTuple> auto produce) {
        auto it = BuiltTable_.find(key);
        if (it != BuiltTable_.end()) {
            it->second.Used = true;
            std::ranges::for_each(it->second.Tuples, produce);
        }
    }

    bool UnusedTrackingOn() const {
        return TrackUnusedTuples;
    }

    const auto& MapView() const {
        return BuiltTable_;
    }

    void ForEachUnused(std::function<void(TTuple)> produce) {
        MKQL_ENSURE(TrackUnusedTuples, "wasn't tracking tuples at all");
        for (auto& tuplesSameKey : BuiltTable_) {
            if (!tuplesSameKey.second.Used) {
                std::ranges::for_each(tuplesSameKey.second.Tuples, produce);
                tuplesSameKey.second.Used = true;
            }
        }
    }

  private:
    const int TupleSize;
    const bool TrackUnusedTuples;
    std::vector<NYql::NUdf::TUnboxedValue> Tuples;
    std::unordered_map<TTuple, TuplesWithSameJoinKey<TTuple>, NKikimr::NMiniKQL::TWideUnboxedHasher,
                       NKikimr::NMiniKQL::TWideUnboxedEqual>
        BuiltTable_;
};

template<typename Layout>
class TStdInnerFormatJoinTable: NNonCopyable::TMoveOnly {
    using EqType = NKikimr::NMiniKQL::TTupleEqual<Layout>;

    void Lookup(TTupleAndOverflow key, std::invocable<TTupleAndOverflow> auto produce){
        auto it = Table_.find(key);
        if (it != Table_.end()) {
            it->second.Used = true;
            std::ranges::for_each(it->second.Tuples, produce);
        }

    }
    TStdInnerFormatJoinTable(TLayoutAndData<Layout> data)
    : FlatData_(std::move(data))
    , Table_(1, NKikimr::NMiniKQL::TTupleHash{}, EqType{&FlatData_}) {
        const int tupleSize = FlatData_.Layout.TotalRowSize;

        MKQL_ENSURE(FlatData_.Tuples.PackedTuples.size() % tupleSize == 0, "packed tuples contain something else from fixed size tuples?");
        MKQL_ENSURE(tupleSize*FlatData_.Tuples.NTuples == FlatData_.Tuples.PackedTuples.size(), "packed tuples contain something else from fixed size tuples?");
        for(int index = 0; index < FlatData_.Tuples.NTuples; ++index) {
            auto val = TTupleAndOverflow{&FlatData_.Tuples[index*tupleSize], &FlatData_.Tuples.Overflow.front()};
            auto [it, ok] = Table_.emplace(val, {});
            it->second.push_back(val);
        }
    }
    TLayoutAndData<Layout> FlatData_;
    std::unordered_map<TTupleAndOverflow, TuplesWithSameJoinKey<TTupleAndOverflow>, NKikimr::NMiniKQL::TTupleHash,
                       NKikimr::NMiniKQL::TTupleEqual<Layout>> Table_;
};

} // namespace NKikimr::NMiniKQL::NJoinTable