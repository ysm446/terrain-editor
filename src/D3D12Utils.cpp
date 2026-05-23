#include "D3D12Utils.h"

#include <stdexcept>

namespace terrain::d3d12
{

void ThrowIfFailed(HRESULT hr, const char* message)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(message);
    }
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferResourceDesc(UINT64 byteSize, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = byteSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

D3D12_RESOURCE_DESC Texture2DResourceDesc(UINT width,
                                          UINT height,
                                          DXGI_FORMAT format,
                                          D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

D3D12_DESCRIPTOR_HEAP_DESC DescriptorHeapDesc(D3D12_DESCRIPTOR_HEAP_TYPE type,
                                             UINT descriptorCount,
                                             D3D12_DESCRIPTOR_HEAP_FLAGS flags)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = descriptorCount;
    desc.Flags = flags;
    return desc;
}

D3D12_DESCRIPTOR_HEAP_DESC ShaderVisibleCbvSrvUavDescriptorHeapDesc(UINT descriptorCount)
{
    return DescriptorHeapDesc(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                              descriptorCount,
                              D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
}

UINT DefaultShaderCompileFlags()
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    return flags;
}

HRESULT CreateRootSignatureFromDesc(ID3D12Device* device,
                                    const D3D12_ROOT_SIGNATURE_DESC& desc,
                                    ID3D12RootSignature** rootSignature,
                                    ID3DBlob** errorBlob)
{
    if (errorBlob)
    {
        *errorBlob = nullptr;
    }

    ID3DBlob* signatureBlob = nullptr;
    ID3DBlob* localErrorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&desc,
                                             D3D_ROOT_SIGNATURE_VERSION_1,
                                             &signatureBlob,
                                             errorBlob ? errorBlob : &localErrorBlob);
    if (FAILED(hr))
    {
        if (localErrorBlob)
        {
            localErrorBlob->Release();
        }
        return hr;
    }

    hr = device->CreateRootSignature(0,
                                     signatureBlob->GetBufferPointer(),
                                     signatureBlob->GetBufferSize(),
                                     IID_PPV_ARGS(rootSignature));
    signatureBlob->Release();
    if (localErrorBlob)
    {
        localErrorBlob->Release();
    }
    return hr;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource,
                                         D3D12_RESOURCE_STATES beforeState,
                                         D3D12_RESOURCE_STATES afterState)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = resource;
    return barrier;
}

} // namespace terrain::d3d12
