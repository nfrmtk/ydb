#include "benchmark_params.h"
using namespace NKikimr::NMiniKQL;
using namespace NKikimr::NMiniKQL::NJoinBenchmarks;

TString CaseName(::ETestedJoinAlgo algo, NKikimr::NMiniKQL::ETestedJoinKeyType keyType, const TBenchmarkParams::TPreset& preset, TBenchmarkParams::TTableSizes size){
    std::optional<TString> algoName = FindName(algo, kAlgoNames);
    std::optional<TString> keyTypeName = FindName(keyType, kKeyTypeNames);
    Y_ABORT_UNLESS(algoName && keyTypeName);
    return *algoName + "_" + *keyTypeName + "_" + preset.PresetName + "_" + std::to_string(size.Left) + "_" + std::to_string(size.Right);
}