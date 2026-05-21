# 天球とボリューム雲

このドキュメントは Terrain Editor の天球 (大気散乱) とボリューム雲システムの概要、内部実装、パラメータの意味をまとめたものです。実装は太陽の向きだけで「青空 → 黄昏 → 夕焼け → 夜」が連動し、雲・地形の照明・遠景フォグが一貫して動く UE5 風のシーン環境を目指しています。

## 出典 / 参照

| 区分 | 内容 |
| --- | --- |
| 単散乱 (sky) | Nishita 1993, *Display Method of the Sky Color Taking into Account Multiple Scattering* |
| 多重散乱 LUT | Sébastien Hillaire 2020, *A Scalable and Production Ready Sky and Atmosphere Rendering Technique* — UE5 の Sky Atmosphere の元手法 |
| 雲のレイマーチ | Schneider 2015 / Horizon Zero Dawn 系のフルスクリーンレイマーチ + 3D 密度テクスチャ |
| Henyey-Greenstein | 雲・大気の前方散乱位相関数の標準モデル |

## 全体の構成

```
   太陽方向 (azimuth, elevation)
              │
              ▼
   ┌──────────────────────────┐
   │ 大気散乱モデル            │  ← shaders/atmosphere.hlsli
   │  (Rayleigh + Mie 単散乱) │     atmosphere_cpu (CPU port)
   └──────────────────────────┘
        │             │
        │ GPU         │ CPU
        ▼             ▼
  shaders/sky.hlsl   AtmosphereSamples
  ├ ピクセル毎       (zenith, horizon, ground, sun の 4 方向)
  └ 多重散乱 LUT を   │
    各ステップで参照  │
        │             ▼
        │       CloudShadowMeshConstants (b1)
        │       ├─ skyZenithColor / horizonColor / groundColor
        │       └─ skySunColor (大気透過後)
        │
        │      ┌──────────────────────────┐
        │      │ mesh_preview.hlsl PSSurface│
        │      │ ├ Hemisphere ambient       │
        │      │ ├ Direct sun (skySunColor) │
        │      │ ├ Cloud shadow (depth-aware)│
        │      │ └ Aerial perspective fog   │
        │      └──────────────────────────┘
        │
        ▼ (フルスクリーンパス、深度認識)
  shaders/cloud_render.hlsl
  ├ 3D 雲密度ボリューム (cloud_density.hlsl で生成)
  ├ レイマーチ (大気透過率で照明)
  └ skySunColor / skyZenithColor で雲頂・雲底の色を整合

  shaders/cloud_shadow.hlsl
  └ 太陽方向の top-down レイマーチで 1024² 雲影テクスチャを焼き
    mesh shader が地形頂点 → 雲底面の投影位置でサンプル
```

レンダ順は以下のとおり ([src/main.cpp](../../src/main.cpp) `RenderGpuMeshPreview`):

1. **Shadow map pass** — 太陽方向から見た地形デプスを焼く
2. **Sky pass** — フルスクリーン三角形で天球を描画 (depth disabled)
3. **Mesh pass** — 地形を depth test 付きで描画 (PBR 半球ライティング + 太陽 + 影 + 雲影 + aerial perspective)
4. **Cloud shadow pass** — 1024² 雲影テクスチャを compute shader で生成 (mesh pass の前)
5. **Cloud render pass** — 深度を SRV として読み、雲をフルスクリーン α ブレンドで合成

## SkyMode

`rock::SkyMode` ([src/node_graph.h](../../src/node_graph.h)) は 2 つ:

| モード | 説明 | 用途 |
| --- | --- | --- |
| `SolidColor` | 単色背景 (`PreviewSettings::viewportBackground`)。地形 ambient もこの色に統一。 | マスク確認・スクショなど中性背景が欲しいとき |
| `Atmospheric` | Nishita + Hillaire ベースの物理ベース天球。太陽角度で全部連動。 | プロジェクトのプレビュー・装飾用途 |

UI の表示モードドロップダウン (`シンプル` / `PBR` / `天球`) のうち「天球」が `Atmospheric` に対応します。

## 大気散乱の実装 (`shaders/atmosphere.hlsli`)

### 物理モデル

Nishita 単散乱を 2 種類の散乱で構成します:

