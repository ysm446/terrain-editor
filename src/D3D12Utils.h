#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>

namespace terrain::d3d12
{

void ThrowIfFailed(HRESULT hr, const char* message);
D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type);
D3D12_RESOURCE_DESC BufferResourceDesc(UINT64 byteSize, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
D3D12_RESOURCE_DESC Texture2DResourceDesc(UINT width,
                                          UINT height,
                                          DXGI_FORMAT format,
                                          D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
D3D12_DESCRIPTOR_HEAP_DESC DescriptorHeapDesc(
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT descriptorCount,
    D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
D3D12_DESCRIPTOR_HEAP_DESC ShaderVisibleCbvSrvUavDescriptorHeapDesc(UINT descriptorCount);
UINT DefaultShaderCompileFlags();
HRESULT CreateRootSignatureFromDesc(ID3D12Device* device,
                                    const D3D12_ROOT_SIGNATURE_DESC& desc,
                                    ID3D12RootSignature** rootSignature,
                                    ID3DBlob** errorBlob = nullptr);
D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource,
                                         D3D12_RESOURCE_STATES beforeState,
                                         D3D12_RESOURCE_STATES afterState);
D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource = nullptr);

} // namespace terrain::d3d12
