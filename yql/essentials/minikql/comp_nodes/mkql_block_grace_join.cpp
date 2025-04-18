#include "mkql_block_grace_join.h"

#include <yql/essentials/minikql/mkql_type_builder.h>
#include <yql/essentials/minikql/computation/mkql_block_builder.h>
#include <yql/essentials/minikql/computation/mkql_block_reader.h>
#include <yql/essentials/minikql/computation/mkql_block_impl.h>
#include <yql/essentials/minikql/computation/block_layout_converter.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders_codegen.h>
#include <yql/essentials/minikql/computation/mkql_resource_meter.h>

#include <yql/essentials/minikql/invoke_builtins/mkql_builtins.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/minikql/mkql_program_builder.h>
#include <yql/essentials/minikql/mkql_block_grace_join_policy.h>

#include <ydb/library/yql/minikql/comp_nodes/packed_tuple/robin_hood_table.h>
#include <ydb/library/yql/minikql/comp_nodes/packed_tuple/neumann_hash_table.h>
#include <ydb/library/yql/minikql/comp_nodes/packed_tuple/cardinality.h>

#include <util/generic/serialized_enum.h>
#include <util/digest/numeric.h>

#include <yql/essentials/public/udf/arrow/util.h>
#include <yql/essentials/public/udf/arrow/block_item_hasher.h>

#include <arrow/array/data.h>
#include <arrow/datum.h>

#include <chrono>

namespace NKikimr::NMiniKQL {

namespace {

using namespace std::chrono_literals;
using THash = ui64;

// -------------------------------------------------------------------
[[maybe_unused]] constexpr size_t KB = 1024;
[[maybe_unused]] constexpr size_t MB = KB * KB;
[[maybe_unused]] constexpr size_t L1_CACHE_SIZE = 32 * KB;
[[maybe_unused]] constexpr size_t L2_CACHE_SIZE = 256 * KB;
[[maybe_unused]] constexpr size_t L3_CACHE_SIZE = 16 * MB;

// -------------------------------------------------------------------
TDefaultBlockGraceJoinPolicy globalDefaultPolicy{};

// -------------------------------------------------------------------
size_t CalcMaxBlockLength(const TVector<TType*>& items, bool isBlockType = true) {
    return CalcBlockLen(std::accumulate(items.cbegin(), items.cend(), 0ULL,
        [isBlockType](size_t max, const TType* type) {
            if (isBlockType) {
                const TType* itemType = AS_TYPE(TBlockType, type)->GetItemType();
                return std::max(max, CalcMaxBlockItemSize(itemType));
            } else {
                return std::max(max, CalcMaxBlockItemSize(type));
            }
        }));
}

THash CalculateTupleHash(const TVector<THash>& hashes) {
    THash hash = 0;
    for (size_t i = 0; i < hashes.size(); i++) {
        if (!hashes[i]) {
            return 0;
        }
        hash = CombineHashes(hash, hashes[i]);
    }
    return hash;
}

// -------------------------------------------------------------------
using TRobinHoodTable = NPackedTuple::TRobinHoodHashBase<true>;
using TNeumannTable = NPackedTuple::TNeumannHashTable<false>;

size_t CalculateExpectedOverflowSize(const NPackedTuple::TTupleLayout* layout, size_t nTuples) {
    size_t varSizedCount = 0;
    for (const auto& column: layout->Columns) {
        if (column.SizeType == NPackedTuple::EColumnSizeType::Variable) {
            varSizedCount++;
        }
    }

    if (varSizedCount == 0) {
        return 0;
    }

    // Some weird heuristic.
    // Lets expect that there will be no more than 10% of var sized values with length
    // bigger than 64 bytes.
    return varSizedCount * nTuples * 64 / 10;
}

// -------------------------------------------------------------------
// Class is used as temporary storage for join algorithm quick start.
// Quick start is stage of hash join algo when join compute node is trying to
// fetch some data from left and right stream and decide what to do: start grace hash join or
// hash join.
// Also this class collects some initial statistics about data, like sizes and cardinality.
class TTempJoinStorage : public TComputationValue<TTempJoinStorage> {
private:
    using TBase = TComputationValue<TTempJoinStorage>;
    using THasherPtr = NYql::NUdf::IBlockItemHasher::TPtr;
    using TReaderPtr = std::unique_ptr<IBlockReader>;

public:
    // Fetched block
    struct TBlock {
        size_t Size; // count of elements in one column
        TVector<arrow::Datum> Columns;

        TBlock() = default;
        TBlock(size_t size, TVector<arrow::Datum>&& columns)
            : Size(size)
            , Columns(std::move(columns))
        {}
    };

    enum class EStatus {
        Unknown,
        OneStreamFinished, // Only one stream is finished
        BothStreamsFinished, // Both streams are finished
        MemoryLimitExceeded // We have to use Grace Hash Join algorithm
    };

public:
    TTempJoinStorage(
        TMemoryUsageInfo*       memInfo,
        const TVector<TType*>&  leftItemTypesArg,
        const TVector<ui32>&    leftKeyColumns,
        NUdf::TUnboxedValue     leftStream,
        const TVector<TType*>&  rightItemTypesArg,
        const TVector<ui32>&    rightKeyColumns,
        NUdf::TUnboxedValue     rightStream,
        IBlockGraceJoinPolicy*  policy,
        arrow::MemoryPool*      pool
    )
        : TBase(memInfo)
        , LeftStream_(leftStream)
        , LeftInputs_(leftItemTypesArg.size())
        , LeftKeyColumns_(leftKeyColumns)
        , RightStream_(rightStream)
        , RightInputs_(rightItemTypesArg.size())
        , RightKeyColumns_(rightKeyColumns)
        , Policy_(policy)
    {
        TBlockTypeHelper helper;
        TVector<TType*> leftItemTypes;
        for (size_t i = 0; i < leftItemTypesArg.size() - 1; i++) { // ignore last column, because this is block size
            TType* blockItemType = AS_TYPE(TBlockType, leftItemTypesArg[i])->GetItemType();
            leftItemTypes.push_back(blockItemType);
            LeftHashers_.push_back(helper.MakeHasher(blockItemType));
            LeftReaders_.push_back(MakeBlockReader(TTypeInfoHelper(), blockItemType));
        }
        TVector<NPackedTuple::EColumnRole> leftRoles(LeftInputs_.size() - 1, NPackedTuple::EColumnRole::Payload);
        for (auto keyCol: leftKeyColumns) {
            leftRoles[keyCol] = NPackedTuple::EColumnRole::Key;
        }
        LeftConverter_ = MakeBlockLayoutConverter(TTypeInfoHelper(), leftItemTypes, leftRoles, pool);

        TVector<TType*> rightItemTypes;
        for (size_t i = 0; i < rightItemTypesArg.size() - 1; i++) { // ignore last column, because this is block size
            TType* blockItemType = AS_TYPE(TBlockType, rightItemTypesArg[i])->GetItemType();
            rightItemTypes.push_back(blockItemType);
            RightHashers_.push_back(helper.MakeHasher(blockItemType));
            RightReaders_.push_back(MakeBlockReader(TTypeInfoHelper(), blockItemType));
        }
        TVector<NPackedTuple::EColumnRole> rightRoles(RightInputs_.size() - 1, NPackedTuple::EColumnRole::Payload);
        for (auto keyCol: rightKeyColumns) {
            rightRoles[keyCol] = NPackedTuple::EColumnRole::Key;
        }
        RightConverter_ = MakeBlockLayoutConverter(TTypeInfoHelper(), rightItemTypes, rightRoles, pool);
    }

    NUdf::EFetchStatus FetchStreams() {
        auto maxFetchedSize = Policy_->GetMaximumInitiallyFetchedData();

        auto resultLeft = NUdf::EFetchStatus::Finish;
        if (!LeftIsFinished_ && LeftEstimatedSize_ < maxFetchedSize) {
            resultLeft = LeftStream_.WideFetch(LeftInputs_.data(), LeftInputs_.size());
            if (resultLeft == NUdf::EFetchStatus::Ok) {
                TBlock leftBlock;
                ExtractBlock(LeftInputs_, leftBlock);
                LeftEstimatedSize_ += EstimateBlockSize(leftBlock, LeftConverter_->GetTupleLayout());
                LeftFetchedTuples_ += leftBlock.Size;
                SampleBlock(leftBlock, LeftKeyColumns_, LeftHashers_, LeftReaders_, LeftSamples_);
                LeftData_.push_back(std::move(leftBlock));
            } else if (resultLeft == NUdf::EFetchStatus::Finish) {
                LeftIsFinished_ = true;
            }
        }
        
        auto resultRight = NUdf::EFetchStatus::Finish;
        if (!RightIsFinished_ && RightEstimatedSize_ < maxFetchedSize) {
            resultRight = RightStream_.WideFetch(RightInputs_.data(), RightInputs_.size());
            if (resultRight == NUdf::EFetchStatus::Ok) {
                TBlock rightBlock;
                ExtractBlock(RightInputs_, rightBlock);
                RightEstimatedSize_ += EstimateBlockSize(rightBlock, RightConverter_->GetTupleLayout());
                RightFetchedTuples_ += rightBlock.Size;
                SampleBlock(rightBlock, RightKeyColumns_, RightHashers_, RightReaders_, RightSamples_);
                RightData_.push_back(std::move(rightBlock));
            } else if (resultRight == NUdf::EFetchStatus::Finish) {
                RightIsFinished_ = true;
            }
        }

        if (resultLeft == NUdf::EFetchStatus::Yield || resultRight == NUdf::EFetchStatus::Yield) {
            return NUdf::EFetchStatus::Yield;
        }
        return NUdf::EFetchStatus::Finish; // Finish here doesn't mean that there is nothing to fetch anymore
    }

