#include <ydb/core/kqp/tools/combiner_perf/streams.h>
#include <ydb/core/kqp/tools/combiner_perf/kqp_setup.h>
#include <ydb/library/yql/dq/comp_nodes/dq_block_hash_join.h>

#include <ydb/library/yql/dq/comp_nodes/ut/utils.h>
namespace NKikimr::NMiniKQL{
    namespace {

        struct Node{
            TType* Type;
            NUdf::TUnboxedValue StreamValues;
            TVector<ui32> KeyColumns;
        };
        struct JoinTestData{
            Node Left;
            Node Right;
            std::optional<Node> Output;
        };
        
        
        auto DoJoinBench(NKikimr::NMiniKQL::TDqSetup<false>& setup, JoinTestData testData ){
            TDqProgramBuilder& pb = setup.GetDqProgramBuilder();
            TRuntimeNode leftList = pb.Arg(testData.Left.Type);
            TRuntimeNode rightList = pb.Arg(testData.Right.Type);
            const TRuntimeNode leftStream = ToWideStream(pb, leftList);
            const TRuntimeNode rightStream = ToWideStream(pb, rightList);

            const TRuntimeNode join = pb.DqBlockHashJoin(leftStream, rightStream, EJoinKind::Inner, testData.Left.KeyColumns, testData.Left.KeyColumns, testData.Left.Type);
            const TRuntimeNode resultNode = FromWideStream(pb, join);
            const auto graph = setup.BuildGraph(resultNode, {leftList.GetNode(), rightList.GetNode()});
            auto& ctx = graph->GetContext();

            graph->GetEntryPoint(0, true)->SetValue(ctx, std::move(testData.Left.StreamValues));
            graph->GetEntryPoint(1, true)->SetValue(ctx, std::move(testData.Right.StreamValues));

            auto graphValue = graph->GetValue();
            // graphValue.Get<typename T>()
            std::vector<NUdf::TUnboxedValue> buff(2, NUdf::TUnboxedValue{});
            size_t lineCount = 0;
            NUdf::EFetchStatus fetchStatus;
            while ((fetchStatus = graphValue.WideFetch(buff.data(),2)) != NUdf::EFetchStatus::Finish) {
                if (fetchStatus == NUdf::EFetchStatus::Ok) {
                    ++lineCount;
                }
            }

            Cout << TStringBuilder() << lineCount << " : total lines in join bench";

        }

        using SamplesStream = NKikimr::NMiniKQL::TKVStream<ui64, ui64, false, 1>;

    }
}
void DoTest(){
    using namespace NKikimr::NMiniKQL;
    NKikimr::NMiniKQL::TDqSetup<false> setup{GetDqNodeFactory()};
    TVector<ui64> leftKeys = {1, 2, 3, 4, 5};
    TVector<TString> leftValues = {"a", "b", "c", "d", "e"};

    TVector<ui64> rightKeys = {2, 3, 4, 6, 7};
    TVector<TString> rightValues = {"x", "y", "z", "u", "v"};

    TVector<ui64> expectedKeys = {1, 2, 3, 4, 5, 2, 3, 4, 6, 7};
    TVector<TString> expectedValues = {"a", "b", "c", "d", "e", "x", "y", "z", "u", "v"};
    JoinTestData data;
    std::tie(data.Left.Type, data.Left.StreamValues) = ConvertVectorsToTuples(setup, leftKeys, leftValues);
    std::tie(data.Right.Type, data.Right.StreamValues) = ConvertVectorsToTuples(setup, rightKeys, rightValues);
    // std::tie(data.Output.Type, data.Output.StreamValues) = ConvertVectorsToTuples(setup, expectedKeys, expectedValues);
    data.Left.KeyColumns.push_back(0);
    data.Right.KeyColumns.push_back(0);
    // data.Output.KeyColumns.push_back(0);
    DoJoinBench(setup, std::move(data));

    // SamplesStream{setup.GetKqpBuilder(), }

}


int main(){
    ::DoTest();
}