- **Rayleigh 散乱** (`β_R = (5.802, 13.558, 33.1) × 10⁻⁶ m⁻¹`)
  - 1/λ⁴ 依存 → 青色の散乱が赤の約 5.7 倍強い
  - 昼の青空、夕焼けの赤色 (青が散乱で抜けて赤が残る) の正体
  - 高度プロファイル: `exp(-h / 7994 m)` (Rayleigh scale height ≈ 8 km)
- **Mie 散乱** (`β_M = 21 × 10⁻⁶ m⁻¹`)
  - 波長依存ほぼ無し → 白く散乱
  - 太陽周辺のグロー・地平の haze・サンセットの暖色味
  - 高度プロファイル: `exp(-h / 1200 m)` (Mie scale height ≈ 1.2 km、地表近くだけ濃い)

### 位相関数

レイの方向と太陽方向のなす角を θ として:

- **Rayleigh**: `phase_R(θ) = (3 / 16π) × (1 + cos²θ)` — 前方散乱・後方散乱が等しい対称形
- **Mie (Henyey-Greenstein)**: `phase_M(θ, g) = (1 - g²) / (4π × (1 + g² - 2g cos θ)^1.5)`
  - `g` (mieEccentricity) は前方散乱の鋭さ。0 で等方、0.7-0.85 で現実的な前方ピーク。

### 単散乱の積分 (`AtmComputeScattering`)

カメラ位置 `o` から視線方向 `viewDir` に向かって、大気上端まで `kAtmNumViewSteps = 32` ステップで積分:

```
for each step i along view ray:
    p = o + viewDir * t_i
    h = altitude at p
    dR, dM = density × stepLen at altitude h
    opticalR += dR; opticalM += dM
    
    sun_optical_depth = ray_march(p → sun, kAtmNumSunSteps = 8)
    
    transmittance = exp(- (β_R × (opticalR + sunR) + β_M × (opticalM + sunM) × 1.1))
    sumR += transmittance × dR
    sumM += transmittance × dM
    
    sumMS += transmittance_view × multi_scatter_LUT(h, cos sun zenith) × (β_R × dR + β_M × dM)

return sun_intensity × (sumR × β_R × phase_R + sumM × β_M × phase_M) + sumMS
```

サンプリング位置は `kAtmCameraHeight = 500 m` を仮の視点高度に固定しています。海抜 1m から地平方向を見ると光路が異常に長くて Rayleigh reddening が極端になるため、地形エディタの「山にいる」という典型シーンに合わせた値です。

### 多重散乱 LUT (`shaders/atmosphere_multiscatter.hlsl`)

単散乱だけだと「日中でも地平が暖色寄り」「太陽の反対側に光が回らない」という Hillaire 論文で指摘されるアーティファクトが出ます。これを補正する 32×32 RGBA16F LUT:

- **U 軸**: `cos(sun zenith)` ∈ [-1, 1] (太陽の高度)
- **V 軸**: `altitude / atmosphereThickness` ∈ [0, 1]
- **値**: 無限次散乱の漸近値 (近似)

各テクセルで以下を計算:

1. その (高度, 太陽角度) の点を中心に、64 方向 (球面均等サンプル、golden angle spiral) にレイマーチ
2. 各方向の単散乱輝度 `L_i` と「等方的な単位輝度の周回光がここに戻ってくる比率」`F_i` を別個に積分
3. 球面平均: `L_avg = Σ L_i / 64`、`F_avg = Σ F_i / 64`
4. 等比級数 (Hillaire): `multi_scatter = L_avg / (1 - F_avg)` で無限次散乱の漸近値

これを sky shader が各レイマーチステップでサンプルし、in-scattering に加算します。LUT は **`mieStrength` / `mieEccentricity` / `atmosphereDensity` のいずれかが変更されたときだけ再生成** (キャッシュ済みなら無視)、~0.5ms。

### 太陽の自動色

太陽ディスクの色は手動指定ではなく、太陽方向の大気透過率 `T_sun = exp(-tau_sun)` × 白色スペクトル で自動計算します。これにより:

- 高度 60-90°: ほぼ白
- 高度 10-15°: 暖色 (黄〜オレンジ)
- 高度 0° 付近: 赤橙、暗い
- 高度 < 0°: 透過率 0 → 太陽消失 (夜)