    EStatus GetStatus() {
        auto maxFetchedSize = Policy_->GetMaximumInitiallyFetchedData();

        if (LeftIsFinished_ && RightIsFinished_) {
            return EStatus::BothStreamsFinished;
        }
        if ((LeftIsFinished_ && RightEstimatedSize_ > maxFetchedSize) ||
            (LeftEstimatedSize_ > maxFetchedSize && RightIsFinished_)) {
          return EStatus::OneStreamFinished;
        }
        if (LeftEstimatedSize_ > maxFetchedSize && RightEstimatedSize_ > maxFetchedSize) {
            return EStatus::MemoryLimitExceeded;
        }
        return EStatus::Unknown;
    }

    std::pair<size_t, size_t> GetFetchedTuples() const {
        return {LeftFetchedTuples_, RightFetchedTuples_};
    }

    std::pair<size_t, size_t> GetPayloadSizes() const {
        return {
            LeftConverter_->GetTupleLayout()->PayloadSize,
            RightConverter_->GetTupleLayout()->PayloadSize};
    }

    // This estimation is rough and depends on selectivity, so use it as a bootstrap
    ui64 EstimateCardinality() const {
        using std::max;

        // TODO: change this values to stream sizes given from optimizer
        auto [lTuples, rTuples] = GetFetchedTuples();
        // Another weird heuristic to get number of buckets for cardinality estimation
        auto buckets = max<ui64>(max<ui64>(lTuples, rTuples) / 2000, 1); // 1/20 (5%) * 1/100 (step) -> 1/2000
        NPackedTuple::CardinalityEstimator estimator{buckets};
        return estimator.Estimate(lTuples, LeftSamples_, rTuples, RightSamples_);
    }

    std::pair<bool, bool> IsFinished() const {
        return {LeftIsFinished_, RightIsFinished_};
    }

    // After the method is called FetchStreams cannot be called anymore
    std::pair<TDeque<TBlock>, TDeque<TBlock>> DetachData() {
        return {std::move(LeftData_), std::move(RightData_)};
    }

private:
    // Extract block from TUnboxedValueVector
    void ExtractBlock(const TUnboxedValueVector& input, TBlock& block) {
        TVector<arrow::Datum> blockColumns;
        for (size_t i = 0; i < input.size() - 1; i++) {
            auto& datum = TArrowBlock::From(input[i]).GetDatum();
            blockColumns.push_back(datum.array());
        }
        auto blockSize = ::GetBlockCount(input[input.size() - 1]);
        block.Size = blockSize;
        block.Columns = std::move(blockColumns);
    }

    // Calculate block size in tuple layout to estimate memory consumption for hash table
    size_t EstimateBlockSize(const TBlock& block, const NPackedTuple::TTupleLayout* layout) {
        return block.Size * layout->TotalRowSize;
    }

    // Make and save hashes of given block samples to estimate Join cardinality
    // Step should be large enough to not affect performance
    void SampleBlock(
        const TBlock& block, const TVector<ui32>& keyColumns,
        TVector<THasherPtr>& hashers, TVector<TReaderPtr>& readers,
        TVector<THash>& samples, size_t step = 100)
    {
        TVector<THash> hashes(keyColumns.size());
        for (size_t i = 0; i < block.Size; i += step) {
            for (size_t j = 0; j < keyColumns.size(); ++j) {
                auto col = keyColumns[j];
                const auto& reader = readers[col];
                const auto& hasher = hashers[col];
                const auto& array = block.Columns[col].array();
                hashes[j] = hasher->Hash(reader->GetItem(*array, i));
            }
            auto keyHash = CalculateTupleHash(hashes);
            samples.push_back(keyHash);
        }
    }

private:
    NUdf::TUnboxedValue LeftStream_;
    TUnboxedValueVector LeftInputs_;
    TVector<ui32>       LeftKeyColumns_;
    TDeque<TBlock>      LeftData_;
    size_t              LeftFetchedTuples_{0}; // count of fetched tuples
    size_t              LeftEstimatedSize_{0}; // size in tuple layout represenation
    bool                LeftIsFinished_{false};
    IBlockLayoutConverter::TPtr LeftConverter_; // Converters here are used only for size estimation via info in TupleLayout class
    TVector<THash>      LeftSamples_; // Samples for cardinality estimation
    TVector<THasherPtr> LeftHashers_; // Hashers to calculate hash of block's key items
    TVector<TReaderPtr> LeftReaders_; // Readers to read blcok's keu items

    NUdf::TUnboxedValue RightStream_;
    TUnboxedValueVector RightInputs_;
    TVector<ui32>       RightKeyColumns_;
    TDeque<TBlock>      RightData_;
    size_t              RightFetchedTuples_{0}; // count of fetched tuples
    size_t              RightEstimatedSize_{0}; // size in tuple layout represenation
    bool                RightIsFinished_{false};
    IBlockLayoutConverter::TPtr RightConverter_;
    TVector<THash>      RightSamples_;
    TVector<THasherPtr> RightHashers_;
    TVector<TReaderPtr> RightReaders_;

    IBlockGraceJoinPolicy*  Policy_;
};

// -------------------------------------------------------------------
// This is storage for payload columns used when payload part of a tuple is big.
// So we don't want to carry this useless data during conversion and join algorithm.
// This storage can save some block and restore payload by index array.
class TExternalPayloadStorage : public TComputationValue<TExternalPayloadStorage> {
    private:
        using TBase = TComputationValue<TExternalPayloadStorage>;
        using TBlock = TTempJoinStorage::TBlock;

    public:
        TExternalPayloadStorage(
            TMemoryUsageInfo*       memInfo,
            TComputationContext&    ctx,
            const TVector<TType*>&  payloadItemTypes,
            bool                    nonClearable = false // if true Clear() method will do nothing. Used for build-stream storage
        )
            : TBase(memInfo)
            , NonClearable_(nonClearable)
        {
            const auto& pgBuilder = ctx.Builder->GetPgBuilder();
            // WARNING: we can not properly track the number of output rows due to uninterruptible for loop in DoBatchLookup,
            // so add some heuristic to prevent overflow in AddMany builder's method.
            auto maxBlockLen = CalcMaxBlockLength(payloadItemTypes, false) * 2;

            for (size_t i = 0; i < payloadItemTypes.size(); i++) {
                Readers_.push_back(MakeBlockReader(TTypeInfoHelper(), payloadItemTypes[i]));
                // FIXME: monitor amount of allocated memory like in BlockMapJoin to prevent overflow
                Builders_.push_back(MakeArrayBuilder(
                    TTypeInfoHelper(), payloadItemTypes[i], ctx.ArrowMemoryPool, maxBlockLen, &pgBuilder));
            }

            // Init indirection indexes datum only once
            auto ui64Type = ctx.TypeEnv.GetUi64Lazy();
            auto maxBufferSize = CalcBlockLen(CalcMaxBlockItemSize(ui64Type));
            std::shared_ptr<arrow::DataType> type;
            ConvertArrowType(ui64Type, type);
            std::shared_ptr<arrow::Buffer> nullBitmap;
            auto dataBuffer = NUdf::AllocateResizableBuffer(sizeof(ui64) * maxBufferSize, &ctx.ArrowMemoryPool);
            IndirectionIndexes = arrow::ArrayData::Make(std::move(type), maxBufferSize, {std::move(nullBitmap), std::move(dataBuffer)});
        }

        ui32 Size() const {
            return PayloadColumnsStorage_.size();
        }

        void AddBlock(TBlock&& block) {
            PayloadColumnsStorage_.push_back(std::move(block));
        }

        void Clear() {
            if (NonClearable_) {
                return;
            }
            PayloadColumnsStorage_.clear();
        }

        TVector<arrow::Datum> RestorePayload(const arrow::Datum& indexes, ui32 length) {
            auto rawIndexes = indexes.array()->GetMutableValues<ui64>(1);

            TVector<arrow::Datum> result;
            for (size_t i = 0; i < Builders_.size(); ++i) {
                auto& builder = Builders_[i];
                auto& reader = Readers_[i];

                for (size_t j = 0; j < length; ++j) {
                    auto blockIndex = static_cast<ui32>(rawIndexes[j] >> 32);
                    auto elemIndex = static_cast<ui32>(rawIndexes[j] & 0xFFFFFFFF);

                    const auto& array = PayloadColumnsStorage_[blockIndex].Columns[i].array();
                    builder->Add(reader->GetItem(*array, elemIndex));
                }

                result.push_back(builder->Build(false));
            }

            return result;
        }

        // Split block on two blocks
        // Lhs contains all key columns and indirection index, rhs contains all payload columns
        static std::pair<TBlock, TBlock> SplitBlock(
            const TBlock& block, TExternalPayloadStorage& payloadStorage, const THashSet<ui32>& keyColumnsSet)
        {
            TBlock keyBlock;
            TBlock payloadBlock;
            for (size_t i = 0; i < block.Columns.size(); ++i) {
                const auto& datum = block.Columns[i];
                if (keyColumnsSet.contains(i)) {
                    keyBlock.Columns.push_back(datum.array());
                } else {
                    payloadBlock.Columns.push_back(datum.array());
                }
            }
            keyBlock.Size = block.Size;
            payloadBlock.Size = block.Size;

            // Init index column
            auto* rawDataBuffer = payloadStorage.IndirectionIndexes.array()->GetMutableValues<ui64>(1);
            ui32 blockIndex = payloadStorage.Size();
            for (size_t i = 0; i < keyBlock.Size; ++i) {
                rawDataBuffer[i] = (static_cast<ui64>(blockIndex) << 32) | i; // indirected index column has such layout: 32 higher bits for block number and 32 bits for offset in block
            }
            payloadStorage.IndirectionIndexes.array()->length = keyBlock.Size;

            // Add index column to fetched key block
            keyBlock.Columns.push_back(payloadStorage.IndirectionIndexes);

            return {std::move(keyBlock), std::move(payloadBlock)};
        }

