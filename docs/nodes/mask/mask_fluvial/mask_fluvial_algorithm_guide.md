# Mask Fluvial アルゴリズム入門

このメモは、`Mask Fluvial` ノードが内部で何をしているかを段階的に理解するための読み物です。
パラメータの詳細一覧は `mask_fluvial_node.md` にまとめています。

## まず何をするノードか

`Mask Fluvial` は、**ハイトフィールドに MFD 流量累積アルゴリズムを適用して水路ネットワークをマスクとして出力**するノードです。

「どこに川が流れるか」を地形の形状だけから計算します。
高さは変化しません (ハイトフィールドのパススルー)。

出力は `[0, 1]` のマスクグリッドです。
大きな川 (集水域が広いセル) ほど明るくなり、分水嶺や斜面は暗くなります。

## 全体の流れ

1. **Largest Detail Level**: 解析用ハイトだけをガウスブラーし、小さな凹凸を流路判定から外す。
2. **Pit Fill**: ハイトフィールドの局所最小値 (pit) を埋めて、流路がすべて端へ抜けるようにする。
3. **高さ降順ソート**: 上流→下流の順にすべてのセルを並べる。
4. **流量累積 (MFD)**: 上流から順に流量を下流へ伝播させる。
5. **出力曲線変換**: 累積値を Log / Threshold / Linear の曲線で [0, 1] に変換する。

ステップ 4 の累積ループは「上流先に処理する」という依存性があるため逐次実行です。
それ以外 (内部 Pit Fill, Sort, 出力変換) は並列化されています。

## 1. Largest Detail Level — 流路判定スケール

入力ハイトフィールドは変更せず、流向計算に使う解析用ハイトだけを分離ガウスブラーします。

```text
radius = Largest Detail Level (m) / cellSize
analysis = gaussian_blur(input_height, radius)
```

4m では細かい支流や小さな窪みを拾いやすく、64m では小さな凹凸を無視して大きな谷筋を優先します。
これはマスクの後処理ではなく、pit fill、ソート、MFD の slope 判定に使う高さそのもののスケールを変えるため、川筋の位置や分岐が自然に変わります。

## 2. Pit Fill — 局所最小値の除去

標高の「くぼみ」 (pit) があると水が行き場をなくして流路が途切れます。
Jacobi 反復 (二重バッファ) で pit をわずかに持ち上げます。

```text
各反復:
    各内部セルに対して:
        8 近傍の最小高さ = minNeighbor
        if 全近傍 ≥ 自分 (= pit): filled[i] = minNeighbor + epsilon
```

`epsilon = 1e-4 m` の微小な傾斜を付けて、水が「ほぼ平坦な経路」を流れられるようにします。
反復数は内部固定値です。現在の UI では、見た目に効きやすい `Largest Detail Level` 側で小さな窪みの扱いを調整します。

境界セルは埋めません。グリッドの端はつねに出口として機能します。

## 3. 高さ降順ソート

pit fill 後の高さで全セルを降順ソートします。

```text
indices = sorted by filled[i] descending
```

`std::execution::par` で並列ソートします。
これにより、累積ループが「必ず上流セルを先に処理する」順序を保証します。

## 4. 流量累積

初期流量はすべてのセルで 1 (= 1 セル分の雨) です。

各セルから、下り勾配があるすべての方向に**勾配の大きさに比例した割合**で流量を分配します。

```text
weight[k] = slope[k] ^ flowConcentration   if slope > 0 else 0
accum[j] += accum[i] × weight[k] / sum(weight)
```

`Flow Concentration` が大きいほど最急方向への集中が強くなります。
`Flow Concentration = 1` に近いほど均等分散になり、流路が拡散します。
典型値は 4.0 です。

MFD は流路がより自然に広がりますが、非常に浅い地形では過拡散になることがあります。

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
| `Output Curve` | Log = 全体、Threshold = 川の有無、Linear = 線形 |
| `Threshold` | この集水域より小さい流路をカットする |
| `Gamma` | Log/Linear モードのガンマ補正。小さいほど細い流路が明るくなる |
| `Softness` | Threshold モードの境界のなめらかさ |
| `Power` | Threshold モードの出力の冪乗 |
| `Largest Detail Level` | 流路判定に使う地形スケール。小さいほど細かい支流、大きいほど大きな谷筋を優先 |
| `Flow Concentration` | 流れの集中度。大きいほど主流へ集まり、小さいほど面的に広がる |

## 見た目が崩れるときの考え方

### 流路がなく全面が均一なグレーになる

入力地形が平坦すぎて流れが分散しています。
`Flow Concentration` を上げるか、`Gamma` を 0.3 に下げてみてください。

### 流路が直線状で不自然

`Largest Detail Level` を上げて細かい凹凸を流路判定から外します。
または `Flow Concentration` を 2〜3 に下げると拡散が強くなります。

### 細い支流が多すぎてうるさい

`Output Curve` を `Threshold` に変え、`Threshold` を上げます。
`Log` モードで `Gamma` を 0.8〜1.0 に上げても支流を暗くできます。

## チューニングの優先順位

1. まず `Log` モードで全体の流路ネットワークを確認する。
2. `Largest Detail Level` で拾う谷筋のスケールを決める。
3. `Flow Concentration` で流れの集中度を調整する。
4. `Gamma` で明るさの分布を調整する (0.3〜0.7 が使いやすい範囲)。
5. 川だけを抽出したい場合は `Threshold` モードに切り替え、`Threshold` で絞る。
