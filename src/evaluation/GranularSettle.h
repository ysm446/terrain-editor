#pragma once

#include "../node_graph.h"

#include <vector>

namespace rock
{
// Snow / Soil が共有する粒状物質の再配分コア。
// 物質を厚みとして注入し、安息角 (motionSlopeLimitDeg) を超えた面から
// 最も急な低い近傍へ移動して安定させる。coverage 変換した mask と
// base + thickness の heights を grid に書き込む。
struct GranularSettleParams
{
    float emissionAmount = 1.0f;         // m. 平地に注入する総厚。
    int iterationCount = 40;             // simulation step 数。
    float emissionTime = 0.0f;           // 0 = 最初に全量、1 = 全 step に均等配分。
    int settlingPasses = 4;              // 各 step 内の再配分パス数。
    float motionSlopeLimitDeg = 35.0f;   // この角度以下の物質面では流れない。
    float transportRate = 0.45f;         // 不安定分のうち 1 パスで動かす割合。
    float surfaceSmoothing = 0.0f;       // 0..1。堆積面のみを平滑化する強さ。
    float maskThresholdM = 0.02f;        // coverage mask が白へ向かう厚み。
    float maskFeatherM = 0.015f;         // coverage mask 境界のグレー幅。
    float largestDetailLevelM = 8.0f;    // m. 移動先探索の最大ストライド。
    float slopeDependentEmission = 0.0f; // 0..1。基盤傾斜に応じて注入を減らす (0 = 一様注入)。
};

// grid.heights を base + 最終厚みへ、grid.mask を coverage へ更新する。
// outThickness が非 null なら最終厚みも書き出す (Thickness マスク用)。
void ApplyGranularSettle(HeightfieldGrid& grid, const GranularSettleParams& params, std::vector<float>* outThickness = nullptr);
} // namespace rock
