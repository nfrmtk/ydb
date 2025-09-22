#pragma once
#include <util/generic/vector.h>
#include <util/generic/set.h>
#include <util/generic/string.h>
namespace NKikimr::NMiniKQL {


enum class ETestedJoinAlgo { kScalarGrace, kScalarMap, kBlockMap, kBlockHash, kScalarHash };
enum class ETestedJoinKeyType { kString, kInteger };

template<typename Enum>
using EnumNames = TVector<std::pair<Enum, TString>>;

template<typename Enum>
std::optional<TString> FindName(Enum value, const EnumNames<Enum>& data){
    auto it = std::ranges::find(data, value, [](const auto& enum_name){return enum_name.first;});
    if (it == data.end()){
        return std::nullopt;
    }else{
        return it->second;
    }
}
template<typename Enum>
std::optional<Enum> FindValue(const TString& name, const EnumNames<Enum>& data){
    auto it = std::ranges::find(data, name, [](const auto& enum_name){return enum_name.second;});
    if (it == data.end()){
        return std::nullopt;
    }else{
        return it->first;
    }
}

static const TVector<std::pair<ETestedJoinAlgo, TString>> kAlgoNames{
        {ETestedJoinAlgo::kScalarGrace, "ScalarGrace"},
        {ETestedJoinAlgo::kScalarMap, "ScalarMap"},
        {ETestedJoinAlgo::kBlockMap, "BlockMap"},
        {ETestedJoinAlgo::kScalarHash, "ScalarHash"}, // hash joins are not ready yet - they require
        {ETestedJoinAlgo::kBlockHash, "BlockHash"}, // same schema for left and right tables as they
        //         just spit left then right
};
static const TVector<std::pair<ETestedJoinKeyType, TString>> kKeyTypeNames = {
        {ETestedJoinKeyType::kString, "String"},
        {ETestedJoinKeyType::kInteger, "Integer"},
};




namespace NJoinBenchmarks{
struct TBenchmarkParams{
    struct TTableSizes{
        int Left;
        int Right;
    };
    struct TPreset{
        TVector<TTableSizes> Cases;
        TString PresetName;
    };
    TVector<TPreset> Presets;
    TSet<ETestedJoinKeyType> KeyTypes;
    TSet<ETestedJoinAlgo> Algorithms;
};
TString CaseName(ETestedJoinAlgo algo, ETestedJoinKeyType keyType, const TBenchmarkParams::TPreset& preset, TBenchmarkParams::TTableSizes size);
}

}

