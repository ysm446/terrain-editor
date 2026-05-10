# マスクテクスチャー描画

このドキュメントは、3D メッシュプレビューにおけるマスクテクスチャー (`HeightfieldGrid::mask`) の表示処理についてまとめたものです。マスクは `Mask Fluvial` の流量分布 / `Sediment` の堆積量 / `Multi-Scale Erosion` の流線・堆積などを `[0, 1]` 正規化した可視化チャネルで、地表をマスクで塗り直す `マスクプレビュー` モードでの描画ロジックを 3 つのマスクシェーディングモードに分けて解説します。

## 全体像

```
   HeightfieldGrid (heights / mask / deposits / flows / age)
          │
          ▼
   HeightfieldPreviewField で 1 ch 選択 → mask 配列に書き込む
          │
          ▼
   ┌─────────────────────────────────────────┐
   │  シェーダー入力                            │
   │  ・地表頂点    : mask = SampleHeightfieldValue(grid.mask, ...) │
   │  ・側壁頂点    : mask = kWallMaskSentinel (= 2.0f)             │
   └─────────────────────────────────────────┘
          │
          ▼
   shaders/mesh_preview.hlsl PSSurface
   ├ if (i.mask > 1.5)        → 壁面 sentinel  (モード別グレーで一色塗り)
   ├ if (mask < 0.5)          → モード 0: グレースケール (純白黒ランプ)
   ├ if (mask > 1.5 && < 2.5) → モード 1: グレー×オレンジ (ライティング付き)
   └ if (mask > 2.5)          → モード 2: グレースケール + 斜線 (ハッチ + ハーフランバート)
```

## マスクプレビューに入る経路

地表頂点の `mask` は CPU メッシュビルダー [src/node_graph.cpp `BuildMeshFromHeightfield`](../../src/node_graph.cpp) と GPU ディスプレースメント VS [shaders/mesh_preview.hlsl `VSDisplacement`](../../shaders/mesh_preview.hlsl) のどちらかで設定されます。GPU パスでは `displacementMask` テクスチャから直接サンプル、CPU パスでは `SampleHeightfieldValue(grid.mask, ...)` でテクスチャ補間します。

PS 側で `cbuffer Constants` の `maskPreview` (= 0/1 のフラグ) を見て、マスクプレビュー分岐に入るかどうかを決めます。`maskPreview > 0.5` のとき、地表は通常のライティングをスキップしてマスクのみを表示します。

## マスクシェーディングモード

`PreviewSettings::maskShadingMode` ([src/node_graph.h](../../src/node_graph.h)) は 3 値で、`Constants::maskShadingMode` として PSSurface に渡ります。

| 値 | モード | 概要 |
| --- | --- | --- |
| 0 | `グレースケール` | 純粋な白黒ランプ (mask 値そのまま RGB) |
| 1 | `グレー×オレンジ` | 暗部=ニュートラルグレー, 明部=暖オレンジのライティング付きシェーディング |
| 2 | `グレースケール + 斜線` | GeoGen 風の対角ハッチング + ハーフランバート陰影 |

UI では `表示設定 > マスクテクスチャー > マスクシェーディング` プルダウンから選択します。

### モード 0: グレースケール

```hlsl
return float4(mask, mask, mask, 1.0);
```

何もしません。`mask=0 → 黒` / `mask=1 → 白` の純粋な線形ランプ。ガンマ補正もリムも乗りません。**2D マップ表示と完全に一致する見た目**になるよう敢えて加工なしにしています。マスクの形状を素朴に確認したいときの基準モード。

### モード 1: グレー×オレンジ

```hlsl
float3 lowMask  = (0.18, 0.20, 0.21);   // 暗部のニュートラルグレー
float3 highMask = (0.95, 0.56, 0.18);   // 明部の暖オレンジ
baseColor       = lerp(lowMask, highMask, mask);
col             = baseColor * (ambient + key * 0.65 + fill * 0.18 + sky * 0.5);
col            += pow(mask, 2.2) * (0.42, 0.20, 0.05);  // 高マスク域の暖色グロー
```