    public:
        arrow::Datum IndirectionIndexes;

    private:
        TVector<TBlock> PayloadColumnsStorage_;
        TVector<std::unique_ptr<IBlockReader>> Readers_;
        TVector<std::unique_ptr<IArrayBuilder>> Builders_;
        bool NonClearable_;
    };

// -------------------------------------------------------------------
// State of joined output.
struct TJoinState : public TBlockState {
public:
    TJoinState(
        TMemoryUsageInfo*           memInfo,
        const TVector<TType*>*      resultItemTypes,
        IBlockLayoutConverter*      buildConverter,
        IBlockLayoutConverter*      probeConverter,
        const TVector<ui32>&        leftIOMap,
        const TVector<ui32>&        rightIOMap,
        TExternalPayloadStorage*    buildPayloadStorage, // can be nullptr
        TExternalPayloadStorage*    probePayloadStorage, // can be nullptr
        bool                        wasSwapped
    )
        : TBlockState(memInfo, resultItemTypes->size())
        , MaxLength_(CalcMaxBlockLength(*resultItemTypes))
        , WasSwapped_(wasSwapped)
        , LeftIOMap_(leftIOMap)
        , RightIOMap_(rightIOMap)
    {
        LeftPackedTuple_ = &BuildPackedOutput;
        LeftOverflow_ = &BuildPackedInput.Overflow;
        LeftConverter_ = buildConverter;
        LeftPayloadStorage_ = buildPayloadStorage;

        RightPackedTuple_ = &ProbePackedOutput;
        RightOverflow_ = &ProbePackedInput.Overflow;
        RightConverter_ = probeConverter;
        RightPayloadStorage_ = probePayloadStorage;

        // Check if was swapped.
        // If was not swapped, left stream is build and right is probe
        if (wasSwapped) {
            using std::swap;
            swap(LeftPackedTuple_, RightPackedTuple_);
            swap(LeftOverflow_, RightOverflow_);
            swap(LeftConverter_, RightConverter_);
            swap(LeftPayloadStorage_, RightPayloadStorage_);
        }
    }

    bool GetSwapped() const {
        return WasSwapped_;
    }

    void SetSwapped(bool wasSwapped) {
        if (wasSwapped != WasSwapped_) {
            using std::swap;
            swap(LeftPackedTuple_, RightPackedTuple_);
            swap(LeftOverflow_, RightOverflow_);
            WasSwapped_ = wasSwapped;
        }
    }

    void MakeBlocks(const THolderFactory& holderFactory) {
        Values.back() = holderFactory.CreateArrowBlock(arrow::Datum(std::make_shared<arrow::UInt64Scalar>(OutputRows)));

        size_t index = 0;
        IBlockLayoutConverter::PackResult leftPackResult{std::move(*LeftPackedTuple_), std::move(*LeftOverflow_), OutputRows};
        TVector<arrow::Datum> leftColumns;
        LeftConverter_->Unpack(leftPackResult, leftColumns);
        if (LeftPayloadStorage_) {
            auto payload = LeftPayloadStorage_->RestorePayload(leftColumns.back(), OutputRows);
            leftColumns.pop_back();
            leftColumns.insert(leftColumns.end(), payload.begin(), payload.end());
        }
        for (size_t i = 0; i < LeftIOMap_.size(); i++, index++) {
            Values[index] = holderFactory.CreateArrowBlock(std::move(leftColumns[LeftIOMap_[i]]));
        }

        IBlockLayoutConverter::PackResult rightPackResult{std::move(*RightPackedTuple_), std::move(*RightOverflow_), OutputRows};
        TVector<arrow::Datum> rightColumns;
        RightConverter_->Unpack(rightPackResult, rightColumns);
        if (RightPayloadStorage_) {
            auto payload = RightPayloadStorage_->RestorePayload(rightColumns.back(), OutputRows);
            rightColumns.pop_back();
            rightColumns.insert(rightColumns.end(), payload.begin(), payload.end());
        }
        for (size_t i = 0; i < RightIOMap_.size(); i++, index++) {
            Values[index] = holderFactory.CreateArrowBlock(std::move(rightColumns[RightIOMap_[i]]));
        }

        FillArrays();
        // Move values back from packed view
        *LeftPackedTuple_ = std::move(leftPackResult.PackedTuples);
        *LeftOverflow_ = std::move(leftPackResult.Overflow);
        *RightPackedTuple_ = std::move(rightPackResult.PackedTuples);
        *RightOverflow_ = std::move(rightPackResult.Overflow);
    }

    bool IsNotFull() const {
        // WARNING: we can not properly track the number of output rows due to uninterruptible for loop in DoBatchLookup,
        // so add some heuristic to prevent overflow in AddMany builder's method.
        return OutputRows * 5 < MaxLength_ * 4;
    }

    bool HasEnoughMemory() const {
        return ProbePackedInput.Overflow.capacity() == 0 ||
               ProbePackedInput.Overflow.size() * 5 <
                   ProbePackedInput.Overflow.capacity() * 4;
    }

    bool HasBlocks() const {
        return Count > 0;
    }

    void ResetInput() {
        ProbePackedInput.PackedTuples.clear();
        ProbePackedInput.Overflow.clear();
        ProbePackedInput.NTuples = 0;
        // Do not clear build input, because it is constant for all DoProbe calls
        if (LeftPayloadStorage_) {
            LeftPayloadStorage_->Clear();
        }
        if (RightPayloadStorage_) {
            RightPayloadStorage_->Clear();
        }
    }

    void ResetOutput() {
        OutputRows = 0;
        BuildPackedOutput.clear();
        ProbePackedOutput.clear();
    }

public:
    IBlockLayoutConverter::PackResult BuildPackedInput;   // converted data right after fetch
    IBlockLayoutConverter::PackResult ProbePackedInput;

    IBlockLayoutConverter::TPackedTuple BuildPackedOutput;   // packed output after join operation
    IBlockLayoutConverter::TPackedTuple ProbePackedOutput;

    ui32 OutputRows{0};

private:
    ui32 MaxLength_{0};
    bool WasSwapped_;

    IBlockLayoutConverter*                  LeftConverter_;
    IBlockLayoutConverter::TPackedTuple*    LeftPackedTuple_;
    IBlockLayoutConverter::TOverflow*       LeftOverflow_;
    const TVector<ui32>&                    LeftIOMap_;
    TExternalPayloadStorage*                LeftPayloadStorage_; // can be nullptr

    IBlockLayoutConverter*                  RightConverter_;
    IBlockLayoutConverter::TPackedTuple*    RightPackedTuple_;
    IBlockLayoutConverter::TOverflow*       RightOverflow_;
    const TVector<ui32>&                    RightIOMap_;
    TExternalPayloadStorage*                RightPayloadStorage_; // can be nullptr

