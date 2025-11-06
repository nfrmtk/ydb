#pragma once
#include <optional>
#include <yql/essentials/minikql/mkql_alloc.h>

namespace NKikimr::NMiniKQL{

int MemoryUsagePercent(int totalBytes) {
    return 100*totalBytes / TlsAllocState->GetLimit();
}

int FreeMemory() {
    return TlsAllocState->GetLimit() - TlsAllocState->GetAllocated(); 
}

bool AllocateWithSizeMayThrow(i64 size) {
    return FreeMemory() > size;
}

std::optional<int> GetMemoryUsageIfReachedLimit() {
    if (!TlsAllocState->GetMaximumLimitValueReached()) {
        return std::nullopt;
    }
    return std::make_optional<int>(MemoryUsagePercent(TlsAllocState->GetUsed()));
}

bool MemoryPercentIsFree(int freePercent) {
    std::optional<int> usedPercent = GetMemoryUsageIfReachedLimit();
    return usedPercent && (freePercent + *usedPercent < 100);
}
}