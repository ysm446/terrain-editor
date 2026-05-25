# Shape アルゴリズム入門

このメモは、`Shape` ノードが内部で何をしているかを理解するための読み物です。
パラメータの詳細一覧は `shape_node.md` にまとめています。

## まず何をするノードか

`Shape` は、画像ファイルなしに**解析的な数式だけで地形を生成**するノードです。
`Heightmap Load` の代替として使い、浸食ノードのテスト・デバッグ・チュートリアルに向いています。

3 種類のプリミティブ形状を選べます。

- **Hemisphere (半球)**: 中央が高く、放射状になだらかに落ちる丸いドーム。
- **Pyramid (ピラミッド)**: 中央から等間隔の稜線が 4 本伸びる四角錐。
- **Box (箱)**: 指定した Scale の範囲を一定高さで埋める平らな直方体。

## 高さの計算式

グリッドの各セル `(x, z)` を `[-1, 1]` の正規化座標 `(nx, nz)` に変換してから高さを計算します。

```text
nx = x / (resolution - 1) × 2 - 1
nz = z / (resolution - 1) × 2 - 1
```

### Hemisphere

```text
r² = nx² + nz²
normalizedHeight = r² < 1 ? sqrt(1 - r²) : 0
```

半径 1 を超えた部分は 0 になります (グリッドの角が 0 に落ちる)。
これは単位球 `x² + y² + z² = 1` の上半分を真上から見た等高線と同じです。

### Pyramid

```text
normalizedHeight = max(0, 1 - max(|nx|, |nz|))
```

`max(|nx|, |nz|)` はチェビシェフ距離です。グリッドの各辺から内側へ向かって等間隔に増加し、
中央ピクセルだけが最大値 1 になります。斜め方向の稜線はちょうど 45° になります。

### Box

```text
normalizedHeight = 1
```

Scale の範囲内を一定高さで埋め、範囲外は 0 にします。平らな台地、段差、境界処理、影や侵食のテストに使いやすい形です。

### スケーリング

```text
height = normalizedHeight × scale × (relativeHeight / 100)
```

| 変数 | 意味 |
| --- | --- |
| `scale` | 地形の水平幅 (m) |
| `relativeHeight` | 水平スケールに対する最大高さの割合 (%) |

`scale = 1024, relativeHeight = 50` ならば頂点の高さは `1024 × 0.5 = 512 m` です。

## どれを選ぶか

| 観点 | Hemisphere | Pyramid | Box |
| --- | --- | --- | --- |
| 稜線の有無 | 稜線なし (全方向対称) | 4 本の明確な稜線 | 上面は平坦、外周に垂直段差 |
| 浸食の見え方 | 流路が全方向に均等に出る | 稜線で流路が分岐しやすい | 端部から崩れやすく、台地のテスト向き |
| D8 アーティファクトの目立ちやすさ | 均質すぎて対称パターンが出やすい | 稜線の非対称が D8 をほぼ隠す | 平坦部では出にくく、境界で確認しやすい |
| Thermal パスの検証 | しやすい (崩れが均質) | 稜線 vs 谷の違いが見えやすい | 段差崩壊や境界処理が見えやすい |

一般的には **Hemisphere** の方が D8 の対称アーティファクトが目立ちます。
`Multi-Scale Erosion` の動作確認には `Pyramid` の方が見やすいことが多いです。

## パラメータを直感で見る

| パラメータ | 直感的な役割 |
| --- | --- |
| `Shape Type` | Hemisphere、Pyramid、Box の選択 |
| `Scale (m)` | 地形の水平幅。Heightmap Load の Scale と合わせる |
| `Relative Height (%)` | 頂点の高さを水平幅の何%にするか |
| `Simulation Resolution` | 生成する格子の解像度 |

## テスト時のおすすめ設定

浸食ノードの挙動をゼロから確認したいとき:

```
Shape Type: Hemisphere
Scale: 2048 m
Relative Height: 30%
Simulation Resolution: 512
```

`Multi-Scale Erosion` を接続し、`Slope Exponent` と `Thermal Strength` を調整すると、
各パスが地形に与える影響を確認しやすくなります。
