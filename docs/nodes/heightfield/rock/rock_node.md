# Rock ノード

入力ハイトフィールドに「土のかぶっていない岩肌」を被せる加工ノードです。ジッタード Voronoi 格子を **scatter 生成器として** 使い、岩中心を擬 Poisson 分布で配置。各岩は **メートル単位の固有サイズ・回転・アスペクト比** を持ち、サイズは scatter 間隔から完全に独立しています。岩同士は自由に重なり、ピクセルごとに最大寄与で max 合成 — GeoGen の Rock のような **離散的に散らばった岩塊カバー** が出ます。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `Heightmap` |
| 出力 | `Heightmap`(凹凸を加算したハイトフィールド) / `Mask`(岩らしさの 0..1 マスク) |

## 主な設定

| 設定 | 既定値 | 役割 |
| --- | --- | --- |
| `Seed` | 0 | ハッシュオフセット |
| `Density (m)` | 8.0 | 岩中心のばらまき間隔 (m)。岩同士の **中心間距離** を決める |
| `Coverage (%)` | 100 | scatter 点が岩になる確率 |
| `Rock Size Min (m)` | 5.0 | 各岩がランダムに選ぶ **最小直径 (m)** |
| `Rock Size Max (m)` | 10.0 | 各岩がランダムに選ぶ **最大直径 (m)**。Density より大きいと岩が重なる |
| `Rock Height (m)` | 1.5 | 岩塊の最大盛り上がり |
| `Height Jitter (%)` | 50 | 岩ごとの高さ振れ幅(0=均一、1=0×〜2×) |
| `Rotation Variation (%)` | 100 | 各岩のランダム回転量 |
| `Aspect Variation (%)` | 30 | 各岩の細長さ振れ幅。0=円形、1=最大 2:1 |
| `Edge Sharpness (%)` | 100 | シルエット形状。0=円形ドーム、>0 で多角形(4–7 角)で完全クリップ。値はクリップ内部の高さ形状を radial / polyhedral でブレンド。1=完全に平らなダイヤモンドカット |
| `Bumpiness (%)` | 60 | 表面ディテールの振幅 |
| `Facet Sharpness (%)` | 50 | 表面形状。0=丸い凸凹、1=多面体状の平らな面 + 鋭いエッジ |
| `Facet Scale` | 2.5 | 1 つの岩に乗る面の細かさ |
| `Backend` | GPU Compute | `CPU` / `GPU Compute` (D3D12 compute shader) を切り替え。既定は GPU。シェーダーコンパイル/ディスパッチ失敗時は CPU に自動フォールバック |

## アルゴリズム概要

1. **Scatter**: ジッタード Voronoi 格子(セルピッチ = `Density` m)の各整数セルに seed を 1 個配置(`±0.45` セルの範囲でジッタ)。これは scatter のために使うだけで、岩のサイズはセルとは独立。
2. **メートル → セル単位換算**: `rockSizeMinCells = rockSizeMinM / density`。距離計算は内部的にセル単位で行う。
3. **走査範囲**: `searchRadius = max(1, ceil(rockSizeMaxCells × 0.5 × pow(2, aspectVar) - 0.05))`。最大ドーム半径とアスペクト最大伸長を考慮した最悪ケース。既定値で 3×3、巨石+細長設定で最大 ~7×7。
4. **各ピクセルで近傍セルを走査**:
   - seed 位置と距離 `d_iso` を算出。`d_iso ≥ maxReach` なら早期スキップ。
   - カバレッジ判定: `hash(gx, gz) > coverage` ならスキップ。
   - **岩ごとのパラメータをハッシュから決定**:
     - `rockSizeCells = lerp(rockSizeMinCells, rockSizeMaxCells, hash)` → `domeRadius_per`
     - 回転角 `θ = (hash - 0.5) × 2π × rotationVariation`
     - アスペクト `aspect = pow(2, aspectVariation × (2×hash − 1))`(対称な乗法レンジ、面積保存)。アスペクト軸 `aspect_x` ∈ {`aspect`, `1/aspect`} を別ハッシュで選び、`aspect_z = 1 / aspect_x`
   - **ローカル座標変換**: 回転 → アスペクト除算で楕円距離 `d_local` を計算。`d_local ≥ domeRadius_per` ならスキップ。
   - **ドーム高さ(多角形ハードクリップ + 内部ブレンド)**:
     - 円形成分 `radialT = 1 - d_local / domeRadius_per`
     - `edgeSharpness > 0` のときは岩ごとに 4–7 辺の凸多角形 SDF を計算。各辺の法線角は `(2π/N) × i + jitter`、辺の中心からの距離(inradius)は `domeRadius_per × cos(π/N) × (1 - hash × 0.3)`。**多角形の外側のピクセルは即 continue でこの岩の寄与なし(ハードクリップ)**。
     - 内側成分 `polyhedralT`: SDF の内部距離(正で内側)を `baseInradius` で正規化して 0..1 に。
     - ブレンド: `t = (1 - edgeSharpness) × radialT + edgeSharpness × polyhedralT`(クリップ内部の高さ形状を radial / polyhedral で混ぜる)
     - 高さ: `dome = pow(t, 1 + facetSharpness × 1.5 × (1 - edgeSharpness))`(`edgeSharpness` 高で線形に → 平らな三角形ファセットが直立、`facetSharpness` 高で頂点が尖る)
     - `cellHeight = rockHeight × heightJitterFactor`
   - **ファセット場**: ローカル `(rx, rz)` を `facetScale` 倍した位置 + 岩ごとのランダムオフセットで sub-Voronoi(F1, F2, セル座標)を取り、滑らかバンプ項とフラット面+クレース項を `facetSharpness` でブレンド。岩ごとに独立した面パターンになる。
   - **採用**: `rockH = cellHeight × dome × (1 + bumpiness × surfaceMod)` が `bestRockH` を超えれば更新。