`AtmComputeSunTransmittance` が単純な太陽方向のみのレイマーチで透過率を返します。

## 地形ライティングへの統合

CPU 側 `SampleAtmosphericEnvironment` が 4 方向だけ atmosphere model を呼び出して環境色を抽出:

| 用途 | 方向 | 計算 |
| --- | --- | --- |
| `zenithColor` | `(0, 1, 0)` (天頂) | `AtmComputeScattering(up, sun)` |
| `horizonColor` | 太陽と直交する水平方向 | `AtmComputeScattering(perp, sun)` |
| `groundColor` | (擬似) 下半球の bounce | `0.6 × zenith + 0.4 × horizon` を groundAlbedo で乗算 |
| `sunColor` | 太陽方向 | `AtmComputeSunTransmittance(sun)` |

これらが `CloudShadowMeshConstants` cbuffer (b1) に詰められて mesh shader に届きます。`mesh_preview.hlsl` の半球 ambient ライティングはこれを使います:

- **法線が上向き** (n.y > 0): `lerp(horizonColor, zenithColor, n.y)` をサンプル → 山頂は青空色を浴びる
- **法線が下向き** (n.y < 0): `lerp(horizonColor, groundColor, -n.y)` → 谷底などには地面 bounce 色
- **直射光**: `skySunColor × N · L` → 太陽の自動色がそのまま地形を照らす
- **影側 fill**: `skyGroundColor × shadowAmount` → 影の中に地面 bounce が回る

これで天球の色を変える / 太陽を動かす と地形全体の照明が一貫して連動します。

## ボリューム雲

雲システムは 4 つのコンポーネントで構成:

### 1. 雲密度ボリューム (`shaders/cloud_density.hlsl`)

128³ R8_UNORM の 3D テクスチャに 3D 周期 Perlin fBM を焼きます。

- **周期 Perlin** (`Perlin3DPeriodic`): 整数座標を `mod period` してから勾配ハッシュを引くので、`uvw = 0` と `uvw = 1` で勾配配置が一致 → テクスチャがシームレスにタイリング
- **2 スケールの fBM**: `Fbm3DPeriodic(uvw × 4, period=4)` で大きな雲塊形状 + `Fbm3DPeriodic(uvw × 12, period=12)` で細かいディテール
- **キャッシュ**: `seed` が変わったときのみ再生成 (~1ms)

### 2. 雲のレンダリング (`shaders/cloud_render.hlsl`)

フルスクリーン pixel shader でカメラから雲帯までレイマーチ:

```
ray = reconstruct from screen coords + camera basis

# 円柱形 (slab × disc) との交差で march 範囲を決める
tEnter, tExit = intersect_slab_and_disc(ray, [altMin, altMax], fieldRadius)
# 地形深度で打ち切り (山が雲を遮蔽)
tExit = min(tExit, depth_buffer_to_world_distance)

# adaptive step: 雲帯厚みベースで最低 qualitySamples、grazing 角度では最大 4×
numSteps = clamp(ceil((tExit - tEnter) / idealStep), q, 4q)
stepLen = (tExit - tEnter) / numSteps
jitter = hash(input.pos.xy)  # ピクセル毎に 0-1 のジッターでバンディング解消

phase = HG(g = phaseEccentricity, cosθ = dot(ray, sunDir))  # ピクセル毎に 1 回

for each step:
    p = camera + ray × (tEnter + (i + jitter) × stepLen)
    density = SampleCloudDensity(p)  # field fade × vertical profile × 3D noise × coverage
    if density > 0:
        # 雲底寄りの ambient 項(半球光 + 地面 bounce、軽く脱飽和)
        skyTerm = atmosphereSkyColor × 1.5
        skyTerm = lerp(skyTerm, luminance(skyTerm), 0.35)
        ambient = cloudColor × (skyTerm + atmosphereSunColor × 0.5)
        sunlit = cloudColor × atmosphereSunColor

        if lightSamples > 0:
            # 太陽方向に lightSamples ステップ進めて密度を Beer-Lambert 積分
            # (= サンプル点から太陽までの自己遮蔽)
            lightDensity = Σ SampleCloudDensity(p + sunDir × lightStepMeters × (j + 0.5))
            lightTransmittance = exp(-lightDensity × absorption × lightStepMeters)
            lit = ambient + sunlit × lightTransmittance × phase
        else:
            # フォールバック: yNorm ベースの上下ランプ (旧挙動)
            lit = lerp(ambient, sunlit, yNorm)

        dT = exp(-density × absorption × stepLen)  # Beer-Lambert (視線方向)
        dA = (1 - dT) × transmittance
        accumulated += lit × dA
        transmittance *= dT
        if transmittance < 0.01: break

return float4(accumulated, 1 - transmittance)  # premultiplied alpha
```