    NUdf::TUnboxedValue Table_; // Hash table for smaller stream
};

// -------------------------------------------------------------------
class THashJoin : public TComputationValue<THashJoin> {
private:
    using TBase = TComputationValue<THashJoin>;
    using TBlock = TTempJoinStorage::TBlock;
    using TTable = TNeumannTable; // According to benchmarks it is always better to use Neumann HT in HashJoin, due to small build size

public:
    THashJoin(
        TMemoryUsageInfo*       memInfo,
        TComputationContext&    ctx,
        const char *            joinName,
        const TVector<TType*>*  resultItemTypes,
        NUdf::TUnboxedValue*    leftStream,
        const TVector<TType*>*  leftItemTypesArg,
        const TVector<ui32>*    leftKeyColumns,
        const TVector<ui32>&    leftIOMap,
        NUdf::TUnboxedValue*    rightStream,
        const TVector<TType*>*  rightItemTypesArg,
        const TVector<ui32>*    rightKeyColumns,
        const TVector<ui32>&    rightIOMap,
        IBlockGraceJoinPolicy*  policy,
        NUdf::TUnboxedValue     tempStorageValue
    )
        : TBase(memInfo)
        , Ctx_(ctx)
        , JoinName_(joinName)
        , ResultItemTypes_(resultItemTypes)
    {
        using EJoinAlgo = IBlockGraceJoinPolicy::EJoinAlgo;

        auto& tempStorage = *static_cast<TTempJoinStorage*>(tempStorageValue.AsBoxed().Get());
        auto [leftFetchedTuples, rightFetchedTuples] = tempStorage.GetFetchedTuples();
        auto [leftPSz, rightPSz] = tempStorage.GetPayloadSizes();
        auto cardinality = tempStorage.EstimateCardinality(); // bootstrap value, may be far from truth
        auto [isLeftFinished, isRightFinished] = tempStorage.IsFinished();
        auto [leftData, rightData] = tempStorage.DetachData();
        bool wasSwapped = false;
        // assume that finished stream has less size than unfinished
        if ((!isLeftFinished && isRightFinished) ||
            (isLeftFinished && isRightFinished && (leftFetchedTuples > rightFetchedTuples)))
        {
            using std::swap;
            swap(leftStream, rightStream); // so swap them
            swap(leftData, rightData);
            swap(leftItemTypesArg, rightItemTypesArg);
            swap(leftKeyColumns, rightKeyColumns);
            swap(leftPSz, rightPSz);
            wasSwapped = true;
        }

        BuildData_ = std::move(leftData);
        BuildKeyColumns_ = leftKeyColumns;
        BuildKeyColumnsSet_ = THashSet<ui32>(BuildKeyColumns_->begin(), BuildKeyColumns_->end());
        // Use or not external payload depends on the policy
        IsBuildIndirected_ = policy->UseExternalPayload(
            EJoinAlgo::HashJoin, leftPSz, rightFetchedTuples / cardinality);

        ProbeStream_ = *rightStream;
        ProbeData_ = std::move(rightData);
        ProbeKeyColumns_ = rightKeyColumns;
        ProbeInputs_.resize(rightItemTypesArg->size());
        ProbeKeyColumnsSet_ = THashSet<ui32>(ProbeKeyColumns_->begin(), ProbeKeyColumns_->end());
        // Use or not external payload depends on the policy
        IsProbeIndirected_ = policy->UseExternalPayload(
            EJoinAlgo::HashJoin, rightPSz, rightFetchedTuples / cardinality);

        // Create converters
        auto pool = &Ctx_.ArrowMemoryPool;

        TVector<TType*> leftItemTypes;
        if (IsBuildIndirected_) {
            // split types on two lists: key and payload
            TVector<TType*> leftPayloadItemTypes;
            for (size_t i = 0; i < leftItemTypesArg->size() - 1; i++) {
                if (BuildKeyColumnsSet_.contains(i)) {
                    leftItemTypes.push_back(AS_TYPE(TBlockType, (*leftItemTypesArg)[i])->GetItemType());
                } else {
                    leftPayloadItemTypes.push_back(AS_TYPE(TBlockType, (*leftItemTypesArg)[i])->GetItemType());
                }
            }

            // add indirection index column as payload column to converter
            auto ui64Type = Ctx_.TypeEnv.GetUi64Lazy();
            leftItemTypes.push_back(ui64Type);

            // create external payload storage for payload columns
            BuildExternalPayloadStorage_ = Ctx_.HolderFactory.Create<TExternalPayloadStorage>(Ctx_, leftPayloadItemTypes, true);
        } else {
            for (size_t i = 0; i < leftItemTypesArg->size() - 1; i++) { // ignore last column, because this is block size
                leftItemTypes.push_back(AS_TYPE(TBlockType, (*leftItemTypesArg)[i])->GetItemType());
            }
        }
        TVector<NPackedTuple::EColumnRole> buildRoles(leftItemTypes.size(), NPackedTuple::EColumnRole::Payload);
        for (auto keyCol: *BuildKeyColumns_) {
            buildRoles[keyCol] = NPackedTuple::EColumnRole::Key;
        }
        BuildConverter_ = MakeBlockLayoutConverter(TTypeInfoHelper(), leftItemTypes, buildRoles, pool);

        TVector<TType*> rightItemTypes;
        if (IsProbeIndirected_) {
            // split types on two lists: key and payload
            TVector<TType*> rightPayloadItemTypes;
            for (size_t i = 0; i < rightItemTypesArg->size() - 1; i++) {
                if (ProbeKeyColumnsSet_.contains(i)) {
                    rightItemTypes.push_back(AS_TYPE(TBlockType, (*rightItemTypesArg)[i])->GetItemType());
                } else {
                    rightPayloadItemTypes.push_back(AS_TYPE(TBlockType, (*rightItemTypesArg)[i])->GetItemType());
                }
            }

            // add indirection index column as payload column to converter
            auto ui64Type = Ctx_.TypeEnv.GetUi64Lazy();
            rightItemTypes.push_back(ui64Type);

            // create external payload storage for payload columns
            ProbeExternalPayloadStorage_ = Ctx_.HolderFactory.Create<TExternalPayloadStorage>(Ctx_, rightPayloadItemTypes);
        } else {
            for (size_t i = 0; i < rightItemTypesArg->size() - 1; i++) { // ignore last column, because this is block size
                rightItemTypes.push_back(AS_TYPE(TBlockType, (*rightItemTypesArg)[i])->GetItemType());
            }
        }
        TVector<NPackedTuple::EColumnRole> probeRoles(rightItemTypes.size(), NPackedTuple::EColumnRole::Payload);
        for (auto keyCol: *ProbeKeyColumns_) {
            probeRoles[keyCol] = NPackedTuple::EColumnRole::Key;
        }
        ProbeConverter_ = MakeBlockLayoutConverter(TTypeInfoHelper(), rightItemTypes, probeRoles, pool);

        Table_.SetTupleLayout(BuildConverter_->GetTupleLayout());

        // Prepare pointers to external payload storage for Join state
        auto buildPayloadStorage = static_cast<TExternalPayloadStorage*>(BuildExternalPayloadStorage_.AsBoxed().Get());
        auto probePayloadStorage = static_cast<TExternalPayloadStorage*>(ProbeExternalPayloadStorage_.AsBoxed().Get());

        // Create inner hash join state
        JoinState_ = Ctx_.HolderFactory.Create<TJoinState>(
            ResultItemTypes_, BuildConverter_.get(), ProbeConverter_.get(), leftIOMap, rightIOMap,
            buildPayloadStorage, probePayloadStorage, wasSwapped);
        auto& joinState = *static_cast<TJoinState*>(JoinState_.AsBoxed().Get());

        // Reserve buffers for overflow
        size_t nTuplesBuild = 0;
        for (auto& block: BuildData_) {
            nTuplesBuild += block.Size;
        }
        joinState.BuildPackedInput.Overflow.reserve(
            CalculateExpectedOverflowSize(BuildConverter_->GetTupleLayout(), nTuplesBuild));

        size_t nTuplesProbe = CalcMaxBlockLength(rightItemTypes, false) * 4; // Lets assume that average join selectivity eq 25%, so we have to fetch 4 blocks in general to fill output properly
        joinState.ProbePackedInput.Overflow.reserve(
            CalculateExpectedOverflowSize(ProbeConverter_->GetTupleLayout(), nTuplesProbe));

        // Reserve memory for probe input
        joinState.ProbePackedInput.PackedTuples.reserve(
            CalcMaxBlockLength(rightItemTypes, false) * ProbeConverter_->GetTupleLayout()->TotalRowSize);

        // Reserve memory for output
        joinState.BuildPackedOutput.reserve(
            CalcMaxBlockLength(leftItemTypes, false) * BuildConverter_->GetTupleLayout()->TotalRowSize);
        joinState.ProbePackedOutput.reserve(
            CalcMaxBlockLength(rightItemTypes, false) * ProbeConverter_->GetTupleLayout()->TotalRowSize);
    }

    void BuildIndex() {
        const auto begin = std::chrono::steady_clock::now();
        Y_DEFER {
            const auto end = std::chrono::steady_clock::now();
            const auto spent =
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
            globalResourceMeter.UpdateStageSpentTime(JoinName_, "Build", spent);
        };

        auto& joinState = *static_cast<TJoinState*>(JoinState_.AsBoxed().Get());
        auto payloadStorage = static_cast<TExternalPayloadStorage*>(BuildExternalPayloadStorage_.AsBoxed().Get()); 

        for (auto& block: BuildData_) {
            if (IsBuildIndirected_) {
                auto [keyBlock, payloadBlock] = TExternalPayloadStorage::SplitBlock(
                                                    block, *payloadStorage, BuildKeyColumnsSet_);
                BuildConverter_->Pack(keyBlock.Columns, joinState.BuildPackedInput);
                payloadStorage->AddBlock(std::move(payloadBlock));
            } else {
                BuildConverter_->Pack(block.Columns, joinState.BuildPackedInput);
            }
        }
        BuildData_.clear(); // we don't need this data anymore, so don't waste memory

        auto& packed = joinState.BuildPackedInput;
        Table_.Build(packed.PackedTuples.data(), packed.Overflow.data(), packed.NTuples);
    }

    NUdf::EFetchStatus DoProbe() {
        const auto begin = std::chrono::steady_clock::now();
        Y_DEFER {
            const auto end = std::chrono::steady_clock::now();
            const auto spent =
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
            globalResourceMeter.UpdateStageSpentTime(JoinName_, "Probe", spent);
        };

        NUdf::EFetchStatus status{NUdf::EFetchStatus::Finish};
        auto& joinState = *static_cast<TJoinState*>(JoinState_.AsBoxed().Get());

        // If we have some output blocks from previous DoProbe call
        if (joinState.HasBlocks()) {
            return NUdf::EFetchStatus::Ok;
        }

        while (joinState.IsNotFull() && joinState.HasEnoughMemory()) {
            if (!IsFinished_) {
                status = ProbeStream_.WideFetch(ProbeInputs_.data(), ProbeInputs_.size());
            }

            // If we have some cached probe data in ProbeData_
            // handle it no matter what status we got from ProbeStream_
            if (status == NUdf::EFetchStatus::Yield && ProbeData_.empty()) {
                return NUdf::EFetchStatus::Yield;
            }
            if (status == NUdf::EFetchStatus::Finish) {
                IsFinished_ = true;
                if (ProbeData_.empty()) {
                    break;
                }
            }

            if (status == NUdf::EFetchStatus::Ok) {
                // Extract block and put it to cache
                TBlock fetchedBlock;
                TVector<arrow::Datum> blockColumns;
                for (size_t i = 0; i < ProbeInputs_.size() - 1; i++) {
                    auto& datum = TArrowBlock::From(ProbeInputs_[i]).GetDatum();
                    blockColumns.push_back(datum.array());
                }

                auto blockSize = ::GetBlockCount(ProbeInputs_[ProbeInputs_.size() - 1]);
                fetchedBlock.Size = blockSize;
                fetchedBlock.Columns = std::move(blockColumns);
                ProbeData_.emplace_back(std::move(fetchedBlock));
            }

            // Convert
            PackNextProbeBlock(joinState);

            // Do lookup, add result to state
            DoBatchLookup(joinState);

            // Clear probe's packed tuples
            // Overflow cant be cleared because output have pointers to it
            // Also payload block storage can't be cleared too for the same reason
            joinState.ProbePackedInput.PackedTuples.clear();
            joinState.ProbePackedInput.NTuples = 0;
        }

        // Nothing to do, all work was done
        if (joinState.OutputRows == 0) {
            Y_ENSURE(status == NUdf::EFetchStatus::Finish);
            joinState.ResetInput();
            joinState.ResetOutput();
            return NUdf::EFetchStatus::Finish;
        }

        // Make output
        joinState.MakeBlocks(Ctx_.HolderFactory);
        joinState.ResetInput();
        joinState.ResetOutput();
        return NUdf::EFetchStatus::Ok;
    }