3 灯 (key/fill/sky) のシンプル直接光ライティングを乗算したモード。3D 起伏が読み取れるので **マスクの分布と地形の関連を同時に確認**できます。

### モード 2: グレースケール + 斜線

GeoGen ([https://www.geogen.org/](https://www.geogen.org/)) の地形図表示を模した、対角ハッチング + ハーフランバート陰影モード。マスクの**飽和域 (クリッピング) を可視化しつつ、3D 形状も陰影で読み取れる**のが目的です。

#### ハッチパターン

```hlsl
if (mask >= 0.99 || mask <= 0.01) {
    int2 px        = int2(i.pos.xy);
    int  stripeIdx = (px.x + px.y) & 3;       // 4 ストライプ周期
    bool isMinor   = (stripeIdx == 3);         // 4 本中 1 本だけ反転
    bool isHigh    = (mask >= 0.99);
    float majorVal = isHigh ? 1.0 : 0.0;
    float stripeVal = isHigh ? 0.5 : 1.0;     // 白側はグレー縞、黒側は白縞
    c              = isMinor ? stripeVal : majorVal;
} else {
    c = mask;                                  // 中間域は通常ランプ
}
```

| マスク値 | 表示 |
| --- | --- |
| `mask >= 0.99` (白に張り付き) | **白×3 + グレー(0.5)×1** の 3:1 縞 |
| `mask <= 0.01` (黒に張り付き) | **黒×3 + 白(1.0)×1** の 3:1 縞 |
| 中間域 (`0.01 < mask < 0.99`) | 通常のグレースケールランプ (縞なし) |

ストライプは**スクリーンスペース 1px 幅 / 4px 周期**。`SV_POSITION.xy` (ピクセル中心) を `int` に切ってから `(x + y) & 3` を取り、0/1/2 = メジャー、3 = マイナー (縞) としています。`(x + y)` を `float` のまま `floor` すると合計時に精度を失って隣接ピクセルで stripe index が揺れるので、必ず先に `int2` キャストしてから足してください。

> **黒側だけ縞を白にしている理由**: 仕上げにハーフランバートを乗算するため、白縞 (= 1.0) は `halfL` の階調そのままで見え、暗部でも縞が陰影パターンとして読めるようになります。グレー (= 0.5) のままだと暗部で `0.5 × 0.3 = 0.15` まで沈んで見えづらくなります。

#### ハーフランバート陰影

```hlsl
float3 hatchN = normalize(i.worldNor);
float3 hatchL = normalize(sunDirection.xyz);
float halfL   = saturate(dot(hatchN, hatchL) * 0.5 + 0.5);
halfL         = lerp(0.3, 1.0, halfL);
c            *= halfL;
```

GeoGen のリファレンス画像のような陰影付きハッチを再現するため、ハッチ色 `c` にハーフランバートをクランプ付きで乗算します。

| 太陽との角度 | `n·L` | `halfL` | 乗算後 |
| --- | --- | --- | --- |
| 太陽方向 | 1 | 1.000 | **1.000** |
| 真横 | 0 | 0.500 | **0.650** |
| 反対側 | -1 | 0.000 | **0.300** |

床 0.3 にクランプすることで反対側でも完全な黒に潰れず、ハッチが陰影パターンとして読める明度を保ちます。素の `(n·L * 0.5 + 0.5)` だと反対側が 0 になりハッチ自体が消えてしまうため、必ず `lerp(floor, 1.0, halfL)` 形式で使ってください。

> モード 0 / モード 1 は陰影乗算しません。モード 2 限定の処理です。

### 中間域の扱い

モード 2 の中間域 (`0.01 < mask < 0.99`) はハッチパターンが乗らない単純な `c = mask` のグレースケールランプですが、ハーフランバート乗算は同じく適用されます。これにより**飽和していないマスクでも 3D 形状の陰影が見える**ようになっています。

## 壁面 (断面) の sentinel 経路

`BuildMeshFromHeightfield` が生成する側壁 (terrain の縁の垂直面) は、上端頂点の `mask` をそのまま継承すると上端 1 セルのマスク値が縦方向に引き伸ばされて見えてしまいます。これを防ぐため、壁頂点には sentinel 値 `kWallMaskSentinel = 2.0f` を入れ、PS 側で検出してモードごとに塗り分けます。

```hlsl
if (i.mask > 1.5) {
    if (maskShadingMode > 0.5 && maskShadingMode < 1.5) {
        return float4(0.18, 0.20, 0.21, 1.0);   // モード 1: lowMask
    }
    return float4(0.25, 0.25, 0.25, 1.0);       // モード 0 / モード 2: 25% グレー
}
```

| モード | 壁面の色 |
| --- | --- |
| 0 (グレースケール) | RGB(0.25, 0.25, 0.25) |
| 1 (グレー×オレンジ) | RGB(0.18, 0.20, 0.21) — このモードの `lowMask` をライティング非依存の一色で塗布 |
| 2 (グレースケール + 斜線) | RGB(0.25, 0.25, 0.25) — ハッチ・ハーフランバートとも適用しない |

> **sentinel に `2.0f` を選んだ理由**: `[0, 1]` 正規化マスクの想定範囲外で、かつ `PSEdge` の負センチネル (`< -1.5` = リンクプレビュー青、`< -0.5` = 選択ハイライト赤) とも衝突しないため、壁エッジは通常通り `albedoColor` で描画されます。
>
> **GPU ディスプレースメントパスは壁を生成しない** のでこの分岐は実質 CPU メッシュパス専用です。GPU パスではメッシュトポロジー上、地表面のみが描画されます。

## 表示フィールド

`HeightfieldPreviewField` ([src/node_graph.h](../../src/node_graph.h)) で `mask` チャネルにコピーされる元データを切り替えます:

| 値 | フィールド | 取得元 |
| --- | --- | --- |
| `Heightmap` | `heights` (高さ自体) | `HeightfieldGrid::heights` |
| `Deposits` | 堆積マップ | `HeightfieldGrid::deposits` (Multi-Scale Erosion / Sediment) |
| `Flows`    | 流線マップ | `HeightfieldGrid::flows` (Multi-Scale Erosion) |
| `Age`      | 年齢マップ | `HeightfieldGrid::age` |
| `Mask`     | 汎用マスク (Mask Fluvial 等) | `HeightfieldGrid::mask` |

ノードの出力ピンクリックで `SelectHeightfieldPreviewField` が呼ばれ、選択フィールドの値が `mask` に正規化コピーされます。**マスクプレビュー / マスクシェーディングは "どのフィールドを選んだか" は知らず**、最終的に `grid.mask` に書かれた値を一律 `[0, 1]` のスカラーとして扱います。

## 参考実装箇所

| 場所 | 役割 |
| --- | --- |
| [src/node_graph.cpp `BuildMeshFromHeightfield`](../../src/node_graph.cpp) | 地表 + 側壁 + 底面の頂点・三角・エッジを生成。壁頂点 `mask` に sentinel を書き込む。 |
| [shaders/mesh_preview.hlsl `PSSurface`](../../shaders/mesh_preview.hlsl) | マスクプレビュー本体。3 モード分岐とハッチ計算 + ハーフランバート。 |
| [shaders/mesh_preview.hlsl `VSDisplacement`](../../shaders/mesh_preview.hlsl) | GPU パスのマスクサンプリング (バイリニア、`displacementMask` SRV)。 |
| [src/node_graph.h `PreviewSettings::maskShadingMode`](../../src/node_graph.h) | UI とプロジェクトファイルに永続化される列挙。 |

## 関連ドキュメント

- [docs/sky_and_clouds/sky_and_clouds.md](../sky_and_clouds/sky_and_clouds.md) — 大気・雲ライティング全般 (マスクプレビュー時はスキップされる側)
- [docs/nodes/mask/](../nodes/mask/) — マスクを生成するノード群 (`Mask Noise` / `Mask Blend` / `Mask Fluvial`)
- [docs/nodes/heightfield/multi_scale_erosion/multi_scale_erosion_algorithm_guide.md](../nodes/heightfield/multi_scale_erosion/multi_scale_erosion_algorithm_guide.md) — マスクに書き込まれる流線・堆積データの生成側