5. **書き戻し**: `grid.heights[c] += bestRockH`、`grid.mask[c] = bestDome`。

岩同士が重なる場合の境界線は max 合成によって自然に発生する折れ線になり、明示的な crack 彫り込みは不要(削除済み)。

## 用途の使い分け

| 目的 | パラメータの方向性 |
| --- | --- |
| GeoGen 風の散らばった岩塊カバー(参考画像) | `Density` 4-6m / `Rock Size` 3-12m / `Rotation` 1.0 / `Aspect` 0.4-0.6 / `Edge Sharpness` 0.8-1.0 / `Facet Sharpness` 0.6+ |
| 細かい多面体岩肌(垂直崖) | `Density` 2-4m / `Rock Size` 2-5m / `Aspect` 0.2 / `Edge Sharpness` 0.9 / `Facet Sharpness` 0.7+ / `Facet Scale` 3-4 |
| 巨石を密集 | `Density` 6-10m / `Rock Size` 12-30m / `Aspect` 0.3-0.5 / `Rock Height` 3-8m |
| 散らばった岩 | `Density` 15m+ / `Coverage` 0.3-0.5 / `Rock Size` 6-10m |
| 結晶質クリスタル風 | `Density` 6m / `Rock Size` 5-7m / `Aspect` 0.3-0.5 / `Facet Sharpness` 1.0 / `Facet Scale` 1.5-2.5 / `Bumpiness` 0.8+ |
| 滑らかな丘の追加 | `Bumpiness` 0 / `Facet Sharpness` 0 / `Rock Height` 小さめ |

## メモ

- 出力は **加算**(地形がせり上がる)です。`Mask Blend` で他のマスクと合成して、特定領域だけ岩肌を出す使い方が想定。
- ファセット場は岩ローカルの回転・アスペクト座標系で評価され、岩ごとに固有のオフセットが乗ります。隣り合う岩で同じ面パターンが見えることはありません。
- Density より十分小さい Rock Size を選ぶと岩の間に隙間ができ、Density より大きいと岩同士が重なって連続的なカバーになります。両方混ぜたいときは Min を Density の半分以下、Max を Density の倍以上にすると幅広いサイズの岩が混在します。
- プロジェクト互換: 旧キーは自動マイグレーション(`ratio × density = m`)。
  - `rockFill` (3.5.x ratio) → `Min = Max = rockFill × density`
  - `rockSize` (3.6.0 ratio) → `Min = Max = rockSize × density`
  - `rockSizeMin/Max` (3.7.0 ratio) → `× density` で m に
  - `rockSizeMinM/MaxM` (3.8.0+ m) → そのまま
- `Crack Depth` (旧) は削除しました。max 合成で接合線の折れ線は自然に出るので明示的な彫り込みは不要、加えて Voronoi セル境界が岩境界と一致しなくなったので意味を失っていました。
- キャッシュは入力ハッシュ + パラメータハッシュで他ノードと同じく個別キャッシュ。`Backend` もハッシュに含まれるので CPU/GPU 切り替え時は再評価されます。

## GPU Compute バックエンド

`Backend` プルダウンで `GPU Compute` を選ぶと [shaders/rock_compute.hlsl](../../../../shaders/rock_compute.hlsl) の compute shader (`CSRock`、`[numthreads(8,8,1)]`) で評価します。アルゴリズムは CPU 版と完全に同等で、ハッシュ関数 (`Hash2` / `HashFloat01`) と Voronoi (`VoronoiF1F2`) を HLSL に直接移植してあるため、同一パラメータで CPU と GPU は同じ結果を返します。

ピクセルあたり embarrassingly parallel (近傍 (2*searchRadius+1)² セル走査 + per-rock 計算) で、reduction も pixel-local の max のみ。1024² 既定パラメータでおおむね **CPU 比 10-30 倍高速** の見込み (Sediment / Multi-Scale Erosion GPU 化と同オーダー)。

**バッファ構成:**

| スロット | 種類 | 役割 |
| --- | --- | --- |
| `u0` | `RWStructuredBuffer<float>` | InputHeights (UAV としてアップロード後シェーダー内で読み取り) |
| `u1` | `RWStructuredBuffer<float>` | OutputHeights = `inputH + bestRockH` |
| `u2` | `RWStructuredBuffer<float>` | OutputMask = `bestDome` |
| `b0` | 32-bit constants × 20 | resolution / seed / 各種クランプ済みパラメータ + 派生値 (`searchRadius` / `maxReach` / `domeExp`) |

`searchRadius` / `maxReach` / `domeExp` などの派生値は CPU 側 (`RunRockComputeImmediate`) で事前計算してから CB にパックします。多角形 SDF ループは facetCount = 4..7 のため [loop] + 早期 break で展開しています。

非同期評価スレッドから呼ばれた場合はメインスレッド側のキュー (`g_pendingRockGpuRequests`) に投げて `std::promise` で結果を待ちます (Sediment / Mask Noise / Multi-Scale Erosion と同じパターン)。