    void FillOutput(NUdf::TUnboxedValue* output, ui32 width) {
        auto& joinState = *static_cast<TJoinState*>(JoinState_.AsBoxed().Get());
        auto sliceSize = joinState.Slice();
        for (size_t i = 0; i < width; i++) {
            output[i] = joinState.Get(sliceSize, Ctx_.HolderFactory, i);
        }
    }

private:
    void PackNextProbeBlock(TJoinState& joinState) {
        const auto& block = ProbeData_.front();

        if (IsProbeIndirected_) {
            auto& payloadStorage = *static_cast<TExternalPayloadStorage*>(ProbeExternalPayloadStorage_.AsBoxed().Get());
            auto [keyBlock, payloadBlock] = TExternalPayloadStorage::SplitBlock(
                                                block, payloadStorage, ProbeKeyColumnsSet_);
            ProbeConverter_->Pack(keyBlock.Columns, joinState.ProbePackedInput);
            payloadStorage.AddBlock(std::move(payloadBlock));
        } else {
            ProbeConverter_->Pack(block.Columns, joinState.ProbePackedInput);
        }

        ProbeData_.pop_front();
    }

    void DoBatchLookup(TJoinState& joinState) {
        auto* buildLayout = BuildConverter_->GetTupleLayout();
        auto* probeLayout = ProbeConverter_->GetTupleLayout();
        auto  tuple = joinState.ProbePackedInput.PackedTuples.data();
        auto  nTuples = joinState.ProbePackedInput.NTuples;
        auto  overflow = joinState.ProbePackedInput.Overflow.data();

        using TIterPair = std::pair<TTable::TIterator, const ui8*>;
        const auto batchSize = 64;
        TVector<TIterPair> iterators(batchSize);

        for (size_t i = 0; i < nTuples; i += batchSize) {
            auto remaining = std::min<ui64>(batchSize, nTuples - i);
            for (size_t offset = 0; offset < remaining; ++offset, tuple += probeLayout->TotalRowSize) {
                iterators[offset] = {Table_.Find(tuple, overflow), tuple};
            }

            for (size_t offset = 0; offset < remaining; ++offset) {
                auto [it, inTuple] = iterators[offset];
                const ui8* foundTuple = nullptr;
                while ((foundTuple = Table_.NextMatch(it)) != nullptr) {
                    // Copy tuple from build part into output
                    auto prevSize = joinState.BuildPackedOutput.size();
                    joinState.BuildPackedOutput.resize(prevSize + buildLayout->TotalRowSize);
                    std::copy(foundTuple, foundTuple + buildLayout->TotalRowSize, joinState.BuildPackedOutput.data() + prevSize);

                    // Copy tuple from probe part into output
                    prevSize = joinState.ProbePackedOutput.size();
                    joinState.ProbePackedOutput.resize(prevSize + probeLayout->TotalRowSize);
                    std::copy(inTuple, inTuple + probeLayout->TotalRowSize, joinState.ProbePackedOutput.data() + prevSize);

                    // New row added
                    joinState.OutputRows++;
                }
            }
        }
    }

private:
    TComputationContext&        Ctx_;
    const char *                JoinName_;
    const TVector<TType*>*      ResultItemTypes_;

    TDeque<TBlock>              BuildData_;
    const TVector<ui32>*        BuildKeyColumns_;
    THashSet<ui32>              BuildKeyColumnsSet_;
    IBlockLayoutConverter::TPtr BuildConverter_;
    NUdf::TUnboxedValue         BuildExternalPayloadStorage_;
    bool                        IsBuildIndirected_{false}; // was external payload storage used

    NUdf::TUnboxedValue         ProbeStream_;
    TUnboxedValueVector         ProbeInputs_;
    TDeque<TBlock>              ProbeData_;
    const TVector<ui32>*        ProbeKeyColumns_;
    THashSet<ui32>              ProbeKeyColumnsSet_;
    IBlockLayoutConverter::TPtr ProbeConverter_;
    NUdf::TUnboxedValue         ProbeExternalPayloadStorage_;
    bool                        IsProbeIndirected_{false}; // was external payload storage used

    NUdf::TUnboxedValue         JoinState_;
    TTable                      Table_;
    bool                        IsFinished_{false};
};

// -------------------------------------------------------------------
class TInMemoryGraceJoin : public TComputationValue<TInMemoryGraceJoin> {
private:
    using TBase = TComputationValue<TInMemoryGraceJoin>;
    using TBlock = TTempJoinStorage::TBlock;
    using TTable = TNeumannTable;

public:
    TInMemoryGraceJoin(
        TMemoryUsageInfo*       memInfo,
        TComputationContext&    ctx,
        const char *            joinName,
        const TVector<TType*>*  resultItemTypes,
        const TVector<TType*>*  leftItemTypesArg,
        const TVector<ui32>*    leftKeyColumns,
        const TVector<ui32>&    leftIOMap,
        const TVector<TType*>*  rightItemTypesArg,
        const TVector<ui32>*    rightKeyColumns,
        const TVector<ui32>&    rightIOMap,
        IBlockGraceJoinPolicy*  policy,
        NUdf::TUnboxedValue     tempStorageValue
    )
        : TBase(memInfo)
        , Ctx_(ctx)
        , JoinName_(joinName)
        , ResultItemTypes_(resultItemTypes)
    {
        using EJoinAlgo = IBlockGraceJoinPolicy::EJoinAlgo;

        auto& tempStorage = *static_cast<TTempJoinStorage*>(tempStorageValue.AsBoxed().Get());
        auto [leftPSz, rightPSz] = tempStorage.GetPayloadSizes();
        auto [leftFetchedTuples, rightFetchedTuples] = tempStorage.GetFetchedTuples();
        auto maxFetchedTuples = std::max(leftFetchedTuples, rightFetchedTuples);
        auto cardinality = tempStorage.EstimateCardinality(); // bootstrap value, may be far from truth
        auto [leftData, rightData] = tempStorage.DetachData();
        
        size_t leftRowsNum = 0;
        for (const auto& block : leftData) {
            leftRowsNum += block.Size;
        }

        size_t rightRowsNum = 0;
        for (const auto& block : rightData) {
            rightRowsNum += block.Size;
        }

        THashSet<ui32> leftKeyColumnsSet(leftKeyColumns->begin(), leftKeyColumns->end());
        // Use or not external payload depends on the policy
        bool isLeftIndirected = policy->UseExternalPayload(
            EJoinAlgo::InMemoryGraceJoin, leftPSz, maxFetchedTuples / cardinality);

        THashSet<ui32> rightKeyColumnsSet(rightKeyColumns->begin(), rightKeyColumns->end());
        // Use or not external payload depends on the policy
        bool isRightIndirected = policy->UseExternalPayload(
            EJoinAlgo::InMemoryGraceJoin, rightPSz, maxFetchedTuples / cardinality);

        // Create converters
        auto pool = &Ctx_.ArrowMemoryPool;

        TVector<TType*> leftItemTypes;
        if (isLeftIndirected) {
            // split types on two lists: key and payload
            TVector<TType*> leftPayloadItemTypes;
            for (size_t i = 0; i < leftItemTypesArg->size() - 1; i++) {
                if (leftKeyColumnsSet.contains(i)) {
                    leftItemTypes.push_back(AS_TYPE(TBlockType, (*leftItemTypesArg)[i])->GetItemType());
                } else {
                    leftPayloadItemTypes.push_back(AS_TYPE(TBlockType, (*leftItemTypesArg)[i])->GetItemType());
                }
            }

            // add indirection index column as payload column to converter
            auto ui64Type = Ctx_.TypeEnv.GetUi64Lazy();
            leftItemTypes.push_back(ui64Type);

            // create external payload storage for payload columns
            LeftExternalPayloadStorage_ = Ctx_.HolderFactory.Create<TExternalPayloadStorage>(Ctx_, leftPayloadItemTypes, true);
        } else {
            for (size_t i = 0; i < leftItemTypesArg->size() - 1; i++) { // ignore last column, because this is block size
                leftItemTypes.push_back(AS_TYPE(TBlockType, (*leftItemTypesArg)[i])->GetItemType());
            }
        }
        TVector<NPackedTuple::EColumnRole> leftRoles(leftItemTypes.size(), NPackedTuple::EColumnRole::Payload);
        for (auto keyCol: *leftKeyColumns) {
            leftRoles[keyCol] = NPackedTuple::EColumnRole::Key;
        }
        LeftConverter_ = MakeBlockLayoutConverter(TTypeInfoHelper(), leftItemTypes, leftRoles, pool);

        TVector<TType*> rightItemTypes;
        if (isRightIndirected) {
            // split types on two lists: key and payload
            TVector<TType*> rightPayloadItemTypes;
            for (size_t i = 0; i < rightItemTypesArg->size() - 1; i++) {
                if (rightKeyColumnsSet.contains(i)) {
                    rightItemTypes.push_back(AS_TYPE(TBlockType, (*rightItemTypesArg)[i])->GetItemType());
                } else {
                    rightPayloadItemTypes.push_back(AS_TYPE(TBlockType, (*rightItemTypesArg)[i])->GetItemType());
                }
            }

            // add indirection index column as payload column to converter
            auto ui64Type = Ctx_.TypeEnv.GetUi64Lazy();
            rightItemTypes.push_back(ui64Type);

            // create external payload storage for payload columns
            RightExternalPayloadStorage_ = Ctx_.HolderFactory.Create<TExternalPayloadStorage>(Ctx_, rightPayloadItemTypes, true);
        } else {
            for (size_t i = 0; i < rightItemTypesArg->size() - 1; i++) { // ignore last column, because this is block size
                rightItemTypes.push_back(AS_TYPE(TBlockType, (*rightItemTypesArg)[i])->GetItemType());
            }
        }
        TVector<NPackedTuple::EColumnRole> rightRoles(rightItemTypes.size(), NPackedTuple::EColumnRole::Payload);
        for (auto keyCol: *rightKeyColumns) {
            rightRoles[keyCol] = NPackedTuple::EColumnRole::Key;
        }
        RightConverter_ = MakeBlockLayoutConverter(TTypeInfoHelper(), rightItemTypes, rightRoles, pool);

        const size_t leftTupleSize = leftRowsNum * LeftConverter_->GetTupleLayout()->TotalRowSize;
        const size_t rightTupleSize = rightRowsNum * RightConverter_->GetTupleLayout()->TotalRowSize;
        const size_t minTupleSize = std::min(leftTupleSize, rightTupleSize);
        constexpr size_t bucketDesiredSize = 4 * L2_CACHE_SIZE;

        BucketsLogNum_ = minTupleSize ? sizeof(size_t) * 8 - std::countl_zero((minTupleSize - 1) / bucketDesiredSize) : 0;
        LeftBuckets_.resize(1u << BucketsLogNum_);
        RightBuckets_.resize(1u << BucketsLogNum_);

        const size_t leftOverflowSizeEst = CalculateExpectedOverflowSize(LeftConverter_->GetTupleLayout(), leftRowsNum >> BucketsLogNum_);
        const size_t rightOverflowSizeEst = CalculateExpectedOverflowSize(RightConverter_->GetTupleLayout(), rightRowsNum >> BucketsLogNum_);
        for (ui32 bucket = 0; bucket < (1u << BucketsLogNum_); ++bucket) {
            LeftBuckets_[bucket].Overflow.reserve(leftOverflowSizeEst);
            RightBuckets_[bucket].Overflow.reserve(rightOverflowSizeEst);
        }

        // Prepare pointers to external payload storage for Join state
        auto leftPayloadStorage = static_cast<TExternalPayloadStorage*>(LeftExternalPayloadStorage_.AsBoxed().Get());
        auto rightPayloadStorage = static_cast<TExternalPayloadStorage*>(RightExternalPayloadStorage_.AsBoxed().Get());

        // Create inner hash join state
        JoinState_ = Ctx_.HolderFactory.Create<TJoinState>(
            ResultItemTypes_, LeftConverter_.get(), RightConverter_.get(), leftIOMap, rightIOMap,
            leftPayloadStorage, rightPayloadStorage, false);
        auto& joinState = *static_cast<TJoinState*>(JoinState_.AsBoxed().Get());

        for (auto &block : leftData) {
            if (isLeftIndirected) {
                auto [keyBlock, payloadBlock] = TExternalPayloadStorage::SplitBlock(
                                                    block, *leftPayloadStorage, leftKeyColumnsSet);
                LeftConverter_->BucketPack(keyBlock.Columns, LeftBuckets_.data(), BucketsLogNum_);
                leftPayloadStorage->AddBlock(std::move(payloadBlock));
            } else {
                LeftConverter_->BucketPack(block.Columns, LeftBuckets_.data(), BucketsLogNum_);
            }
        }
        leftData.clear();

        for (auto &block : rightData) {
            if (isRightIndirected) {
                auto [keyBlock, payloadBlock] = TExternalPayloadStorage::SplitBlock(
                                                    block, *rightPayloadStorage, rightKeyColumnsSet);
                RightConverter_->BucketPack(keyBlock.Columns, RightBuckets_.data(), BucketsLogNum_);
                rightPayloadStorage->AddBlock(std::move(payloadBlock));
            } else {
                RightConverter_->BucketPack(block.Columns, RightBuckets_.data(), BucketsLogNum_);
            }
        }
        rightData.clear();
        
        // Reserve memory for output
        joinState.BuildPackedOutput.reserve(
            CalcMaxBlockLength(leftItemTypes, false) * LeftConverter_->GetTupleLayout()->TotalRowSize);
        joinState.ProbePackedOutput.reserve(
            CalcMaxBlockLength(rightItemTypes, false) * RightConverter_->GetTupleLayout()->TotalRowSize);
    }

