# Mask Blend アルゴリズム入門

このメモは、`Mask Blend` ノードが内部で何をしているかを段階的に理解するための読み物です。
パラメータの詳細一覧は `mask_blend_node.md` にまとめています。

## まず何をするノードか

`Mask Blend` は、**2 つのマスクグリッドを 4 種類のモードで合成するノード**です。
解像度が異なる 2 つのマスクも自動的にリサンプルして合わせます。

単体では地形を変形しません。`Mask Fluvial` や `Mask Noise` の出力を組み合わせて、
複雑なマスクを構築するためのブリッジとして機能します。

## 全体の流れ

1. 入力 Foreground と Background の解像度を比較し、大きい方に合わせてリサンプルする。
2. 各セルでブレンドモードに従って演算する。
3. `Intensity` で演算結果と Foreground をブレンドし、`[0, 1]` にクランプして出力する。

## 1. 解像度のリサンプル

入力 Foreground と Background の解像度が異なる場合、低解像度側をバイリニア補間でアップサンプルします。

```text
n = max(a.resolution, b.resolution)
```

アップサンプル時は UV 座標 `(u, v) = (x / (n-1), z / (n-1))` でバイリニアサンプルします。

```text
u_src = u × (src.resolution - 1)
v_src = v × (src.resolution - 1)

x0 = floor(u_src), x1 = x0 + 1
z0 = floor(v_src), z1 = z0 + 1

sample = lerp(lerp(src[x0,z0], src[x1,z0], tx),
              lerp(src[x0,z1], src[x1,z1], tx), tz)
```

これにより低解像度のマスクを滑らかに引き伸ばして高解像度マスクに重ねられます。

## 2. ブレンドモード

4 種類のブレンドモードを選べます。入力 Foreground, Background のセル値を `foreground`, `background` としたとき:

| モード | 演算 | 用途 |
| --- | --- | --- |
| `Add` | `foreground + background` | 2 つのマスクを足し合わせる。合計が 1 を超えた部分は飽和する |
| `Multiply` | `foreground × background` | どちらも白い部分だけを残す (論理 AND に近い) |
| `Min` | `min(foreground, background)` | 2 つのマスクの暗い方を残す |
| `Max` | `max(foreground, background)` | 2 つのマスクの明るい方を残す (論理 OR に近い) |

### 各モードの性質

**Add**: 小さなマスクの合計。明るい領域が広がりやすいです。
`Mask Fluvial` (流路マスク) と `Mask Noise` を足して、ノイジーな流路帯を作るのに使えます。

**Multiply**: 共通する明るい領域だけを抽出します。
フィルタとして機能し、Foreground のマスクを Background で絞り込むときに便利です。
たとえば「流路かつ高地」のような条件を作れます。

**Min**: どちらか暗い方を残します。Multiply に近いですが、0 に向かって収縮する速度が異なります。

**Max**: どちらか明るい方を残します。Add と似ていますが、1 を超えて飽和しません。
「どちらかに流路がある」エリアを表すのに使えます。

## 3. Intensity によるブレンドと最終クランプ

ブレンド結果は Foreground と `Intensity` で線形補間した後、`[0, 1]` にクランプします。

```text
blended = blend_mode(foreground, background)
result  = clamp(lerp(foreground, blended, intensity), 0, 1)
```

`Intensity = 0` のとき出力は Foreground そのままです。
`Intensity = 1` のとき出力はブレンド結果そのものです。

`Intensity` を中間値にすると、元の Foreground マスクとブレンド結果を混ぜられます。
たとえば `Multiply (Intensity = 0.5)` は「Foreground と Background の共通部分を 50% だけ縮小する」効果になります。

## パラメータを直感で見る

| パラメータ | 直感的な役割 |
| --- | --- |
| `Blend Mode` | Add / Multiply / Min / Max から演算を選ぶ |
| `Blend Intensity (%)` | ブレンド結果と入力 Foreground の混合率 |

## 典型的な使い方

### 流路と広域ノイズの合成

```
Foreground: Mask Fluvial (流路マスク)
Background: Mask Noise  (大きなうねりノイズ)
Mode: Max
Intensity: 70%
```

流路に沿った細い線と、大きなノイズの明るい領域を統合します。

### ノイズでマスクを絞り込む

```
Foreground: Mask Fluvial (流路マスク)
Background: Mask Noise  (細かいノイズ)
Mode: Multiply
Intensity: 100%
```

流路マスクの上にノイズを乗算して、流路内をまばらにします。

### 2 つのノイズを重ねてより複雑なパターンを作る

`Mask Noise → Mask Blend (Add) → Mask Blend (Multiply)` のようにチェーンして、
多段階の合成ができます。各 `Intensity` で各ステージの効き具合を微調整します。

## 見た目が崩れるときの考え方

### Add で全体が白く飽和する

2 つのマスクの明るい領域が重なりすぎています。
`Intensity` を 50〜70% に下げるか、`Multiply` モードに変えてください。

### Multiply で真っ黒になる

どちらかのマスクに暗い領域が多すぎます。
事前に各マスクの中間輝度を確認し、どちらかが全体的に暗い場合は `Add` か `Max` の方が向いています。
