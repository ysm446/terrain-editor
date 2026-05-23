#pragma once

#include <cstdint>
#include <functional>
#include <thread>

#include <d3d12.h>

namespace terrain::gpu
{

struct GpuComputeContext
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64* fenceLastSignaledValue = nullptr;
    std::thread::id mainThreadId{};
    std::function<void(UINT64)> waitForFenceValue;
};

inline bool IsGpuComputeContextReady(const GpuComputeContext& context)
{
    return context.device != nullptr &&
           context.commandQueue != nullptr &&
           context.fence != nullptr &&
           context.fenceLastSignaledValue != nullptr &&
           static_cast<bool>(context.waitForFenceValue);
}

} // namespace terrain::gpu