    NUdf::EFetchStatus DoProbe() {
        const auto begin = std::chrono::steady_clock::now();
        Y_DEFER {
            const auto end = std::chrono::steady_clock::now();
            const auto spent =
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
            globalResourceMeter.UpdateStageSpentTime(JoinName_, "Probe", spent);
        };

        if (CurrBucket_ >> BucketsLogNum_) {
            return NUdf::EFetchStatus::Finish;
        }

        auto& joinState = *static_cast<TJoinState*>(JoinState_.AsBoxed().Get());

        // If we have some output blocks from previous DoProbe call
        if (joinState.HasBlocks()) {
            return NUdf::EFetchStatus::Ok;
        }

        if (NeedNextBucket_) {
            NeedNextBucket_ = false;
            BuildIndex(joinState);
        }

        // Fill output buffers and signal if next bucket is needed
        DoBatchLookup(joinState);

        if (joinState.OutputRows == 0) {
            return DoProbe();
        }

        // Make output
        joinState.MakeBlocks(Ctx_.HolderFactory);
        // Reset input only if pair of buckets fully processed, or we will reset currently processing data
        if (NeedNextBucket_) {
            joinState.ResetInput();
        }
        joinState.ResetOutput();
        return NUdf::EFetchStatus::Ok;
    }

    void FillOutput(NUdf::TUnboxedValue* output, ui32 width) {
        auto& joinState = *static_cast<TJoinState*>(JoinState_.AsBoxed().Get());
        auto sliceSize = joinState.Slice();
        for (size_t i = 0; i < width; i++) {
            output[i] = joinState.Get(sliceSize, Ctx_.HolderFactory, i);
        }
    }

private:
    void BuildIndex(TJoinState& joinState) {
        const auto begin = std::chrono::steady_clock::now();
        Y_DEFER {
            const auto end = std::chrono::steady_clock::now();
            const auto spent =
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
            globalResourceMeter.UpdateStageSpentTime(JoinName_, "Build", spent);
        };

        auto& leftPack = LeftBuckets_[CurrBucket_];
        auto& rightPack = RightBuckets_[CurrBucket_];

        if (leftPack.NTuples < rightPack.NTuples) {
            joinState.SetSwapped(false);
            joinState.BuildPackedInput = std::move(leftPack);
            joinState.ProbePackedInput = std::move(rightPack);
            Table_.SetTupleLayout(LeftConverter_->GetTupleLayout());
        } else {
            joinState.SetSwapped(true);
            joinState.BuildPackedInput = std::move(rightPack);
            joinState.ProbePackedInput = std::move(leftPack);
            Table_.SetTupleLayout(RightConverter_->GetTupleLayout());
        }
        
        auto& packed = joinState.BuildPackedInput;
        Table_.Build(packed.PackedTuples.data(), packed.Overflow.data(), packed.NTuples);
    }

    void DoBatchLookup(TJoinState& joinState) {
        const bool wasSwapped = joinState.GetSwapped();
        auto *const buildLayout = wasSwapped ? RightConverter_->GetTupleLayout() : LeftConverter_->GetTupleLayout();
        auto *const probeLayout = wasSwapped ? LeftConverter_->GetTupleLayout() : RightConverter_->GetTupleLayout();

        const auto nTuples = joinState.ProbePackedInput.NTuples;
        auto *const overflow = joinState.ProbePackedInput.Overflow.data();
        auto *tuple = joinState.ProbePackedInput.PackedTuples.data() + CurrProbeRow_ * probeLayout->TotalRowSize;

        using TIterPair = std::pair<TTable::TIterator, const ui8*>;
        const auto batchSize = 64;
        TVector<TIterPair> iterators(batchSize);

        // TODO: interrupt this loop when joinState is full as in BlockMapJoin. So track current iterator and save iterators somewhere.
        // WARNING: we can not properly track the number of output rows due to uninterruptible for loop in DoBatchLookup,
        // so add joinState.IsNotFull() check to prevent overflow in AddMany builder's method.
        for (; CurrProbeRow_ < nTuples && joinState.IsNotFull(); CurrProbeRow_ += batchSize) {
            auto remaining = std::min<ui64>(batchSize, nTuples - CurrProbeRow_);
            for (size_t offset = 0; offset < remaining; ++offset, tuple += probeLayout->TotalRowSize) {
                iterators[offset] = {Table_.Find(tuple, overflow), tuple};
            }

            for (size_t offset = 0; offset < remaining; ++offset) {
                auto [it, inTuple] = iterators[offset];
                const ui8* foundTuple = nullptr;
                while ((foundTuple = Table_.NextMatch(it)) != nullptr) {
                    // Copy tuple from build part into output
                    auto prevSize = joinState.BuildPackedOutput.size();
                    joinState.BuildPackedOutput.resize(prevSize + buildLayout->TotalRowSize);
                    std::copy(foundTuple, foundTuple + buildLayout->TotalRowSize, joinState.BuildPackedOutput.data() + prevSize);

                    // Copy tuple from probe part into output
                    prevSize = joinState.ProbePackedOutput.size();
                    joinState.ProbePackedOutput.resize(prevSize + probeLayout->TotalRowSize);
                    std::copy(inTuple, inTuple + probeLayout->TotalRowSize, joinState.ProbePackedOutput.data() + prevSize);

                    // New row added
                    joinState.OutputRows++;
                }
            }
        }

        if (CurrProbeRow_ >= nTuples) { // >= because remaining can be less than batchSize
            NeedNextBucket_ = true;
            ++CurrBucket_;
            CurrProbeRow_ = 0;
        }
    }

private:
    TComputationContext&        Ctx_;
    const char *                JoinName_;
    const TVector<TType*>*      ResultItemTypes_;

