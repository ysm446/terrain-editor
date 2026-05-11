# Mask Fluvial アルゴリズム入門

このメモは、`Mask Fluvial` ノードが内部で何をしているかを段階的に理解するための読み物です。
パラメータの詳細一覧は `mask_fluvial_node.md` にまとめています。

## まず何をするノードか

`Mask Fluvial` は、**ハイトフィールドに D8 または MFD 流量累積アルゴリズムを適用して水路ネットワークをマスクとして出力**するノードです。

「どこに川が流れるか」を地形の形状だけから計算します。
高さは変化しません (ハイトフィールドのパススルー)。

出力は `[0, 1]` のマスクグリッドです。
大きな川 (集水域が広いセル) ほど明るくなり、分水嶺や斜面は暗くなります。

## 全体の流れ

1. **Pit Fill**: ハイトフィールドの局所最小値 (pit) を埋めて、流路がすべて端へ抜けるようにする。
2. **高さ降順ソート**: 上流→下流の順にすべてのセルを並べる。
3. **流量累積 (D8 または MFD)**: 上流から順に流量を下流へ伝播させる。
4. **出力曲線変換**: 累積値を Log / Threshold / Linear の曲線で [0, 1] に変換する。

ステップ 3 の累積ループは「上流先に処理する」という依存性があるため逐次実行です。
それ以外 (Pit Fill, Sort, 出力変換) は並列化されています。

## 1. Pit Fill — 局所最小値の除去

標高の「くぼみ」 (pit) があると水が行き場をなくして流路が途切れます。
Jacobi 反復 (二重バッファ) で pit をわずかに持ち上げます。

```text
各反復:
    各内部セルに対して:
        8 近傍の最小高さ = minNeighbor
        if 全近傍 ≥ 自分 (= pit): filled[i] = minNeighbor + epsilon
```

`epsilon = 1e-4 m` の微小な傾斜を付けて、水が「ほぼ平坦な経路」を流れられるようにします。
反復数は `Pit Fill Iterations` で制御します (デフォルト 8 回)。
0 にすると pit fill なしで計算し、閉じた盆地に流量が溜まります。

境界セルは埋めません。グリッドの端はつねに出口として機能します。

## 2. 高さ降順ソート

pit fill 後の高さで全セルを降順ソートします。

```text
indices = sorted by filled[i] descending
```

`std::execution::par` で並列ソートします。
これにより、累積ループが「必ず上流セルを先に処理する」順序を保証します。

## 3. 流量累積

初期流量はすべてのセルで 1 (= 1 セル分の雨) です。

### D8 アルゴリズム (単一方向)

各セルから**最も急な下り方向 1 つだけ**にすべての流量を送ります。

```text
for each cell i in descending height order:
    slope[k] = (h[i] - h[neighbor_k]) / kDist[k]
    bestK = argmax(slope[k] × align[k])   ← align は Inertia 補正
    accum[bestK] += accum[i]
```

8 方向を探索するため、対角方向の距離は `√2` で割り引きます (`kDist[k] ∈ {1.0, √2}`)。

D8 は実装がシンプルで高速ですが、**流路が直線的になりやすい**という弱点があります。
実地形では分岐や蛇行があるため、`MFD` の方が自然に見えることが多いです。

### MFD アルゴリズム (多方向)

各セルから、下り勾配があるすべての方向に**勾配の大きさに比例した割合**で流量を分配します。

```text
weight[k] = (slope[k] × align[k]) ^ mfdExponent   if slope > 0 else 0
accum[j] += accum[i] × weight[k] / sum(weight)
```

`MFD Exponent` が大きいほど最急方向への集中が強くなり、D8 に近づきます。
`MFD Exponent = 1` に近いほど均等分散になり、流路が拡散します。
典型値は 4.0 です。

MFD は流路がより自然に広がりますが、非常に浅い地形では過拡散になることがあります。

## 4. Inertia — 流れの慣性

D8 / MFD どちらも、流れが急方向へ極端に曲がるのを抑える補正係数を適用できます。

Sobel 3×3 カーネルで局所の下り方向 `(downX, downZ)` を計算し、
各方向との整合性 (内積) を `Inertia` で重み付けします。

