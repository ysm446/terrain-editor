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
    if (resources.surfacePso) return true;
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
}

} // namespace terrain::rendering
