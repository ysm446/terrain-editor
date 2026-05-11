# Mask Noise アルゴリズム入門

このメモは、`Mask Noise` ノードが内部で何をしているかを段階的に理解するための読み物です。
パラメータの詳細一覧は `mask_noise_node.md` にまとめています。

## まず何をするノードか

`Mask Noise` は、**Perlin ノイズを fBM (Fractional Brownian Motion) で重ね合わせた [0, 1] のマスクグリッドを生成**するノードです。

ハイトフィールドには依存しません。独立した確率的テクスチャとして、
`Mask Blend` や `Mask Fluvial` と組み合わせてマスク合成のベースとして使います。

## 全体の流れ

1. 解像度に応じて各セルの UV 座標 `(u, v) ∈ [0, 1]` を計算する。
2. UV に `Frequency` をかけて空間スケールを決める。
3. `Octaves` 回の Perlin ノイズを振幅・周波数を変えながら積み重ねる (fBM)。
4. 出力 ≈ [-1, 1] を `× 0.5 + 0.5` で [0, 1] に変換する。

## 1. Perlin ノイズの仕組み

Perlin ノイズは格子点ごとにランダムな**グラジェントベクトル**を割り当て、
隣接する 4 点のグラジェントとの内積を補間して滑らかな値を生成します。

```text
各格子点 (x0, y0), (x1, y0), (x0, y1), (x1, y1) に対して:
    gradient = cos(angle), sin(angle)    ← hash(x, y, seed) から角度を決める
    v = dot(gradient, offset_to_cell)   ← 評価点からのオフセットとの内積

補間:
    u = fade(dx), v = fade(dy)
    result = lerp(lerp(v00, v10, u), lerp(v01, v11, u), v)
```

`fade` 関数は 5 次エルミート曲線です。

```text
fade(t) = t³ × (t × (6t - 15) + 10)
```

2 次の smoothstep (`3t² - 2t³`) より滑らかで、2 階微分がゼロになります。
これにより格子点での繋ぎ目が視覚的に完全に消えます。

## 2. fBM (Fractional Brownian Motion)

fBM は複数のオクターブのノイズを重ねて自然なテクスチャを作る手法です。

```text
total = 0
amplitude = 1
maxAmplitude = 0
freq = 1

for i in range(octaves):
    total += Perlin2D(x × freq, y × freq, seed + i × 1013) × amplitude
    maxAmplitude += amplitude
    amplitude *= persistence
    freq     *= lacunarity

result = total / maxAmplitude    ← 振幅の合計で正規化
```

各オクターブのシードには `seed + i × 1013` を使います。
1013 は素数なので、シードのオフセットが異なるオクターブ間でパターンが被りません。

| パラメータ | 役割 |
| --- | --- |
| `Octaves` | 重ねる周波数の数。多いほど細かいディテールが増す |
| `Lacunarity` | 次のオクターブの周波数倍率。2.0 = 1/2 サイズのパターンを重ねる |
| `Persistence` | 次のオクターブの振幅倍率。0.5 = 高周波ほど弱くなる |
| `Frequency` | 基本周波数。大きいほど細かいパターン |

### オクターブと Persistence の関係

`Persistence = 0.5` のとき各オクターブの振幅は `1, 0.5, 0.25, 0.125, ...` と半減します。
低周波 (大きなうねり) が支配的で、高周波 (細かい凹凸) がアクセントになります。

`Persistence = 0.8` にすると高周波が相対的に強くなり、粒度の細かいテクスチャになります。
`Persistence = 0.3` にすると大きなうねりだけが残り、細部が弱まります。

## 3. [0, 1] へのリマップ

Perlin の出力は理論上 `[-1, 1]` ですが、fBM の積み重ねで実際の範囲は縮まります。

```text
mask[i] = clamp(fbm × 0.5 + 0.5, 0, 1)
```

単純な線形リマップです。`fbm = 0` が中間グレー (0.5) になります。

## パラメータを直感で見る

| パラメータ | 直感的な役割 |
| --- | --- |
| `Seed` | 乱数の初期値。変えると全く別のパターンになる |
| `Octaves` | 細かさの層の数。1 = 大きなうねりのみ、12 = 最大ディテール |
| `Frequency` | パターンの細かさ。2倍にすると2倍細かくなる |
| `Lacunarity` | オクターブごとの周波数の増加率。通常 2.0 |
| `Persistence` | オクターブごとの振幅の減衰率。通常 0.5 |
| `Simulation Resolution` | 生成するマスクグリッドの解像度 |

## よくある使い方

### 地形の質感マスクとして

`Mask Blend` に接続して、岩場エリアと植生エリアを分けるベースマスクとして使います。
`Frequency` を 2〜4 にすると、地形スケールに合った大きめのパターンが出ます。

### 小さなディテールのみ

`Octaves = 1`, `Frequency = 8〜16` で高周波ノイズだけを生成します。
`Mask Fluvial` の流路にこのノイズを `Mask Blend (Multiply)` で乗算すると、
流路のエッジがざらついて自然に見えます。

## 見た目が崩れるときの考え方

### パターンが均一すぎて変化がない

`Octaves` を増やし、`Persistence` を 0.6〜0.7 に上げます。
高周波成分が加わり、有機的なディテールが生まれます。

### パターンが細かすぎてノイジー

`Frequency` を下げるか、`Octaves` を減らします。
`Octaves = 3〜4`, `Frequency = 2〜4` が汎用的なバランスです。

### 特定の空間スケールのパターンが欲しい

`Frequency × Lacunarity^(n-1)` が n 番目のオクターブの周波数です。
目標スケールにあわせて `Frequency` を逆算します。
例: 地形サイズ 2048 m でパターンサイズ 256 m にしたいなら `Frequency = 2048/256 = 8`。
