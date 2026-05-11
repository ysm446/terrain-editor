# Sediment アルゴリズム入門

このメモは、`Sediment` ノードが内部で何をしているかを段階的に理解するための読み物です。
パラメータの詳細一覧は `sediment_node.md` にまとめています。

## まず何をするノードか

`Sediment` は、GeoGen 互換の**土砂スライド (タラス崩壊) シミュレーション**を行うノードです。
水流で削るのではなく、「急すぎる傾斜は重力で崩れる」という現象だけを扱います。

浸食とは逆向きの用途にも使えます。
高所から崩れた土砂が低地に積もり、谷や盆地を埋めていく様子を再現します。

`Heightmap Blur` が形状を均質に崩すのとは異なり、`Sediment` は物理的なタラス角 (安息角) に従います。
それより急な斜面は崩れ、安定した斜面は変化しません。

## 全体の流れ

1. 入力地形を **岩盤 (bedrock)** と **土砂 (sediment)** の 2 層に分割する。
2. `Iterations` 回だけ次を繰り返す:
   - 指定期間 (Emission Time) 内は、全セルに均一に土砂を降らせる。
   - `MacroPasses × StabIterations` 回の単位スライドを実行する。
3. 最終的に `height = bedrock + sediment` を出力し、
   マスクに土砂の分布 (95 パーセンタイル正規化) を書き込む。

## 1. 岩盤と土砂の分割

`Convert Terrain to Sediment` が有効 (デフォルト) のとき:

```text
bedrock[i] = 0
sediment[i] = grid.heights[i]  ← 入力地形がすべて土砂になる
```

無効のとき:

```text
bedrock[i] = grid.heights[i]   ← 入力地形が固定岩盤になる
sediment[i] = 0                ← 土砂はゼロから積み上げる
```

前者は「現在の地形が崩れていく」シナリオ、後者は「岩盤の上に土砂を降らせる」シナリオです。

## 2. タラス角の計算

`Sediment Viscosity` から安息角を求めます。粘性が高い (= 流動しにくい) ほど角度が急になります。

```text
talusAngleDeg = viscosity² × 80°
talusH        = tan(talusAngleDeg × π/180) × cellSizeM
```

粘性と角度の対応の例 (おおよその目安):

| Viscosity | talusAngleDeg | 相当する材料 |
| --- | --- | --- |
| 0.0 | 0° | 完全な液体 (崩壊なし) |
| 0.2 | 3.2° | 泥流・湿った砂 |
| 0.5 | 20° | 乾いた砂 |
| 0.8 | 51° | 砂礫・玉石 |
| 1.0 | 80° | 岩石 |

## 3. 単位スライド (ThermalSlideUnitStride)

タラス崩壊の 1 ステップは**2 スイープ構造**で行われます。

### スイープ 1: 流出量の計算

各セルから 4 近傍 (上下左右) へ土砂がどれだけ流れるかを計算します。

```text
各方向 k について:
    diff = (bedrock[i] + sediment[i]) - (bedrock[j] + sediment[j])
    drop[k] = diff - talusH  if diff > talusH else 0
```

アクティブな方向 (drop > 0 の方向) の数を `n_active` とすると、
各方向への送出量は **(n_active + 1) 除算**で決めます。

```text
idealOut    = totalDrop / (n_active + 1)
actualOut   = min(sediment[i], idealOut)   ← 手持ちを超えては送れない
outgoing[k] = (drop[k] / totalDrop) × actualOut
```

`(n+1)` で割る理由: スライド後に各隣接セルとの高さ差がちょうど `talusH` になるように調整するための式です。
これにより 1 ステップで収束し、振動やオーバーシュートが起きません。

### スイープ 2: 土砂の更新

流出と流入を同時に適用します。

```text
sediment[i] = max(0, sediment[i] - totalOut + incoming)
```

`max(0, ...)` で土砂がマイナスにならないよう保護します。
この 2 スイープ分離のおかげで並列実行しても競合が発生しません。

## 4. マクロパスと移動距離

1 回の `ThermalSlideUnitStride` で土砂が移動できる最大距離は 1 セルです。
大きな範囲を崩したい場合、`MacroPasses` 回繰り返す必要があります。

```text
macroPasses = ceil(largestDetailLevelM / cellSizeM)
```

`Largest Detail Level = 8 m` でセルサイズ `4 m` なら `macroPasses = 2` になります。
これを `Stabilization Iterations` (安定化の繰り返し数) 倍だけ毎反復実行します。

## 5. 土砂の降下 (Emission)

各反復の最初に、全セルへ均一に土砂を追加します。

```text
emissionEnd    = ceil(iterations × emissionTime)
emissionPerIter = emissionAmountM / emissionEnd

if iter < emissionEnd:
    sediment[i] += emissionPerIter  (全セルで)
```

`Emission Time = 0.0` にすると土砂の降下はなくなり、初期状態の土砂のみが崩落します。
`Emission Time = 1.0` にすると全反復にわたって均等に土砂が降ります。

## 6. マスクの正規化

出力マスクは土砂の厚さを第 95 パーセンタイルで正規化します。
最も堆積している 5% のセルが飽和 (白) になります。

```text
norm = 95th_percentile(sediment)
mask[i] = ApplyMaskContrast(sediment[i] / norm, contrast)
```

`Mask Contrast` を上げると薄い堆積も白く浮かび上がります。

## パラメータを直感で見る

| パラメータ | 直感的な役割 |
| --- | --- |
| `Iterations` | シミュレーションの総ステップ数 |
| `Stabilization Iterations` | 1 反復あたり何回タラス安定化を行うか |
| `Largest Detail Level (m)` | 土砂が 1 反復で移動できる最大距離 |
| `Emission Amount (m)` | 全反復を通じて降下する土砂の総量 |
| `Emission Time (%)` | 0 = 最初だけ、1 = 全反復にわたって降下 |
| `Sediment Viscosity (%)` | 安息角。高いほど急な傾斜でも崩れない |
| `Convert Terrain to Sediment` | OFF = 入力地形を固定岩盤として扱う |
| `Mask Contrast` | マスクのガンマ補正 |

## 見た目が崩れるときの考え方

### 地形が崩れすぎて平坦になる

`Emission Amount` と `Iterations` が多すぎます。
まず `Iterations = 20〜40`、`Emission Amount = 0.2〜0.5` から試してください。

### 土砂が局所に固まって自然に広がらない

`Largest Detail Level` を大きくし、`Stabilization Iterations` を増やします。
`Largest Detail Level = 32 m` 程度にすると、より広い範囲へ土砂が広がります。

### 崩れ方が急すぎる (液状に流れる)

`Sediment Viscosity` を上げてタラス角を大きくします。
岩石なら 0.8、砂礫なら 0.5 前後が目安です。

### Convert Terrain to Sediment = OFF にすると何も動かない

`Emission Amount` を設定してください。
無効のとき土砂はゼロから始まるため、降下がなければ何も崩れません。

## チューニングの優先順位

1. `Convert Terrain to Sediment` と `Emission Amount` で素材量を決める。
2. `Sediment Viscosity` で崩れやすさ (タラス角) を設定する。
3. `Largest Detail Level` で土砂の移動距離を調整する。
4. `Iterations` を増やして成熟度を上げる。
5. `Mask Contrast` でマスクの視認性を整える。