ブレンドステートは `ONE / INV_SRC_ALPHA` (premultiplied alpha) で空に合成。`SRC_ALPHA / INV_SRC_ALPHA` で書くと alpha が二重に乗算されて薄い雲が空より暗くなります。

#### 雲のフィールド境界

雲は無限に地平まで続かないように、地形中心を原点とする円形の **field disc** で囲まれています:

- `fieldRadius`: 雲が存在する半径 (m)
- `fieldFalloff`: 端のフェード幅。saturate(1 - (dist - inner) / falloff) で 0 へ滑らかに減衰
- レイマーチ範囲もこの disc とスラブの交差で求めるので、grazing 角度のレイにも自然な打ち切りが入ります

#### 雲頂・雲底の色 (大気連動)

雲のシェーディングは「ambient(半球光 + 地面 bounce)」と「direct sun(太陽光 × 自己遮蔽 × 位相)」の足し合わせです。

- **ambient 項**: `cloudColor × (atmosphereSkyColor × 1.5 + atmosphereSunColor × 0.5)`
  - `× 1.5`: 天頂色を半球輝度に近似スケール (zenith 1 点だと過小評価のため)。multi-scatter LUT 導入で天頂値自体が以前より明るいので、過去の `× 2.5` から下げています。
  - `lerp(skyTerm, luminance, 0.35)`: 雲内部 Mie 多重散乱で色が脱飽和する効果の近似 (= 雲底が氷のような青ではなく、自然な灰色寄りになる)。強すぎると夕焼けの暖色が消えるので 0.35 程度に抑えています。
- **direct 項** (`lightSamples > 0` 時): `cloudColor × atmosphereSunColor × lightTransmittance × phase`
  - `lightTransmittance`: サンプル点から太陽方向に `lightSamples` ステップ進んで Beer-Lambert で積分した透過率。雲塊の奥に行くほど暗くなり、これが「ボリューム感のある陰影」を生みます。
  - `phase`: Henyey-Greenstein 位相関数 `(1 - g²) / (1 + g² - 2g cosθ)^1.5` (4π 正規化)。`g = phaseEccentricity = 0.4` で太陽方向側がピーク ~3.9× / 影側 ~0.3× の差が付き、シルバーライニングと逆光時の縁の暗化が出ます。
- **`lightSamples = 0`** ではこの直接光 + 自己遮蔽が無効化され、旧バージョンと同じ `lerp(ambient, sunlit, yNorm)` の上下ランプにフォールバックします (比較用)。

### 3. 雲影 (`shaders/cloud_shadow.hlsl`)

地形フットプリント上空に置いた 1024² R8_UNORM 視点から太陽方向にレイマーチして雲帯の透過率を 2D テクスチャに焼きます (毎フレーム再生成、~0.5ms)。

mesh shader 側の `ComputeCloudShadowVisibility` が地形頂点 (x, z, y) を太陽方向に altitudeMin まで投影した位置で透過率を読み、direct sun と ambient に乗算します。低い太陽角度では影が長く伸び、太陽が真上だと影が真下に短くなる挙動。

### 4. CloudSettings の主要パラメータ

[src/node_graph.h](../../src/node_graph.h) の `CloudSettings`:

| パラメータ | 意味 | 典型値 |
| --- | --- | --- |
| `enabled` | 雲の有効/無効 | true |
| `seed` | ノイズシード (再生成トリガ) | 1 |
| `coverage` | 雲が空に占める割合 | 0.55 |
| `densityMultiplier` | 不透明度倍率 | 1.0 |
| `altitudeMin` / `altitudeMax` | 雲帯の高度範囲 (m) | 1500 / 3500 |
| `horizontalScale` | 雲塊の水平スケール (m) | 4000 |
| `absorption` | Beer-Lambert 吸収係数 | 0.06 |
| `color` | 雲のベース色 (アルベド) | (1, 1, 1) |
| `loopPhase` | 雲タイル一周の位相。0 と 1 は同じ位置 | 0 |
| `windDirectionDegrees` / `windSpeedMetersPerSec` | 風 (アニメーション) | 45° / 0 m/s |
| `qualitySamples` | レイマーチサンプル数 | 32 |
| `shadowStrength` | 雲影の強さ | 0.7 |
| `shadowResolution` / `shadowSamples` | 雲影テクスチャ解像度 / 太陽方向サンプル | 1024 / 16 |
| `fieldRadius` / `fieldFalloff` | フィールド境界 | 6000 / 2000 |
| `lightSamples` | 自己遮蔽の太陽方向ライトマーチ段数 (0 で無効化) | 6 |
| `lightStepMeters` | ライトマーチ 1 ステップの距離 (m)。総投光距離 = `lightSamples × lightStepMeters` | 80 |
| `phaseEccentricity` | HG 位相関数の g 値。0 等方、正値で前方散乱(シルバーライニング) | 0.4 |

## 自動遠景フォグ / 地平ヘイズ (Aerial Perspective)

地形の遠景に大気の霞を重ねる効果。独立した強度スライダーは持たず、`atmosphereDensity` と `mieStrength` から自動で決まります。`mesh_preview.hlsl` PSSurface の最後で適用:

```hlsl
viewDist = length(worldPos - cameraPos)
fogExtinction = atmosphereDensity × (45e-6 + mieStrength × 12e-6)
fogFactor = saturate(1 - exp(-viewDist × fogExtinction))

# 視線方向別のフォグ色。空シェーダーの地平色をそのまま使うので
# 空と地形遠景の合流ラインで色がずれない。太陽方向に近いレイは
# 太陽色を少し混ぜて夕焼け方向の地平に温度を持たせる。
fogColor = lerp(skyHorizonColor, skySunColor, sunDirectionTerm)

col = lerp(col, fogColor, fogFactor)
```

クリップなしの素直な Beer-Lambert 減衰なので、十分に遠い地形は完全にフォグ色に飽和します。`atmosphereDensity` を上げると空の Rayleigh 散乱と一緒に遠景フォグも強くなり、Mie は太陽方向の温度と太陽グローに主に効きます。空シェーダーの地平帯も同様に、地平の輝度を捏造せずに微妙に脱飽和+クール側へ寄せるだけにしてあります。

## SkySettings パラメータ一覧

[src/node_graph.h](../../src/node_graph.h):

| パラメータ | 意味 | 典型値 |
| --- | --- | --- |
| `mode` | `SolidColor` / `Atmospheric` | SolidColor (デフォルト) |
| `atmosphereDensity` | Rayleigh 係数 β_R 倍率。空の濃さ。 | 1.0 (地球標準) |
| `mieStrength` | Mie 係数倍率。haze 量 | 0.2 |
| `mieEccentricity` | HG g 値。太陽グローの鋭さ | 0.76 |
| `groundAlbedo` | 地面の概算反射色 | (0.3, 0.3, 0.3) |
| `sunSizeDegrees` | 太陽ディスクの直径 | 2.5° |
| `sunGlowStrength` | 太陽周辺グローの強さ | 0.3 |

## D3D12 リソースとパス

| リソース | 種類 | サイズ | 生成タイミング |
| --- | --- | --- | --- |
| Multi-scatter LUT | 2D RGBA16F | 32×32 | mie/density 変更時 |
| Cloud density volume | 3D R8_UNORM | 128×128×128 (2 MB) | seed 変更時 |
| Cloud shadow texture | 2D R8_UNORM | 1024×1024 | 毎フレーム |
| Mesh depth (R32_TYPELESS) | DSV + SRV | viewport size | RT 再作成時 |

LUT・3D テクスチャは `g_srvHeap` の slot を 1 つずつ確保。雲影と深度 SRV も同様。Compute pass の UAV は per-call の専用 descriptor heap を使うので、メイン RT 系の SRV ヒープと干渉しません。

## ファイル構成

