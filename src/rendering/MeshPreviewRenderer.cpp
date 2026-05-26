#include "MeshPreviewRenderer.h"

#include <d3dcompiler.h>

#include "../D3D12Utils.h"

namespace terrain::rendering
{
namespace
{
using Microsoft::WRL::ComPtr;
using terrain::d3d12::CreateRootSignatureFromDesc;
using terrain::d3d12::DefaultShaderCompileFlags;
} // namespace

bool EnsureMeshPreviewPipeline(MeshPreviewPipelineResources& resources,
                               const MeshPreviewPipelineContext& context,
                               std::string* error)
{
    if (resources.surfacePso && resources.renderTargetFormat == context.renderTargetFormat) return true;
    if (resources.surfacePso && resources.renderTargetFormat != context.renderTargetFormat)
    {
        resources.surfacePso.Reset();
        resources.waterPso.Reset();
        resources.wirePso.Reset();
        resources.gridPso.Reset();
        resources.shadowPso.Reset();
        resources.rootSignature.Reset();
        resources.renderTargetFormat = DXGI_FORMAT_UNKNOWN;
    }
    if (!context.device)
    {
        if (error) *error = "D3D12 device not initialized";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE meshResourceRange{};
    meshResourceRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    meshResourceRange.NumDescriptors = 8; // t0 shadow, t1 cloud shadow, t2/t3 displacement, t4 Colorize, t5 AO, t6 scene color, t7 scene depth
    meshResourceRange.BaseShaderRegister = 0;
    meshResourceRange.RegisterSpace = 0;
    meshResourceRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // Root parameter budget: mesh constants + 2 (cloud shadow CBV)
    // + 1 (mesh resource table). The caller owns the mesh constant layout.
    D3D12_ROOT_PARAMETER rootParams[3]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = context.rootConstantDwordCount;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &meshResourceRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister = 0;
    samplers[0].RegisterSpace = 0;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ShaderRegister = 1;
    samplers[1].RegisterSpace = 0;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = CreateRootSignatureFromDesc(context.device,
                                             rsDesc,
                                             resources.rootSignature.ReleaseAndGetAddressOf(),
                                             errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create mesh preview root sig failed";
        return false;
    }

    const UINT compileFlags = DefaultShaderCompileFlags();
    ComPtr<ID3DBlob> vsBlob, psBlob, psEdgeBlob, psWaterBlob, vsShadowBlob;
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", compileFlags, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile mesh VS failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSSurface", "ps_5_0", compileFlags, 0, &psBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile mesh PS failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSEdge", "ps_5_0", compileFlags, 0, &psEdgeBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile mesh edge PS failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSWater", "ps_5_0", compileFlags, 0, &psWaterBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile mesh water PS failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSShadow", "vs_5_0", compileFlags, 0, &vsShadowBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile mesh shadow VS failed"; return false; }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = resources.rootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.InputLayout = {inputLayout, 4};
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = context.renderTargetFormat;
    psoDesc.DSVFormat = context.depthStencilFormat;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // The terrain grid winding presents the visible top side as D3D's back face.
    // Cull front faces so the underside of the heightfield is not drawn.
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.surfacePso));
    if (FAILED(hr)) { if (error) *error = "Create mesh surface PSO failed"; return false; }

    psoDesc.PS = {psWaterBlob->GetBufferPointer(), psWaterBlob->GetBufferSize()};
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.waterPso));
    if (FAILED(hr)) { if (error) *error = "Create mesh water PSO failed"; return false; }

    psoDesc.PS = {psEdgeBlob->GetBufferPointer(), psEdgeBlob->GetBufferSize()};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.wirePso));
    if (FAILED(hr)) { if (error) *error = "Create mesh wire PSO failed"; return false; }

    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.gridPso));
    if (FAILED(hr)) { if (error) *error = "Create mesh grid PSO failed"; return false; }

    psoDesc.VS = {vsShadowBlob->GetBufferPointer(), vsShadowBlob->GetBufferSize()};
    psoDesc.PS = {};
    psoDesc.NumRenderTargets = 0;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.RasterizerState.DepthBias = 1200;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.shadowPso));
    if (FAILED(hr)) { if (error) *error = "Create mesh shadow PSO failed"; return false; }

    resources.renderTargetFormat = context.renderTargetFormat;
    return true;
}

