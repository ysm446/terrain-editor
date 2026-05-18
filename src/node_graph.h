#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rock
{
using GraphId = int;

enum class NodeKind
{
    HeightmapLoad = 4,
    HeightmapBlur = 5,
    Shape = 6,
    MultiScaleErosion = 8,
    MaskNoise = 9,
    MaskBlend = 10,
    MaskFluvial = 11,
    Rock = 12,
    Sediment = 13,
    Snow = 14,
    Colorize = 15,
    MaskCurvature = 16,
    MaskLevels = 17,
    MaskSlope = 18,
    MaskHeight = 19,
    Crumbling = 20,
};

enum class PinKind
{
    Input,
    Output,
};

enum class ValueType
{
    Mesh = 1,
    HeightField = 2,
    Mask = 3,
    ColorTexture = 4,
};

enum class MultiScaleErosionBackend
{
    CpuReference,
    GpuCompute,
};

enum class SedimentBackend
{
    CpuReference,
    GpuCompute,
};

enum class RockBackend
{
    CpuReference,
    GpuCompute,
};

enum class RockStyle
{
    Classic,
    Polygonal,
    Shard,
};

enum class RockOrientationRule
{
    Flat,
    FollowGround,
    SlopeOriented,
};

enum class MaskFluvialBackend
{
    CpuReference,
    GpuCompute,
};

enum class MaskFluvialSimulationMode
{
    FlowAccumulation,
    Particles,
};

enum class SnowBackend
{
    CpuReference,
    GpuCompute,
};

enum class MaskNoiseBackend
{
    CpuParallel,
    GpuCompute,
};

enum class ColorizeBackend
{
    CpuParallel,
    GpuCompute,
};

enum class ShapeKind
{
    Hemisphere,
    Pyramid,
};

enum class MaskBlendMode
{
    Add,
    Multiply,
    Min,
    Max,
};

enum class MaskCurvatureMode
{
    Ridges,
    Valleys,
    Absolute,
};

enum class FlowAccumulationAlgorithm
{
    D8,
    MFD,
};

// How the 3D viewport renders the heightfield. CPU mesh builds an explicit
// vertex / triangle / edge mesh on the host and uploads it; GPU
// displacement uploads the heightfield as a texture and lets a vertex
// shader sample it on a static UV grid. The latter avoids re-uploading a
// large vertex buffer per parameter change at the cost of one extra
// texture upload (~few ms vs tens of ms).
enum class MeshPreviewBackend
{
    CpuMesh,
    GpuDisplacement,
};

enum class TerrainBoundaryMode
{
    None,
    SectionPolygon,
    Lines,
};

enum class SunDirectionMode
{
    Manual,
    DateTime,
};

// マスクプレビューのシェーディング方式。
// Grayscale: mask=0 を黒、mask=1 を白とする純粋な白黒ランプ (既定)。
// GrayOrange: ライティング付きのグレー×オレンジのトーン。
// GrayscaleHatched: グレースケール + 斜線オーバーレイ (GeoGen 風)。
//   mask が 1.0 付近: 白背景 + 密な白斜線。mask が 0.0 付近: 黒背景 +
//   疎な白斜線。中間域は標準のグレースケールランプ。マスクの飽和・
//   減衰具合をスクリーンスペースの対角線パターンで可視化します。
enum class MaskShadingMode
{
    Grayscale,
    GrayOrange,
    GrayscaleHatched,
};

enum class MaskFluvialOutputCurve
{
    // log(1 + accum) / log(1 + maxAdjusted), then pow(gamma). Continuous
    // dendritic visualization — the standard GIS "log flow accumulation"
    // look. Threshold acts as a noise floor (subtracted before normalize).
    Log,
    // smoothstep around accumulationThreshold, then pow(power). Sharp
    // binary river / non-river output for layering masks downstream.
    Threshold,
    // (accum - threshold) / max, then pow(gamma). Linear continuous,
    // preserves the long tail without log compression. Tends to overweight
    // the main trunks at the cost of fine branches.
    Linear,
};

enum class PreviewStage
{
    Graph = 3,
    HeightmapBlur = 4,
    Shape = 5,
    MultiScaleErosion = 7,
    MaskNoise = 8,
    MaskBlend = 9,
    MaskFluvial = 10,
    Rock = 11,
    Sediment = 12,
    Snow = 13,
    Colorize = 14,
    MaskCurvature = 15,
    MaskLevels = 16,
    MaskSlope = 17,
    MaskHeight = 18,
    Crumbling = 19,
};

enum class HeightfieldPreviewField
{
    Heightmap,
    Deposits,
    Flows,
    Age,
    Mask,
    UniqueMask,
};

struct Pin
{
    GraphId id = 0;
    GraphId nodeId = 0;
    PinKind kind = PinKind::Input;
    ValueType valueType = ValueType::HeightField;
    std::string label;
};

struct HeightmapLoadSettings
{
    std::string path;
    float scaleMeters = 1024.0f;
    float relativeVerticalScalePercent = 100.0f;
    float verticalOffsetMeters = 0.0f;
    int simulationResolution = 512;
};

struct ShapeSettings
{
    ShapeKind kind = ShapeKind::Hemisphere;
    float scaleMeters = 1024.0f;
    float relativeHeightPercent = 50.0f;
    int simulationResolution = 512;
};

struct HeightmapBlurSettings
{
    float radius = 3.0f;
    float strength = 1.0f;
    int iterations = 1;
};

struct MaskNoiseSettings
{
    int seed = 0;
    int octaves = 4;
    float frequency = 4.0f;
    float lacunarity = 2.0f;
    float persistence = 0.5f;
    int simulationResolution = 512;
    MaskNoiseBackend backend = MaskNoiseBackend::GpuCompute;
};

struct MaskBlendSettings
{
    MaskBlendMode mode = MaskBlendMode::Add;
    float intensity = 1.0f;
};

// Heightfield -> mask. Compares each height sample with a blurred local
// neighbourhood to detect convex ridges, concave valleys, or both as an
// absolute curvature mask. The output only writes the mask channel; the input
// heightfield itself is passed through unchanged for preview chaining.
struct MaskCurvatureSettings
{
    MaskCurvatureMode mode = MaskCurvatureMode::Absolute;
    int radius = 3;
    float sensitivityMeters = 1.0f;
    float threshold = 0.0f;
    float gamma = 1.0f;
};

struct MaskLevelsSettings
{
    float blackPoint = 0.0f;
    float whitePoint = 1.0f;
    float gamma = 1.0f;
    bool invert = false;
};

struct MaskSlopeSettings
{
    float slopeMinDeg = 25.0f;
    float slopeMaxDeg = 60.0f;
    float gamma = 1.0f;
    bool invert = false;
};

struct MaskHeightSettings
{
    bool useFullRange = false;
    float heightMinMeters = 0.0f;
    float heightMaxMeters = 1000.0f;
    float featherMeters = 0.0f;
    float gamma = 1.0f;
    bool invert = false;
};

struct CrumblingSettings
{
    int physicsCount = 48;
    float debrisAmount = 0.65f;
    float debrisSizeMinM = 2.0f;
    float debrisSizeMaxM = 8.0f;
    RockStyle style = RockStyle::Shard;
    float gravity = 0.75f;
    int seed = 0;
};

// Heightfield + mask. Scatters rocks on a jittered Voronoi grid (used
// purely as a deterministic Poisson-like scatter pattern) and lifts each
// scatter point into a rotated, possibly elongated dome with per-rock
// facet detail. Rock size is specified directly in metres and is
// independent of scatter spacing, so rocks freely overlap and pixels
// take the max rock contribution. The natural max-blend already
// produces creases where rocks meet — there is no separate crack
// carving step.
struct RockSettings
{
    RockStyle style = RockStyle::Polygonal; // New nodes default to sharper low-poly rocks; old projects without this key are migrated to Classic.
    RockOrientationRule orientationRule = RockOrientationRule::Flat;
    int layerCount = 1;
    int seed = 0;
    float density = 8.0f;            // Scatter pitch (m). Spacing between rock centres.
    float coverage = 1.0f;           // 0..1, fraction of scatter points that become a rock.
    float rockSizeMinM = 5.0f;       // Min rock diameter (m). Each rock samples uniformly from [min, max].
    float rockSizeMaxM = 10.0f;      // Max rock diameter (m). May be larger or smaller than density — overlap is handled by max-blend.
    float rockHeight = 1.5f;         // m, max bump height per rock.
    float heightJitter = 0.5f;       // 0..1, per-rock height variation (0 = uniform).
    float rotationVariation = 1.0f;  // 0..1, fraction of full 2π rotation each rock can take. 0 = aligned, 1 = full random.
    float aspectVariation = 0.3f;    // 0..1, per-rock aspect ratio variation. 0 = circular, 1 = up to 2:1 along a random axis.
    float edgeSharpness = 1.0f;      // 0 = circular silhouette (smooth dome). > 0 = polyhedral silhouette hard-clipped by 4–7 sided polygon; the value blends interior height between radial and polyhedral. 1 = pure flat-faceted polyhedron.
    float bumpiness = 0.6f;          // 0..1, surface detail amplitude (smooth or faceted, see facetSharpness).
    float facetSharpness = 0.5f;     // 0 = smooth dome with rounded bumps, 1 = polyhedral flat facets with sharp creases.
    float facetScale = 2.5f;         // Sub-cell Voronoi frequency in the rock-local frame (higher = more, smaller facets per rock).
    RockBackend backend = RockBackend::GpuCompute; // CPU reference vs GPU compute (D3D12). Defaults to GPU; falls back to CPU on shader/device failure.
};

// Heightfield → heightfield + mask. GeoGen-style sediment simulation.
// Lays a uniform layer of loose sediment on the bedrock (or treats the
// whole input as sediment if `convertTerrainToSediment`) and lets gravity
// redistribute it through multi-scale thermal sliding: at each scale
// (largest → cell-sized) sediment flows from cells whose slope to a
// neighbour exceeds the talus angle (= `sedimentViscosity`). Coarser
// scales settle large basins first, finer scales add detail. Output
// heights = bedrock + redistributed sediment; output mask = sediment
// thickness normalised by its maximum so valleys (thickly piled) are
// bright and ridges (denuded) are dark.
struct SedimentSettings
{
    int iterations = 40;                  // GeoGen "Iterations count". Outer relaxation passes.
    int stabilizationIterations = 2;      // GeoGen "Stabilization iterations". Inner sliding passes per scale per outer iteration.
    float largestDetailLevelM = 8.0f;     // GeoGen "Largest detail level" (m). Coarsest neighbour stride; halved each level down to one cell.
    float emissionAmountM = 0.5f;         // GeoGen "Emission amount" (m). Total sediment thickness added uniformly.
    float emissionTime = 0.0f;            // GeoGen "Emission time". 0..1, fraction of iterations over which the emission is gradually added; 0 = all up-front.
    float sedimentViscosity = 0.20f;      // GeoGen "Sediment viscosity". 0..1; controls the talus angle (low = fluid, high = stiff piles).
    bool convertTerrainToSediment = true; // GeoGen "Convert terrain to sediment". If true, the input height itself is treated as movable sediment over a flat bedrock = 0.
    float maskContrast = 0.0f;            // Mask output contrast (S-curve). 0 = linear, 1 = near-binary.
    SedimentBackend backend = SedimentBackend::GpuCompute; // CPU reference vs GPU compute (D3D12). Defaults to GPU; falls back to CPU on shader/device failure.
};

// Heightfield -> heightfield + mask. Drops a uniform "snowfall" thickness
// across the terrain and lets the local slope angle decide how much actually
// stays. Cells flatter than `slopeLimitMinDeg` keep all of `emissionAmount`,
// cells steeper than `slopeLimitMaxDeg` keep none, the band in between is a
// smoothstep blend. Heightmap output = input + snow thickness; mask output =
// snow thickness normalised against `maskMaxSnow`. GeoGen Snow ノードと同じ
// 「斜面に雪は積もらない」見た目を、シングルパスで作る簡易モデル。
struct SnowSettings
{
    float emissionAmount = 1.0f;       // m. 平地 (slope <= min) に降り積もる雪厚。
    float slopeLimitMinDeg = 50.0f;    // この角度以下では雪が満杯まで積もる。
    float slopeLimitMaxDeg = 60.0f;    // この角度以上では雪はまったく積もらない。間は smoothstep。
    float maskMaxSnow = 1.0f;          // m. mask 出力の正規化基準 (snow >= この値 → mask = 1.0)。
    // 雪の表面を反復的に「平滑化 + 溝埋め」する反復数。各反復で
    // snowSurface = heights + thickness の近傍 box blur を取り、
    // max(snowSurface, blurred) でセルを更新する。これにより:
    //   - 周囲より低いセル (= 溝の底) は雪が増えて埋まる
    //   - 周囲より高いセル (= 出っ張り) は変わらない
    //   - スロープ遷移域の per-cell な thickness 揺らぎが消える
    // 結果として「雪の envelope」が次第に滑らかになる。0 = 平滑化なし。
    int smoothingIterations = 8;
    float largestDetailLevelM = 8.0f; // m. GeoGen "Largest detail level". Controls the widest snow envelope smoothing scale.
    int fillRadius = 3; // Legacy saved setting. Largest Detail Level now drives envelope radius.
    SnowBackend backend = SnowBackend::GpuCompute;
};

// カラーグラデーションの色ストップ。position は 0..1 の範囲でグラデーション上の位置を指定する。
struct ColorStop
{
    float position = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// RGBA8 カラーテクスチャ出力。resolution x resolution の正方グリッドで、
// pixels は row-major 順で各ピクセル 4 バイト (R, G, B, A) を保持する。
struct ColorGrid
{
    int resolution = 0;
    std::vector<uint8_t> pixels; // RGBA8, row-major
};

// Heightmap (optional) + Base Color (optional) + Mask (optional) + Gradient Mask → Color Texture.
// Gradient Mask の各ピクセル値 (0..1) をグラデーション上の参照位置として色を決定し、
// Base Color がある場合は Mask を合成強度として上書き合成する。Heightmap は 3D プレビュー用の地形形状にのみ使用。
struct ColorizeSettings
{
    std::vector<ColorStop> stops = {{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}};
    ColorizeBackend backend = ColorizeBackend::GpuCompute;
};

// Heightfield -> mask. Performs MFD flow accumulation on the
// input heights and emits a mask. The default Log curve produces the
// classic continuous dendritic drainage tree (every cell visible, fine
// branches dim, main trunks bright). Switch to Threshold for sharp
// binary river extraction or Linear for a non-log continuous map.
struct MaskFluvialSettings
{
    MaskFluvialSimulationMode simulationMode = MaskFluvialSimulationMode::FlowAccumulation;
    FlowAccumulationAlgorithm algorithm = FlowAccumulationAlgorithm::MFD; // Legacy serialized field. Mask Fluvial now evaluates as MFD.
    MaskFluvialOutputCurve outputCurve = MaskFluvialOutputCurve::Log;
    // In Log/Linear modes: noise floor — cells with fewer upstream cells
    // than this fraction of the grid get clipped to 0. Default 0 shows
    // the full tree.
    // In Threshold mode: the actual river threshold (0.005 ≈ 0.5%
    // gives a clean main-channel mask).
    float accumulationThreshold = 0.0f;
    float gamma = 0.5f;           // Log/Linear curve exponent (lower = brighter leaves)
    float softness = 0.15f;       // Threshold mode: smoothstep transition width
    float power = 1.6f;           // Threshold mode: pow(mask, power) for edge taper
    int pitFillIterations = 8;    // Legacy serialized setting. Internally fixed to the default.
    float largestDetailLevelM = 8.0f; // m. Low-pass scale for flow-direction analysis; larger values ignore smaller terrain wrinkles.
    float mfdExponent = 4.0f;     // Flow concentration. Higher = more channelised, lower = more distributed.
    float inertia = 0.0f;         // Legacy serialized setting. Internally fixed to the default.
    int particleCount = 32768;    // Particle mode: number of deterministic droplets traced across the heightfield.
    int particleLifetime = 96;    // Particle mode: maximum trail length in steps.
    float particleInertia = 0.55f; // Particle mode: higher keeps direction, lower follows local slope more eagerly.
    float particleStepLengthM = 4.0f; // Particle mode: distance travelled per step in metres.
    int particleSeed = 1337;
    MaskFluvialBackend backend = MaskFluvialBackend::GpuCompute; // CPU 厳密 (sort + topological walk) vs GPU 反復 (Jacobi gather, ~2*resolution iters; 視覚的に同等だが数値は完全一致せず). 既定 GPU. シェーダー / ディスパッチ失敗時は CPU に自動フォールバック.
};

struct MultiScaleErosionSettings
{
    int iterations = 50;
    bool enableStreamPower = true;
    bool enableThermal = true;
    bool enableDeposition = true;
    // Multi-grid pyramid: run progressively from a coarse grid up to the
    // target resolution, with bilinear upsampling between levels. Drainage
    // networks form quickly at coarse scales (path_length / cellSize is
    // small) and finer levels only refine, giving near-resolution-invariant
    // results. Disable to fall back to a single-resolution simulation.
    bool useMultigrid = true;

    // Stream Power Erosion (erosion.glsl)
    float speStrength = 0.004f;         // k
    float streamExponent = 0.9f;        // p_sa
    float slopeExponent = 2.0f;         // p_sl
    float maxStreamPower = 10000.0f;    // max_spe
    float flowExponent = 1.3f;          // flow_p
    float speTimeStep = 1.0f;           // dt

    // Thermal (thermal.glsl)
    float thermalAngleDegrees = 30.0f;  // tanThresholdAngle (in degrees)
    float thermalStrength = 0.005f;     // eps
    bool thermalNoisifyAngle = true;
    float thermalNoiseMin = 0.9f;
    float thermalNoiseMax = 1.4f;
    float thermalNoiseWavelength = 0.0023f;

    // Deposition (deposition.glsl)
    float depositionStrength = 0.2f;
    float rain = 2.6f;

    MultiScaleErosionBackend backend = MultiScaleErosionBackend::CpuReference;
};

struct Node
{
    GraphId id = 0;
    NodeKind kind = NodeKind::HeightmapLoad;
    std::string title;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    HeightmapLoadSettings heightmap;
    ShapeSettings shape;
    HeightmapBlurSettings heightmapBlur;
    MultiScaleErosionSettings multiScaleErosion;
    MaskNoiseSettings maskNoise;
    MaskBlendSettings maskBlend;
    MaskCurvatureSettings maskCurvature;
    MaskLevelsSettings maskLevels;
    MaskSlopeSettings maskSlope;
    MaskHeightSettings maskHeight;
    CrumblingSettings crumbling;
    MaskFluvialSettings maskFluvial;
    RockSettings rock;
    SedimentSettings sediment;
    SnowSettings snow;
    ColorizeSettings colorize;
};

struct Link
{
    GraphId id = 0;
    GraphId startPin = 0;
    GraphId endPin = 0;
};

struct PreviewSettings
{
    float terrainSizeMeters = 1024.0f;
    int simulationResolution = 512;
    int resolution = 512;
    int lod = 0;
    bool showSurface = true;
    bool showWireframe = false;
    bool showGrid = true;
    int lightingMode = 0;
    MeshPreviewBackend meshBackend = MeshPreviewBackend::CpuMesh;
    TerrainBoundaryMode terrainBoundaryMode = TerrainBoundaryMode::SectionPolygon;
    bool viewportTessellation = false;
    float tessellationMinFactor = 1.0f;
    float tessellationMaxFactor = 8.0f;
    float tessellationNearDistance = 450.0f;
    float tessellationFarDistance = 4500.0f;
    bool depthOfFieldEnabled = false;
    float dofFStop = 5.6f;
    float dofFocusDistanceMeters = 1200.0f;
    float dofSensorHeightMm = 24.0f;
    float dofMaxBlurPixels = 14.0f;
    int dofApertureShape = 0; // 0 circle, 1 triangle, 2 hexagon, 3 octagon, 4 custom blades
    int dofApertureBlades = 6;
    float dofApertureRotationDegrees = 0.0f;
    float dofHighlightBoost = 0.0f;
    float sunAzimuthDegrees = 315.0f;
    float sunElevationDegrees = 38.0f;
    float sunIntensity = 1.05f;
    float ambientStrength = 0.38f;
    float shadowStrength = 0.36f;
    int shadowMapResolution = 2048;
    float shadowBias = 0.0035f;
    SunDirectionMode sunDirectionMode = SunDirectionMode::Manual;
    float sunLatitudeDegrees = 35.0f;
    float sunLongitudeDegrees = 139.0f;
    float sunUtcOffsetHours = 9.0f;
    int sunMonth = 6;
    int sunDay = 21;
    float sunTimeHours = 14.0f;
    std::array<float, 3> pbrAlbedo = {0.80f, 0.80f, 0.80f};
    int gridCellCount = 10;
    float gridCellSizeMeters = 100.0f;
    std::array<float, 3> gridColor = {0.2f, 0.2f, 0.2f};
    std::array<float, 3> viewportBackground = {0.268f, 0.268f, 0.268f};
    MaskShadingMode maskShading = MaskShadingMode::Grayscale;
    bool maskPreviewUseNearestHeightmap = false;
};

enum class SkyMode
{
    SolidColor,
    Atmospheric,
};

// Atmospheric (Nishita single-scatter Rayleigh + Mie). The sky / sun /
// terrain ambient / cloud lighting are all derived from the same model
// driven only by sun direction so the whole scene transitions through
// day -> sunset -> night coherently when sun elevation is animated.
struct SkySettings
{
    SkyMode mode = SkyMode::SolidColor;
    float atmosphereDensity = 1.0f;            // multiplies the Rayleigh β coefficients — overall atmosphere "thickness"
    float mieStrength = 0.2f;                  // turbidity / haze (multiplies Mie scattering coefficient)
    float mieEccentricity = 0.76f;             // Henyey-Greenstein g — sun-glow tightness, 0 = isotropic, 0.9 = sharp
    std::array<float, 3> groundAlbedo = {0.30f, 0.30f, 0.30f};
    float sunSizeDegrees = 2.5f;
    float sunGlowStrength = 0.3f;
};

struct CloudSettings
{
    bool enabled = false;
    int seed = 1;
    float coverage = 0.55f;
    float densityMultiplier = 1.0f;
    float altitudeMin = 1500.0f;
    float altitudeMax = 3500.0f;
    float horizontalScale = 4000.0f;
    float absorption = 0.06f;
    std::array<float, 3> color = {1.0f, 1.0f, 1.0f};
    bool animate = false;
    float windDirectionDegrees = 45.0f;
    float windSpeedMetersPerSec = 0.0f;
    int qualitySamples = 32;
    float shadowStrength = 0.7f;
    int shadowResolution = 1024;
    int shadowSamples = 16;
    float fieldRadius = 6000.0f;
    float fieldFalloff = 2000.0f;
    // Volumetric self-shadowing: each view sample marches a few steps
    // toward the sun and accumulates density to compute a Beer-Lambert
    // light transmittance. 0 disables (back to pure top-down ramp).
    int lightSamples = 6;
    float lightStepMeters = 80.0f;
    // Henyey-Greenstein eccentricity for the cloud phase function. 0.4 gives
    // a gentle silver lining around the sun direction; 0.0 is isotropic.
    float phaseEccentricity = 0.4f;
};

struct GraphSettings
{
    PreviewSettings preview;
    SkySettings sky;
    CloudSettings clouds;
};

struct MeshVertex
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float nx = 0.0f;
    float ny = 1.0f;
    float nz = 0.0f;
    float mask = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

struct MeshTriangle
{
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
};

struct MeshEdge
{
    uint32_t a = 0;
    uint32_t b = 0;
};

struct MeshData
{
    std::vector<MeshVertex> vertices;
    std::vector<MeshTriangle> triangles;
    std::vector<MeshEdge> edges;
};

struct HeightfieldGrid
{
    int resolution = 0;
    float terrainSizeMeters = 1.0f;
    std::vector<float> heights;
    std::vector<float> mask;
    std::vector<float> uniqueMask;
    std::vector<float> deposits;
    std::vector<float> flows;
    std::vector<float> age;
};

struct MaskGrid
{
    int resolution = 0;
    std::vector<float> values;
};

using MultiScaleErosionGpuEvaluator = bool (*)(HeightfieldGrid& grid, const MultiScaleErosionSettings& settings, std::string* error);
using MaskNoiseGpuEvaluator = bool (*)(MaskGrid& grid, const MaskNoiseSettings& settings, std::string* error);
using SedimentGpuEvaluator = bool (*)(HeightfieldGrid& grid, const SedimentSettings& settings, std::string* error);
using RockGpuEvaluator = bool (*)(HeightfieldGrid& grid, const RockSettings& settings, std::string* error);
using MaskFluvialGpuEvaluator = bool (*)(HeightfieldGrid& grid, const MaskFluvialSettings& settings, std::string* error);
using SnowGpuEvaluator = bool (*)(HeightfieldGrid& grid, const SnowSettings& settings, std::string* error);
using ColorizeGpuEvaluator = bool (*)(ColorGrid& grid, const ColorizeSettings& settings, const MaskGrid& gradientMask, const MaskGrid* mask, const ColorGrid* baseColor, std::string* error);

struct HeightfieldPipeline
{
    struct HeightfieldOperation
    {
        enum class Kind
        {
            HeightmapBlur,
            MultiScaleErosion,
            MaskCurvature,
            MaskSlope,
            MaskHeight,
            Crumbling,
            MaskFluvial,
            Rock,
            Sediment,
            Snow,
        };

        Kind kind = Kind::HeightmapBlur;
        GraphId nodeId = 0;
        HeightmapBlurSettings heightmapBlur;
        MultiScaleErosionSettings multiScaleErosion;
        MaskCurvatureSettings maskCurvature;
        MaskSlopeSettings maskSlope;
        MaskHeightSettings maskHeight;
        CrumblingSettings crumbling;
        MaskFluvialSettings maskFluvial;
        RockSettings rock;
        SedimentSettings sediment;
        SnowSettings snow;
    };

    bool hasSource = false;
    int simulationResolution = 512;
    float terrainSizeMeters = 1024.0f;
    GraphId heightmapNodeId = 0;
    HeightmapLoadSettings heightmap;
    bool useShape = false;
    GraphId shapeNodeId = 0;
    ShapeSettings shape;
    std::vector<HeightfieldOperation> heightfieldOperations;
};

struct EvaluationSummary
{
    uint64_t version = 0;
    bool dirty = true;
    std::string status = "Graph has not been evaluated";
    PreviewStage previewStage = PreviewStage::Graph;
    GraphId previewNodeId = 0;
    GraphId previewPinId = 0;
    bool previewShowsMask = false;
    HeightfieldPreviewField previewField = HeightfieldPreviewField::Heightmap;
    std::string previewMessage;
    HeightfieldGrid previewHeightfield;
    MeshData previewMesh;
    ColorGrid previewColorGrid;
    bool previewIsColor = false;
};

class NodeGraph
{
public:
    static NodeGraph CreateDefaultTerrainGraph();

    const std::vector<Node>& Nodes() const;
    const std::vector<Link>& Links() const;
    GraphSettings& Settings();
    const GraphSettings& Settings() const;
    const EvaluationSummary& Evaluation() const;

    const Pin* FindPin(GraphId pinId) const;
    const Node* FindNode(GraphId nodeId) const;
    Node* FindMutableNode(GraphId nodeId);
    const Node* FindUpstreamForPin(GraphId pinId) const;
    bool IsInputPin(GraphId pinId) const;
    bool IsOutputPin(GraphId pinId) const;
    bool PinHasLink(GraphId pinId) const;
    bool CanCreateLink(GraphId startPin, GraphId endPin) const;

    bool CreateLink(GraphId startPin, GraphId endPin);
    bool DeleteLink(GraphId linkId);
    GraphId CreateNode(NodeKind kind);
    bool DeleteNode(GraphId nodeId);
    void ReplaceNodes(std::vector<Node> nodes);
    void ReplaceLinks(std::vector<Link> links);
    bool SetPreviewStage(PreviewStage stage);
    bool SetPreviewNode(GraphId nodeId);
    bool SetPreviewPin(GraphId pinId);
    PreviewStage Preview() const;
    HeightfieldPipeline PipelineFor(PreviewStage stage) const;
    HeightfieldPipeline PreviewPipeline() const;
    void MarkDirty(std::string_view reason);
    void SetEvaluationPending(std::string_view status);
    void ApplyEvaluationResultFrom(const NodeGraph& evaluatedGraph);
    void Evaluate(int previewMeshResolution = 0);

private:
    GraphId AllocateGraphId();
    void RebuildNextGraphId();
    GraphId AddNode(NodeKind kind, std::string title);
    GraphId AddPin(GraphId nodeId, PinKind kind, ValueType valueType, std::string label);
    void AddInitialLink(GraphId startPin, GraphId endPin);
    const Node* FindFirstNode(NodeKind kind) const;
    const Node* FindNodeByOutputPin(GraphId pinId) const;
    const Node* FindUpstreamNode(const Node& node) const;
    struct UpstreamConnection
    {
        const Node* node = nullptr;
        const Pin* outputPin = nullptr;
    };
    UpstreamConnection FindUpstreamConnectionForPin(GraphId pinId) const;
    HeightfieldPipeline PipelineTo(NodeKind targetKind) const;
    HeightfieldPipeline PipelineToNode(const Node& targetNode) const;
    const Node* FindNearestHeightfieldForMaskPreview(const Node& maskNode) const;
    HeightfieldGrid EvaluateMaskAsHeightfield(const Node& node, std::string* message);
    MaskGrid EvaluateMaskGridForNodeCached(const Node& node, int depth, uint64_t* outputHash, std::string_view outputLabel = {});
    HeightfieldGrid EvaluateHeightPipelineCached(const HeightfieldPipeline& pipeline, std::string* message, HeightfieldPreviewField previewField, uint64_t* outputHash);
    MeshData BuildMeshFromHeightPipelineCached(const HeightfieldPipeline& pipeline, int resolution, std::string* message, HeightfieldPreviewField previewField = HeightfieldPreviewField::Heightmap, HeightfieldGrid* previewGrid = nullptr);

    struct HeightfieldNodeCache
    {
        bool valid = false;
        int resolution = 0;
        uint64_t inputHash = 0;
        uint64_t parameterHash = 0;
        uint64_t outputHash = 0;
        HeightfieldGrid grid;
        std::string message;
    };

    struct MaskNodeCache
    {
        bool valid = false;
        uint64_t inputHash = 0;
        uint64_t parameterHash = 0;
        uint64_t outputHash = 0;
        MaskGrid grid;
    };

    struct ColorNodeCache
    {
        bool valid = false;
        uint64_t inputHash = 0;
        uint64_t parameterHash = 0;
        uint64_t outputHash = 0;
        ColorGrid grid;
    };

    struct MeshNodeCache
    {
        bool valid = false;
        int resolution = 0;
        uint64_t inputHash = 0;
        HeightfieldPreviewField previewField = HeightfieldPreviewField::Heightmap;
        MeshData mesh;
    };

    ColorGrid EvaluateColorGridForNodeCached(const Node& node, int depth, uint64_t* outputHash);

    std::vector<Node> nodes_;
    std::vector<Link> links_;
    GraphSettings settings_;
    std::unordered_map<GraphId, HeightfieldNodeCache> heightfieldCache_;
    std::unordered_map<GraphId, MaskNodeCache> maskCache_;
    std::unordered_map<GraphId, ColorNodeCache> colorCache_;
    std::unordered_map<GraphId, MeshNodeCache> meshCache_;
    EvaluationSummary evaluation_;
    GraphId nextGraphId_ = 1;
};

std::string_view ToString(ShapeKind kind);
std::string_view ToString(MaskBlendMode mode);
std::string_view ToString(NodeKind kind);
std::string_view ToString(PreviewStage stage);
std::string_view ToString(ValueType type);
PreviewStage PreviewStageFor(NodeKind kind);
bool IsMaskOnlyNodeKind(NodeKind kind);
bool IsColorOnlyNodeKind(NodeKind kind);
void SetMultiScaleErosionGpuEvaluator(MultiScaleErosionGpuEvaluator evaluator);
void SetMaskNoiseGpuEvaluator(MaskNoiseGpuEvaluator evaluator);
void SetSedimentGpuEvaluator(SedimentGpuEvaluator evaluator);
void SetRockGpuEvaluator(RockGpuEvaluator evaluator);
void SetMaskFluvialGpuEvaluator(MaskFluvialGpuEvaluator evaluator);
void SetSnowGpuEvaluator(SnowGpuEvaluator evaluator);
void SetColorizeGpuEvaluator(ColorizeGpuEvaluator evaluator);

// Thread-safe progress signal: holds the GraphId of the node whose
// evaluation kernel is currently running on a worker thread, or 0 when
// no kernel is active. Updated only on cache misses (cache hits are
// instantaneous so the badge wouldn't be visible anyway). The UI thread
// reads this to draw a "計算中" badge that walks the upstream chain in
// real time as the pipeline progresses.
std::atomic<GraphId>& CurrentlyEvaluatingNodeId();

} // namespace rock