| ファイル | 内容 |
| --- | --- |
| [shaders/atmosphere.hlsli](../../shaders/atmosphere.hlsli) | Nishita 単散乱 + 多重散乱 LUT サンプリングの共通ヘッダ |
| [shaders/atmosphere_multiscatter.hlsl](../../shaders/atmosphere_multiscatter.hlsl) | 多重散乱 LUT 生成 compute shader |
| [shaders/sky.hlsl](../../shaders/sky.hlsl) | フルスクリーン天球パス |
| [shaders/cloud_density.hlsl](../../shaders/cloud_density.hlsl) | 周期 Perlin による 3D 密度ボリューム生成 |
| [shaders/cloud_render.hlsl](../../shaders/cloud_render.hlsl) | フルスクリーン雲レイマーチ + 大気連動シェーディング |
| [shaders/cloud_shadow.hlsl](../../shaders/cloud_shadow.hlsl) | 太陽方向 top-down レイマーチで雲影テクスチャ生成 |
| [shaders/mesh_preview.hlsl](../../shaders/mesh_preview.hlsl) | 地形 PBR + 雲影 + aerial perspective を統合 |
| [src/main.cpp](../../src/main.cpp) | パイプライン管理、CPU port (atmosphere_cpu)、CB 充填、UI |
| [src/node_graph.h](../../src/node_graph.h) | `SkyMode` / `SkySettings` / `CloudSettings` 構造体定義 |

## よくある質問 / トラブルシューティング

### 日中なのに地平が暖色寄り

Nishita 単散乱の既知のアーティファクトです。`atmosphere_multiscatter.hlsl` の LUT で大半が緩和されますが、完全には消えません。空シェーダー側ではさらに地平帯 (`abs(ray.y) ≤ 0.20`) で太陽が高い時 (`smoothstep(0.05, 0.30, sun.y)` で gating) に微妙に脱飽和+クール側へ寄せています。`mieStrength` は 0.2 前後が日中の編集ビューでは扱いやすく、値を上げるほど太陽方向の霞・グローと一緒に地平の暖色帯も出やすくなります。低い太陽高度では物理的にも暖色が増えるため、昼の確認は Sun Elevation を 30° 以上にして切り分けてください。

### 雲が暗すぎる/明るすぎる

ambient 項は `atmosphereSkyColor × 1.5 + atmosphereSunColor × 0.5` (脱飽和 lerp 0.35)、direct 項は `atmosphereSunColor × lightTransmittance × phase`(`lightSamples > 0` 時)。**夜になっても雲がうっすら明るい場合**は `atmosphereSunColor` の最低値を見直す (現状は太陽が地平下なら 0)。**夕焼け時に雲が灰色のまま**な場合は脱飽和 lerp を 0.2 程度まで下げる、または `phaseEccentricity` を 0.5〜0.7 に上げて太陽方向のシルバーライニングを強める。**雲全体が真っ白でのっぺり**な場合は `lightSamples` を 6 以上に上げ、`absorption` も 0.06 前後に上げると自己遮蔽がはっきりします。

### 雲がのっぺりして立体感が無い

`lightSamples = 0` になっていないか確認してください(0 だと旧バージョンの上下ランプのみ = 自己遮蔽無し)。標準は 6、薄い雲が多い場合は 4 でも十分、厚い雲塊に陰影を強く出したい場合は 8〜12。`lightStepMeters` は雲スケール(`horizontalScale`)に対して短すぎると雲塊の中まで届かず、長すぎるとサンプリングが粗くなります。`horizontalScale = 4000` なら `lightStepMeters = 60〜120` あたりが扱いやすい範囲です。

### 雲が地平に薄く伸びて見える / 横線がある

レイマーチのサンプリング不足です。`qualitySamples` を 64 まで上げると改善。`cloud_render.hlsl` には grazing 角度で自動的に最大 4× にスケールアップする処理 + ピクセル毎ジッターが入っているので通常は十分。

### 太陽の角度を変えても天球が更新されない

`SkyMode` が `SolidColor` のままになっていないか確認。表示メニュー → 天球モードに切り替えてください。

### 大気密度を上げたら遠景が白っぽい

`atmosphereDensity` は空の散乱と地形の自動遠景フォグに連動します。地平の厚みを出すために密度を上げすぎると、遠くの地形は白っぽくなります。雲だけが原因か切り分ける場合は、いったん `mieStrength` を下げるか、雲を非表示にして確認してください。