bool EnsureMeshPreviewDisplacementPipeline(MeshPreviewPipelineResources& resources,
                                           const MeshPreviewPipelineContext& context,
                                           std::string* error)
{
    if (resources.displacementSurfacePso && resources.displacementRenderTargetFormat == context.renderTargetFormat) return true;
    if (resources.displacementSurfacePso && resources.displacementRenderTargetFormat != context.renderTargetFormat)
    {
        resources.displacementSurfacePso.Reset();
        resources.displacementShadowPso.Reset();
        resources.displacementWirePso.Reset();
        resources.displacementSectionPso.Reset();
        resources.displacementSectionShadowPso.Reset();
        resources.displacementSectionWirePso.Reset();
        resources.displacementTessSurfacePso.Reset();
        resources.displacementTessShadowPso.Reset();
        resources.displacementTessWirePso.Reset();
        resources.displacementRootSignature.Reset();
        resources.displacementRenderTargetFormat = DXGI_FORMAT_UNKNOWN;
    }
    if (!context.device) { if (error) *error = "D3D12 device not initialized"; return false; }

    // Persistent CBV upload buffer for mesh constants. Aligned to 256 bytes
    // (CB requirement) and filled per-draw via memcpy. One instance is
    // sufficient since the GPU consumes it before the next draw of this
    // pass; no in-flight overlap to worry about.
    if (!resources.displacementCbv)
    {
        const UINT64 cbSize = context.displacementCbvByteSize;
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = cbSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = context.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&resources.displacementCbv));
        if (FAILED(hr)) { if (error) *error = "Create mesh displacement CBV failed"; return false; }
    }

    D3D12_DESCRIPTOR_RANGE shadowRange{};
    shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors = 1;
    shadowRange.BaseShaderRegister = 0; // t0
    shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE cloudShadowRange{};
    cloudShadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cloudShadowRange.NumDescriptors = 1;
    cloudShadowRange.BaseShaderRegister = 1; // t1
    cloudShadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE heightRange{};
    heightRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    heightRange.NumDescriptors = 1;
    heightRange.BaseShaderRegister = 2; // t2
    heightRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE maskRange{};
    maskRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    maskRange.NumDescriptors = 1;
    maskRange.BaseShaderRegister = 3; // t3
    maskRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE colorRange{};
    colorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    colorRange.NumDescriptors = 1;
    colorRange.BaseShaderRegister = 4; // t4
    colorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE aoRange{};
    aoRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    aoRange.NumDescriptors = 1;
    aoRange.BaseShaderRegister = 5; // t5
    aoRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // Budget: 2 (mesh CBV) + 2 (cloud shadow CBV) + 8 (displacement consts)
    // + 1*6 (6 SRV tables) = 18 DWORDs of 64.
    D3D12_ROOT_PARAMETER rootParams[9]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[2].Constants.ShaderRegister = 2;
    rootParams[2].Constants.Num32BitValues = context.displacementConstantDwordCount;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges = &shadowRange;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[4].DescriptorTable.pDescriptorRanges = &cloudShadowRange;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[5].DescriptorTable.pDescriptorRanges = &heightRange;
    rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[6].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[6].DescriptorTable.pDescriptorRanges = &maskRange;
    rootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[7].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[7].DescriptorTable.pDescriptorRanges = &colorRange;
    rootParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[8].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[8].DescriptorTable.pDescriptorRanges = &aoRange;
    rootParams[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister = 0;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ShaderRegister = 1;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = _countof(rootParams);
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = CreateRootSignatureFromDesc(context.device,
                                             rsDesc,
                                             resources.displacementRootSignature.ReleaseAndGetAddressOf(),
                                             errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create displacement root sig failed"; return false; }

    const UINT compileFlags = DefaultShaderCompileFlags();
    ComPtr<ID3DBlob> vsBlob, psBlob, psEdgeBlob, vsShadowBlob, vsSectionBlob, vsSectionShadowBlob, vsPatchBlob, hsPatchBlob, dsPatchBlob, dsPatchShadowBlob;
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSDisplacement", "vs_5_0", compileFlags, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile VSDisplacement failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSSurface", "ps_5_0", compileFlags, 0, &psBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile PSSurface (displacement) failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSEdge", "ps_5_0", compileFlags, 0, &psEdgeBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile PSEdge (displacement) failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSDisplacementShadow", "vs_5_0", compileFlags, 0, &vsShadowBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile VSDisplacementShadow failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSDisplacementSection", "vs_5_0", compileFlags, 0, &vsSectionBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile VSDisplacementSection failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSDisplacementSectionShadow", "vs_5_0", compileFlags, 0, &vsSectionShadowBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile VSDisplacementSectionShadow failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSDisplacementPatch", "vs_5_0", compileFlags, 0, &vsPatchBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile VSDisplacementPatch failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "HSDisplacement", "hs_5_0", compileFlags, 0, &hsPatchBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile HSDisplacement failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "DSDisplacement", "ds_5_0", compileFlags, 0, &dsPatchBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile DSDisplacement failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "DSDisplacementShadow", "ds_5_0", compileFlags, 0, &dsPatchShadowBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile DSDisplacementShadow failed"; return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = resources.displacementRootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    // No vertex buffer — VS reads SV_VertexID. Empty input layout.
    psoDesc.InputLayout = {nullptr, 0};
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = context.renderTargetFormat;
    psoDesc.DSVFormat = context.depthStencilFormat;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // The terrain grid winding presents the visible top side as D3D's back face.
    // Cull front faces so the underside of the heightfield is not drawn.
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.displacementSurfacePso));
    if (FAILED(hr)) { if (error) *error = "Create displacement surface PSO failed"; return false; }

    psoDesc.PS = {psEdgeBlob->GetBufferPointer(), psEdgeBlob->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthBias = -64;
    psoDesc.RasterizerState.SlopeScaledDepthBias = -0.25f;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.displacementWirePso));
    if (FAILED(hr)) { if (error) *error = "Create displacement wire PSO failed"; return false; }

    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    psoDesc.VS = {vsSectionBlob->GetBufferPointer(), vsSectionBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.HS = {};
    psoDesc.DS = {};
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = context.renderTargetFormat;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.displacementSectionPso));
    if (FAILED(hr)) { if (error) *error = "Create displacement section PSO failed"; return false; }

    psoDesc.PS = {psEdgeBlob->GetBufferPointer(), psEdgeBlob->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthBias = -64;
    psoDesc.RasterizerState.SlopeScaledDepthBias = -0.25f;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.displacementSectionWirePso));
    if (FAILED(hr)) { if (error) *error = "Create displacement section wire PSO failed"; return false; }

    psoDesc.VS = {vsSectionShadowBlob->GetBufferPointer(), vsSectionShadowBlob->GetBufferSize()};
    psoDesc.PS = {};
    psoDesc.NumRenderTargets = 0;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.DepthBias = 1200;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.displacementSectionShadowPso));
    if (FAILED(hr)) { if (error) *error = "Create displacement section shadow PSO failed"; return false; }

    // Shadow PSO — same root sig (so the shader can read displacement
    // constants + height texture), but writes only depth.
    psoDesc.VS = {vsShadowBlob->GetBufferPointer(), vsShadowBlob->GetBufferSize()};
    psoDesc.PS = {};
    psoDesc.NumRenderTargets = 0;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.RasterizerState.DepthBias = 1200;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.displacementShadowPso));
    if (FAILED(hr)) { if (error) *error = "Create displacement shadow PSO failed"; return false; }

    psoDesc.VS = {vsPatchBlob->GetBufferPointer(), vsPatchBlob->GetBufferSize()};
    psoDesc.HS = {hsPatchBlob->GetBufferPointer(), hsPatchBlob->GetBufferSize()};
    psoDesc.DS = {dsPatchBlob->GetBufferPointer(), dsPatchBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = context.renderTargetFormat;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.displacementTessSurfacePso));
    if (FAILED(hr)) { if (error) *error = "Create displacement tessellation surface PSO failed"; return false; }

    psoDesc.PS = {psEdgeBlob->GetBufferPointer(), psEdgeBlob->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthBias = -64;
    psoDesc.RasterizerState.SlopeScaledDepthBias = -0.25f;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.displacementTessWirePso));
    if (FAILED(hr)) { if (error) *error = "Create displacement tessellation wire PSO failed"; return false; }

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    psoDesc.DS = {dsPatchShadowBlob->GetBufferPointer(), dsPatchShadowBlob->GetBufferSize()};
    psoDesc.PS = {};
    psoDesc.NumRenderTargets = 0;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.RasterizerState.DepthBias = 1200;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;
    hr = context.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&resources.displacementTessShadowPso));
    if (FAILED(hr)) { if (error) *error = "Create displacement tessellation shadow PSO failed"; return false; }

    resources.displacementRenderTargetFormat = context.renderTargetFormat;
    return true;
}

void ResetMeshPreviewPipelineResources(MeshPreviewPipelineResources& resources)
{
    resources.surfacePso.Reset();
    resources.waterPso.Reset();
    resources.wirePso.Reset();
    resources.gridPso.Reset();
    resources.shadowPso.Reset();
    resources.rootSignature.Reset();

    resources.displacementSurfacePso.Reset();
    resources.displacementShadowPso.Reset();
    resources.displacementWirePso.Reset();
    resources.displacementSectionPso.Reset();
    resources.displacementSectionShadowPso.Reset();
    resources.displacementSectionWirePso.Reset();
    resources.displacementTessSurfacePso.Reset();
    resources.displacementTessShadowPso.Reset();
    resources.displacementTessWirePso.Reset();
    resources.displacementRootSignature.Reset();
    resources.displacementCbv.Reset();

    resources.rtvHeap.Reset();
    resources.dsvHeap.Reset();
    resources.renderTargetFormat = DXGI_FORMAT_UNKNOWN;
    resources.displacementRenderTargetFormat = DXGI_FORMAT_UNKNOWN;
}

} // namespace terrain::rendering
