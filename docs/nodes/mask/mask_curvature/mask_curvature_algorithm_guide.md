# Mask Curvature アルゴリズムメモ

`Mask Curvature` は、厳密な微分幾何の曲率ではなく、地形制作で扱いやすい局所平均との差分を使います。

1. 入力 height を `Radius` の box blur で平滑化する。
2. `delta = height - blurredHeight` を計算する。
3. `Mode` に応じて `delta` を解釈する。
   - `Ridges`: `delta`
   - `Valleys`: `-delta`
   - `Absolute`: `abs(delta)`
4. `Sensitivity (m)` で正規化し、`Threshold` と `Gamma` を適用する。
5. `[0, 1]` に clamp して `Mask` として出力する。

この方式は実装が軽く、`Radius` を変えることで微細な凹凸から大きな谷地形まで同じノードで扱えます。
