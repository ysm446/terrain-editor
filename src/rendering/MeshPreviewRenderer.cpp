#include "MeshPreviewRenderer.h"

namespace terrain::rendering
{

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