    std::unique_ptr<IBlockLayoutConverter> LeftConverter_;
    std::unique_ptr<IBlockLayoutConverter> RightConverter_;

    ui32 BucketsLogNum_;
    TVector<IBlockLayoutConverter::PackResult> LeftBuckets_;
    TVector<IBlockLayoutConverter::PackResult> RightBuckets_;
    NUdf::TUnboxedValue JoinState_;
    TTable Table_;

    NUdf::TUnboxedValue LeftExternalPayloadStorage_;
    NUdf::TUnboxedValue RightExternalPayloadStorage_;

    ui32 CurrBucket_ = 0;
    ui32 CurrProbeRow_ = 0;
    bool NeedNextBucket_{true};  // if need to get to next bucket
};

// -------------------------------------------------------------------
class TStreamValue : public TComputationValue<TStreamValue> {
private:
    using TBase = TComputationValue<TStreamValue>;

    enum class EMode {
        Start,  // trying to decide what algorithm use: hash join or grace hash join
        HashJoin,
        InMemoryGraceJoin,
        GraceHashJoin,
    };
    
public:
    TStreamValue(
        TMemoryUsageInfo*       memInfo,
        TComputationContext&    ctx,
        const TVector<TType*>&  resultItemTypes,
        NUdf::TUnboxedValue&&   leftStream,
        const TVector<TType*>&  leftItemTypes,
        const TVector<ui32>&    leftKeyColumns,
        const TVector<ui32>&    leftIOMap,
        NUdf::TUnboxedValue&&   rightStream,
        const TVector<TType*>&  rightItemTypes,
        const TVector<ui32>&    rightKeyColumns,
        const TVector<ui32>&    rightIOMap,
        IBlockGraceJoinPolicy*  policy
    )
        : TBase(memInfo)
        , Ctx_(ctx)
        , ResultItemTypes_(resultItemTypes)
        , LeftStream_(std::move(leftStream))
        , LeftItemTypes_(leftItemTypes)
        , LeftKeyColumns_(leftKeyColumns)
        , LeftIOMap_(leftIOMap)
        , RightStream_(std::move(rightStream))
        , RightItemTypes_(rightItemTypes)
        , RightKeyColumns_(rightKeyColumns)
        , RightIOMap_(rightIOMap)
        , Policy_(policy)
    {
        TempStorage_ = Ctx_.HolderFactory.Create<TTempJoinStorage>(
            leftItemTypes,
            leftKeyColumns,
            LeftStream_,
            rightItemTypes,
            rightKeyColumns,
            RightStream_,
            Policy_,
            &Ctx_.ArrowMemoryPool
        );
    }

private:
    NUdf::EFetchStatus WideFetch(NUdf::TUnboxedValue* output, ui32 width) {
        const auto begin = std::chrono::steady_clock::now();
        Y_DEFER {
            const auto end = std::chrono::steady_clock::now();
            const auto spent =
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
            globalResourceMeter.UpdateSpentTime(JoinName_, spent);
            globalResourceMeter.UpdateConsumptedMemory(JoinName_, TlsAllocState->GetUsed());
        };

    switch_mode:
        switch (GetMode()) {
        case EMode::Start: {
            using EStatus = TTempJoinStorage::EStatus;
            using EJoinAlgo = IBlockGraceJoinPolicy::EJoinAlgo;

            auto& tempStorage = *GetTempState();
            auto status = EStatus::Unknown;

            while (status == EStatus::Unknown) {
                if (tempStorage.FetchStreams() == NUdf::EFetchStatus::Yield) {
                    return NUdf::EFetchStatus::Yield;
                }
                status = tempStorage.GetStatus();
            }

            switch (status) {
            case TTempJoinStorage::EStatus::BothStreamsFinished: {
                auto [lTuples, rTuples] = tempStorage.GetFetchedTuples();

                // The choice of algorithm depends on the policy. See default policy
                if (Policy_->PickAlgorithm(lTuples, rTuples) == EJoinAlgo::HashJoin) {
                    MakeHashJoin();
                    auto& hashJoin = *GetHashJoin();
                    hashJoin.BuildIndex();
                    SwitchModeTo(EMode::HashJoin);
                } else {
                    MakeInMemoryGraceJoin();
                    SwitchModeTo(EMode::InMemoryGraceJoin);
                }

                goto switch_mode;
            }
            case TTempJoinStorage::EStatus::OneStreamFinished: {
                auto [lTuples, rTuples] = tempStorage.GetFetchedTuples();
                auto [isLeftFinished, isRightFinished] = tempStorage.IsFinished();
                if (!isLeftFinished) {
                    lTuples = IBlockGraceJoinPolicy::STREAM_NOT_FETCHED;
                } else {
                    rTuples = IBlockGraceJoinPolicy::STREAM_NOT_FETCHED;
                }

                // The choice of algorithm depends on the policy. See default policy
                if (Policy_->PickAlgorithm(lTuples, rTuples) == EJoinAlgo::HashJoin) {
                    MakeHashJoin();
                    auto& hashJoin = *GetHashJoin();
                    hashJoin.BuildIndex();
                    SwitchModeTo(EMode::HashJoin);
                } else {
                    /// TODO: not implemented
                    Y_ASSERT(false); // Grace hash join not implemented yet
                    SwitchModeTo(EMode::GraceHashJoin);
                }

                goto switch_mode;
            }
            case TTempJoinStorage::EStatus::MemoryLimitExceeded: {
                /// TODO: not implemented
                Y_ASSERT(false); // Grace hash join not implemented yet

                SwitchModeTo(EMode::GraceHashJoin);
                goto switch_mode;
            }
            case TTempJoinStorage::EStatus::Unknown:
                Y_UNREACHABLE();
            }

            Y_UNREACHABLE();
        }
        case EMode::HashJoin: {
            auto& hashJoin = *GetHashJoin();
            auto status = hashJoin.DoProbe();
            if (status == NUdf::EFetchStatus::Ok) {
                hashJoin.FillOutput(output, width);
            }
            return status;
        }
        case EMode::InMemoryGraceJoin: {
            auto& join = *GetInMemoryGraceJoin();
            auto status = join.DoProbe();
            if (status == NUdf::EFetchStatus::Ok) {
                join.FillOutput(output, width);
            }
            return status;
        }
        case EMode::GraceHashJoin: {
            /// TODO: not implemented
            Y_ASSERT(false); // Grace hash join not implemented yet
        }
        }

        Y_UNREACHABLE();
    }

private:
    TTempJoinStorage* GetTempState() {
        return static_cast<TTempJoinStorage*>(TempStorage_.AsBoxed().Get());
    }

    void MakeHashJoin() {
        auto newJoinName = "BlockGraceJoin::HashJoin";
        Join_ = Ctx_.HolderFactory.Create<THashJoin>(
            Ctx_, newJoinName, &ResultItemTypes_,
            &LeftStream_, &LeftItemTypes_, &LeftKeyColumns_, LeftIOMap_,
            &RightStream_, &RightItemTypes_, &RightKeyColumns_, RightIOMap_,
            Policy_, std::move(TempStorage_));
        globalResourceMeter.MergeHistoryPages(JoinName_, newJoinName);
        JoinName_ = newJoinName;
    }

    THashJoin* GetHashJoin() {
        return static_cast<THashJoin*>(Join_.AsBoxed().Get());
    }

    void MakeInMemoryGraceJoin() {
        auto newJoinName = "BlockGraceJoin::InMemoryGraceJoin";
        Join_ = Ctx_.HolderFactory.Create<TInMemoryGraceJoin>(
            Ctx_, newJoinName, &ResultItemTypes_,
            &LeftItemTypes_, &LeftKeyColumns_, LeftIOMap_,
            &RightItemTypes_, &RightKeyColumns_, RightIOMap_,
            Policy_, std::move(TempStorage_));
        globalResourceMeter.MergeHistoryPages(JoinName_, newJoinName);
        JoinName_ = newJoinName;
    }

    TInMemoryGraceJoin* GetInMemoryGraceJoin() {
        return static_cast<TInMemoryGraceJoin*>(Join_.AsBoxed().Get());
    }

    EMode GetMode() const {
        return Mode_;
    }

    void SwitchModeTo(EMode other) {
        Mode_ = other;
    }

private:
    EMode                   Mode_{EMode::Start};
    TComputationContext&    Ctx_;
    const TVector<TType*>&  ResultItemTypes_;

    NUdf::TUnboxedValue     LeftStream_;
    const TVector<TType*>&  LeftItemTypes_;
    const TVector<ui32>&    LeftKeyColumns_;
    const TVector<ui32>&    LeftIOMap_;

    NUdf::TUnboxedValue     RightStream_;
    const TVector<TType*>&  RightItemTypes_;
    const TVector<ui32>&    RightKeyColumns_;
    const TVector<ui32>&    RightIOMap_;

    IBlockGraceJoinPolicy*  Policy_;

