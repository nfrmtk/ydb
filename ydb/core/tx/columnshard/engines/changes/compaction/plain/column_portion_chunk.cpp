#include "column_portion_chunk.h"

#include <ydb/core/formats/arrow/accessor/plain/accessor.h>
#include <ydb/core/tx/columnshard/engines/changes/counters/general.h>
#include <ydb/core/tx/columnshard/engines/storage/chunks/column.h>

#include <ydb/library/formats/arrow/switch/switch_type.h>
#include <ydb/library/formats/arrow/validation/validation.h>

namespace NKikimr::NOlap::NCompaction {

ui32 TColumnPortion::AppendSlice(const std::shared_ptr<arrow::Array>& a, const ui32 startIndex, const ui32 length) {
    Y_ABORT_UNLESS(a);
    Y_ABORT_UNLESS(length);
    Y_ABORT_UNLESS(startIndex + length <= a->length());
    AFL_VERIFY(Type->id() == a->type_id())("own", Type->ToString())("a", a->type()->ToString());
    const ui32 packedRecordSize = Context.GetColumnStat() ? Context.GetColumnStat()->GetPackedRecordSize() : 0;
    // Resolve the concrete Arrow type once for the whole slice instead of paying a SwitchType
    // runtime dispatch per row (the column type is constant, verified above).
    NArrow::SwitchType(a->type_id(), [&](const auto& type) {
        using TWrap = std::decay_t<decltype(type)>;
        using TArray = typename arrow::TypeTraits<typename TWrap::T>::ArrayType;
        using TBuilder = typename arrow::TypeTraits<typename TWrap::T>::BuilderType;

        const auto& typedArray = static_cast<const TArray&>(*a);
        for (ui32 i = startIndex; i < startIndex + length; ++i) {
            // Re-read the builder each row: FlushBuffer() swaps it for a fresh one.
            auto& typedBuilder = static_cast<TBuilder&>(*Builder);
            ui64 recordSize = 0;
            if (typedArray.IsNull(i)) {
                NArrow::TStatusValidator::Validate(typedBuilder.AppendNull());
                recordSize = 4;
            } else {
                const auto view = typedArray.GetView(i);
                NArrow::TStatusValidator::Validate(typedBuilder.Append(view));
                if constexpr (arrow::has_string_view<typename TWrap::T>::value) {
                    recordSize = view.size();
                } else {
                    recordSize = sizeof(view);
                }
            }
            CurrentChunkRawSize += recordSize;
            PredictedPackedBytes += packedRecordSize ? packedRecordSize : (recordSize / 2);
            if (CurrentChunkRawSize >= Context.GetChunkRawBytesLimit() || PredictedPackedBytes >= Context.GetExpectedBlobPackedBytes()) {
                FlushBuffer();
            }
        }
        return true;
    });
    return 0;
}

bool TColumnPortion::FlushBuffer() {
    if (!Builder->length()) {
        return false;
    }
    auto newArrayChunk = NArrow::TStatusValidator::GetValid(Builder->Finish());
    Chunks.emplace_back(std::make_shared<NChunks::TChunkPreparation>(Context.GetSaver().Apply(newArrayChunk, Context.GetResultField()),
        std::make_shared<NArrow::NAccessor::TTrivialArray>(newArrayChunk), TChunkAddress(Context.GetColumnId(), 0), ColumnInfo));
    Builder = Context.MakeBuilder();
    CurrentChunkRawSize = 0;
    PredictedPackedBytes = 0;
    PackedSize += Chunks.back()->GetPackedSize();
    return true;
}

}   // namespace NKikimr::NOlap::NCompaction