```text
(downX, downZ) = -Sobel_gradient / |Sobel_gradient|   ← 下り方向の単位ベクトル

align[k] = (1 - inertia) + inertia × max(0, dot(direction[k], downhill))
```

`Inertia = 0` のとき `align[k] = 1` でどの方向も等しく評価します (通常の D8/MFD)。
`Inertia = 1` のとき Sobel 下り方向に揃った方向だけが強調され、流路が蛇行しにくくなります。

Inertia は川の蛇行の少ない山岳地形に向いています。平坦な谷底では過拘束になることがあります。

## 5. 出力曲線

累積値 `accum` (= 上流域セル数の近似値) は非常に大きな値になりえます。
3 種類の曲線で [0, 1] に変換します。

### Log モード (デフォルト)

```text
a = max(0, accum - threshold)
t = log(1 + a) / log(1 + maxAdjusted)
mask = clamp(t, 0, 1) ^ gamma
```

対数スケールで変換するため、大きな本流と細い支流の両方を視認できます。
樹枝状のネットワーク全体を見たいときに最適です。

`Gamma` を下げる (0.3〜0.4) と細い支流が明るくなります。
`Gamma` を上げる (0.8〜1.0) と本流だけが際立ちます。

### Threshold モード

```text
thresholdLow  = max(1, thresholdCells)
thresholdHigh = thresholdLow × (1 + 4 × softness)

t = clamp((accum - thresholdLow) / (thresholdHigh - thresholdLow), 0, 1)
smooth = t² × (3 - 2t)
mask = smooth ^ power
```

集水域が閾値以上のセルだけを白く抽出します。「川のある/ない」を二値化するのに使います。

`Threshold` で川の太さを制御します (大きいほど本流のみ)。
`Softness` で二値化の境界をぼかします。

### Linear モード

```text
a = max(0, accum - threshold)
t = a / maxAdjusted
mask = clamp(t, 0, 1) ^ gamma
```

線形スケールで変換します。ほとんどの小さな支流は 0 に近くなります。
数値的な精度が必要な場合以外は Log の方が視覚的に有用です。

## パラメータを直感で見る

| パラメータ | 直感的な役割 |
| --- | --- |
| `Algorithm` | D8 = 単一方向 (シャープな流路)、MFD = 多方向 (拡散した流路) |
| `Output Curve` | Log = 全体、Threshold = 川の有無、Linear = 線形 |
| `Threshold` | この集水域より小さい流路をカットする |
| `Gamma` | Log/Linear モードのガンマ補正。小さいほど細い流路が明るくなる |
| `Softness` | Threshold モードの境界のなめらかさ |
| `Power` | Threshold モードの出力の冪乗 |
| `Pit Fill Iterations` | 局所最小値除去の反復数。0 で無効 |
| `MFD Exponent` | MFD モードでの集中度。4.0 がデフォルト |
| `Inertia` | 流れ方向のなめらかさ。0 = なし、1 = Sobel 整合優先 |

## 見た目が崩れるときの考え方

### 流路がなく全面が均一なグレーになる

入力地形が平坦すぎて流れが分散しています。
`Pit Fill Iterations` を 0 にして pit fill を無効化し、`Gamma` を 0.3 に下げてみてください。

### 流路が直線状で不自然

`Algorithm` を D8 から `MFD` に変えます。
または `MFD Exponent` を 2〜3 に下げると拡散が強くなります。

### 細い支流が多すぎてうるさい

`Output Curve` を `Threshold` に変え、`Threshold` を上げます。
`Log` モードで `Gamma` を 0.8〜1.0 に上げても支流を暗くできます。

### pit が残って流路が途切れる

`Pit Fill Iterations` を 16〜32 に増やします。
それでも pit が残る場合は、入力地形自体に大きなフラット領域があります。
`Multi-Scale Erosion` を先に通すと pit が自然に解消されます。

## チューニングの優先順位

1. まず `Log` モードで全体の流路ネットワークを確認する。
2. `Algorithm` を D8/MFD で切り替えて流路の質を比較する。
3. `Gamma` で明るさの分布を調整する (0.3〜0.7 が使いやすい範囲)。
4. 川だけを抽出したい場合は `Threshold` モードに切り替え、`Threshold` で絞る。
5. `Inertia` は平坦地形では 0 にして、複雑な山岳地形では 0.2〜0.4 で試す。
