# Snow アルゴリズムメモ

`Snow` ノードは、雪を単なる slope mask として作るのではなく、雪厚を持つ素材として地形上に注入し、斜面に沿って再配分する簡易モデルです。

## 基本方針

- `Emission Amount (m)` は地形へ注入される雪の総量です。
- `Iterations Count` は simulation step 数です。GeoGen の観察に合わせ、新規ノードでは `40` を既定値にしています。
- `Emission Time (%)` は、総量をいつ注入するかを決めます。`0%` は最初に全量、`100%` は全 step に均等配分です。
- `Snow Motion Slope Limit (deg)` は、雪が流れずに止まれる最大傾斜です。
- `Snow Surface Smoothing (%)` は、再配分後に積もった雪面をならす強さです。半径は `Largest Detail Level (m)` を流用します。
- mask は雪厚をそのまま正規化せず、`Mask Threshold (m)` と `Mask Feather (m)` で coverage に変換します。

## 処理手順

### 1. 注入量

```text
emissionIterations =
    emissionTime <= 0 ? 1 : ceil(iterationCount * emissionTime)

stepEmission = emissionAmount / emissionIterations
```

`iter < emissionIterations` の間だけ、全セルへ `stepEmission` を加えます。

### 2. 雪面の安定判定

各セルの雪面は次の高さです。

```text
surface = baseHeight + snowThickness
```

8 近傍のうち、現在セルより低く、かつ `Snow Motion Slope Limit` を超える最も急な方向を探します。

```text
slope = (surface[current] - surface[neighbor]) / distance
flow if slope > tan(motionSlopeLimitDeg)
```

### 3. 雪の移動

不安定なセルでは、安定角を超えた分の一部だけを低い近傍へ移します。

```text
stableDrop = tan(limit) * distance
excess = max(0, surface[current] - surface[neighbor] - stableDrop)
slopeFactor = clamp((slope - tan(limit)) / slope, 0, 1)
amount = min(snow[current], excess * 0.5, snow[current] * transportRate * slopeFactor)
```

`Settling Passes` 回だけこの移動を繰り返します。移動先探索の stride は `Largest Detail Level (m)` から始まり、pass ごとに小さくなるため、大きな谷へ寄せたあと細部で落ち着く挙動になります。

移動先探索は行単位で並列化し、各セルの移動先と移動量を一度バッファに書き出してからまとめて適用します。これにより scatter 書き込みの競合を避けつつ、高解像度でも Snow ノードで長く止まって見えにくくしています。

### 4. 雪面平滑化

`Snow Surface Smoothing (%)` が 0 より大きい場合、最終的な雪厚に separable blur をかけます。平滑化半径は追加パラメータを増やさず、`Largest Detail Level (m)` から決めます。

露出地面へ雪が広がりすぎないよう、blur の重みには coverage を使います。

```text
coverage = smoothstep(maskThreshold - maskFeather,
                      maskThreshold + maskFeather,
                      snowThickness)

smoothed = weightedBlur(snowThickness, coverage)
snowThickness = lerp(snowThickness, smoothed,
                     surfaceSmoothing * coverage)
```

### 5. 出力

```text
heightOut = baseHeight + snowThickness
```

mask は coverage として出します。

```text
if maskFeather == 0:
    mask = snowThickness >= maskThreshold ? 1 : 0
else:
    mask = smoothstep(maskThreshold - maskFeather,
                      maskThreshold + maskFeather,
                      snowThickness)
```

このため、薄い雪が広い中間グレーとして出るのではなく、積雪域は白、露出地面は黒、境界だけがグレーになります。

## 旧実装との差

旧実装は `Slope Limit Min/Max` で各セルの初期雪量を直接決め、envelope smoothing で谷を埋めていました。新実装では、雪量はまず注入され、その後 `Snow Motion Slope Limit` に従って移動します。

そのため、以下の挙動が期待できます。

- `Emission Amount = 0` では雪がまったくない。
- `0` から小さな値へ上げたとき、いきなり広範囲がグレーになるのではなく、しきい値を超えた場所から積雪域として出る。
- `Iterations Count` を増やすと、点在していた雪が谷や棚へまとまりやすくなる。
- `Emission Time` を変えると、最初に置いた雪を長く流すか、最後まで少しずつ追加するかが変わる。

## 今後の候補

- 風向き/風速を追加して、斜面上部や風下への偏りを作る。
- GPU と CPU の見た目差を実地確認し、必要なら gather 方式の近傍判定をさらに CPU 版へ寄せる。
- `Slope Limit Min/Max` と `Mask Max Snow` の保存互換フィールドを、将来のプロジェクト形式更新時に整理する。
