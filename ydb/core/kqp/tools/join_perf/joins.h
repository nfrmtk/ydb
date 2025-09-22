#pragma once
#include <ydb/core/kqp/tools/join_perf/benchmark_params.h>
#include <ydb/core/kqp/tools/combiner_perf/printout.h>

namespace NKikimr {
namespace NMiniKQL {

namespace NJoinBenchmarks{

struct TBenchmarkCaseResult{
    TString Name;
    TBenchmarkParams::TTableSizes Size;
};

void RunJoinsBench(const TBenchmarkParams& params, std::function<void(const TBenchmarkCaseResult&)> callback);

}
}
}