    NUdf::TUnboxedValue     TempStorage_;
    NUdf::TUnboxedValue     Join_;
    const char *            JoinName_ = "BlockGraceJoin";
};

// -------------------------------------------------------------------
class TBlockGraceJoinCoreWraper : public TMutableComputationNode<TBlockGraceJoinCoreWraper> {
private:
    using TBaseComputation = TMutableComputationNode<TBlockGraceJoinCoreWraper>;

public:
    TBlockGraceJoinCoreWraper(
        TComputationMutables&   mutables,
        const TVector<TType*>&& resultItemTypes,
        const TVector<TType*>&& leftItemTypes,
        const TVector<ui32>&&   leftKeyColumns,
        const TVector<ui32>&&   leftIOMap,
        const TVector<TType*>&& rightItemTypes,
        const TVector<ui32>&&   rightKeyColumns,
        const TVector<ui32>&&   rightIOMap,
        IComputationNode*       leftStream,
        IComputationNode*       rightStream,
        IBlockGraceJoinPolicy*  policy
    )
        : TBaseComputation(mutables, EValueRepresentation::Boxed)
        , ResultItemTypes_(std::move(resultItemTypes))
        , LeftItemTypes_(std::move(leftItemTypes))
        , LeftKeyColumns_(std::move(leftKeyColumns))
        , LeftIOMap_(std::move(leftIOMap))
        , RightItemTypes_(std::move(rightItemTypes))
        , RightKeyColumns_(std::move(rightKeyColumns))
        , RightIOMap_(std::move(rightIOMap))
        , LeftStream_(std::move(leftStream))
        , RightStream_(std::move(rightStream))
        , Policy_(policy)
        , KeyTupleCache_(mutables)
    {}

    NUdf::TUnboxedValuePod DoCalculate(TComputationContext& ctx) const {
        return ctx.HolderFactory.Create<TStreamValue>(
            ctx,
            ResultItemTypes_,
            std::move(LeftStream_->GetValue(ctx)),
            LeftItemTypes_,
            LeftKeyColumns_,
            LeftIOMap_,
            std::move(RightStream_->GetValue(ctx)),
            RightItemTypes_,
            RightKeyColumns_,
            RightIOMap_,
            Policy_
        );
    }

private:
    void RegisterDependencies() const final {
        this->DependsOn(LeftStream_);
        this->DependsOn(RightStream_);
    }

private:
    const TVector<TType*>   ResultItemTypes_;

    const TVector<TType*>   LeftItemTypes_;
    const TVector<ui32>     LeftKeyColumns_;
    const TVector<ui32>     LeftIOMap_;

    const TVector<TType*>   RightItemTypes_;
    const TVector<ui32>     RightKeyColumns_;
    const TVector<ui32>     RightIOMap_;

    IComputationNode*       LeftStream_;
    IComputationNode*       RightStream_;

    IBlockGraceJoinPolicy*  Policy_;

    const TContainerCacheOnContext KeyTupleCache_;
};

} // namespace

IComputationNode* WrapBlockGraceJoinCore(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 9, "Expected 9 args");

    const auto joinType = callable.GetType()->GetReturnType();
    MKQL_ENSURE(joinType->IsStream(), "Expected WideStream as a resulting stream");
    const auto joinStreamType = AS_TYPE(TStreamType, joinType);
    MKQL_ENSURE(joinStreamType->GetItemType()->IsMulti(),
                "Expected Multi as a resulting item type");
    const auto joinComponents = GetWideComponents(joinStreamType);
    MKQL_ENSURE(joinComponents.size() > 0, "Expected at least one column");
    const TVector<TType*> joinItems(joinComponents.cbegin(), joinComponents.cend());

    const auto leftType = callable.GetInput(0).GetStaticType();
    MKQL_ENSURE(leftType->IsStream(), "Expected WideStream as a left stream");
    const auto leftStreamType = AS_TYPE(TStreamType, leftType);
    MKQL_ENSURE(leftStreamType->GetItemType()->IsMulti(),
                "Expected Multi as a left stream item type");
    const auto leftStreamComponents = GetWideComponents(leftStreamType);
    MKQL_ENSURE(leftStreamComponents.size() > 0, "Expected at least one column");
    const TVector<TType*> leftStreamItems(leftStreamComponents.cbegin(), leftStreamComponents.cend());

    const auto rightType = callable.GetInput(1).GetStaticType();
    MKQL_ENSURE(rightType->IsStream(), "Expected WideStream as a right stream");
    const auto rightStreamType = AS_TYPE(TStreamType, rightType);
    MKQL_ENSURE(rightStreamType->GetItemType()->IsMulti(),
                "Expected Multi as a right stream item type");
    const auto rightStreamComponents = GetWideComponents(rightStreamType);
    MKQL_ENSURE(rightStreamComponents.size() > 0, "Expected at least one column");
    const TVector<TType*> rightStreamItems(rightStreamComponents.cbegin(), rightStreamComponents.cend());

    const auto joinKindNode = callable.GetInput(2);
    const auto rawKind = AS_VALUE(TDataLiteral, joinKindNode)->AsValue().Get<ui32>();
    const auto joinKind = GetJoinKind(rawKind);
    MKQL_ENSURE(joinKind == EJoinKind::Inner,
                "Only inner join is supported in block grace hash join prototype");

    const auto leftKeyColumnsLiteral = callable.GetInput(3);
    const auto leftKeyColumnsTuple = AS_VALUE(TTupleLiteral, leftKeyColumnsLiteral);
    TVector<ui32> leftKeyColumns;
    leftKeyColumns.reserve(leftKeyColumnsTuple->GetValuesCount());
    for (ui32 i = 0; i < leftKeyColumnsTuple->GetValuesCount(); i++) {
        const auto item = AS_VALUE(TDataLiteral, leftKeyColumnsTuple->GetValue(i));
        leftKeyColumns.emplace_back(item->AsValue().Get<ui32>());
    }
    const THashSet<ui32> leftKeySet(leftKeyColumns.cbegin(), leftKeyColumns.cend());

    const auto leftKeyDropsLiteral = callable.GetInput(4);
    const auto leftKeyDropsTuple = AS_VALUE(TTupleLiteral, leftKeyDropsLiteral);
    THashSet<ui32> leftKeyDrops;
    leftKeyDrops.reserve(leftKeyDropsTuple->GetValuesCount());
    for (ui32 i = 0; i < leftKeyDropsTuple->GetValuesCount(); i++) {
        const auto item = AS_VALUE(TDataLiteral, leftKeyDropsTuple->GetValue(i));
        leftKeyDrops.emplace(item->AsValue().Get<ui32>());
    }

    for (const auto& drop : leftKeyDrops) {
        MKQL_ENSURE(leftKeySet.contains(drop),
                    "Only key columns has to be specified in drop column set");
    }

    const auto rightKeyColumnsLiteral = callable.GetInput(5);
    const auto rightKeyColumnsTuple = AS_VALUE(TTupleLiteral, rightKeyColumnsLiteral);
    TVector<ui32> rightKeyColumns;
    rightKeyColumns.reserve(rightKeyColumnsTuple->GetValuesCount());
    for (ui32 i = 0; i < rightKeyColumnsTuple->GetValuesCount(); i++) {
        const auto item = AS_VALUE(TDataLiteral, rightKeyColumnsTuple->GetValue(i));
        rightKeyColumns.emplace_back(item->AsValue().Get<ui32>());
    }
    const THashSet<ui32> rightKeySet(rightKeyColumns.cbegin(), rightKeyColumns.cend());

    const auto rightKeyDropsLiteral = callable.GetInput(6);
    const auto rightKeyDropsTuple = AS_VALUE(TTupleLiteral, rightKeyDropsLiteral);
    THashSet<ui32> rightKeyDrops;
    rightKeyDrops.reserve(rightKeyDropsTuple->GetValuesCount());
    for (ui32 i = 0; i < rightKeyDropsTuple->GetValuesCount(); i++) {
        const auto item = AS_VALUE(TDataLiteral, rightKeyDropsTuple->GetValue(i));
        rightKeyDrops.emplace(item->AsValue().Get<ui32>());
    }

    for (const auto& drop : rightKeyDrops) {
        MKQL_ENSURE(rightKeySet.contains(drop),
                    "Only key columns has to be specified in drop column set");
    }

    MKQL_ENSURE(leftKeyColumns.size() == rightKeyColumns.size(), "Key columns mismatch");

    [[maybe_unused]] const auto rightAnyNode = callable.GetInput(7);

    const auto untypedPolicyNode = callable.GetInput(8);
    const auto untypedPolicy = AS_VALUE(TDataLiteral, untypedPolicyNode)->AsValue().Get<ui64>();
    const auto policy = static_cast<IBlockGraceJoinPolicy*>(reinterpret_cast<void*>(untypedPolicy));

    // XXX: Mind the last wide item, containing block length.
    TVector<ui32> leftIOMap;
    for (size_t i = 0; i < leftStreamItems.size() - 1; i++) {
        if (leftKeyDrops.contains(i)) {
            continue;
        }
        leftIOMap.push_back(i);
    }

    // XXX: Mind the last wide item, containing block length.
    TVector<ui32> rightIOMap;
    for (size_t i = 0; i < rightStreamItems.size() - 1; i++) {
        if (rightKeyDrops.contains(i)) {
            continue;
        }
        rightIOMap.push_back(i);
    }

    const auto leftStream = LocateNode(ctx.NodeLocator, callable, 0);
    const auto rightStream = LocateNode(ctx.NodeLocator, callable, 1);

    return new TBlockGraceJoinCoreWraper(
        ctx.Mutables,
        std::move(joinItems),
        std::move(leftStreamItems),
        std::move(leftKeyColumns),
        std::move(leftIOMap),
        std::move(rightStreamItems),
        std::move(rightKeyColumns),
        std::move(rightIOMap),
        leftStream,
        rightStream,
        policy ? policy : &globalDefaultPolicy
    );
}

} // namespace NKikimr::NMiniKQL
