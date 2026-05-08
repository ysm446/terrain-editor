# 変更履歴

## 未リリース

- ハイトフィールド→メッシュ生成 (`BuildMeshFromHeightfield`) をフル並列化し、パラメータ操作中のメッシュ再生成負荷を削減しました(Phase 1)。具体的な変更点:
  - 頂点/三角形/エッジを事前に `resize` で確保し、`ParallelForRows` でインデックス指定書き込みに(`push_back` を撤廃)。
  - **頂点法線をハイトフィールドの 4-tap 勾配ベースに変更**(三角形ループ + per-vertex 累積を撤廃)。データレースなしで並列化でき、見た目も滑らか寄りに変わります(面ベース法線特有のフラットシェーディング感が減ります)。
  - エッジを規則格子から構造的に直接生成し、`std::unordered_set<uint64_t>` を使った dedup を撤廃。
  - 壁/底は法線が定数なので、サンプリングなしで高速に書き込み。底はインデックス順を反転して下向き法線を確定。
  - 1024² メッシュで体感 5-10× 速くなる想定(8 コア 想定、Debug ビルド込みでも数十ms 範囲)。Phase 2 では GPU 頂点ディスプレイス版を追加して、メッシュ再生成自体をなくす予定。
- 入力ハイトフィールドに「土のかぶっていない岩肌」風の凹凸を載せる `Rock` ノードを追加しました。地形を `density` 間隔のジッタード Voronoi グリッドでタイリングし、各セルをドーム + サブセル凹凸 + セル境界の亀裂で岩塊化、結果のハイトフィールドと「岩らしさ」マスクを出力します。Heightfield 系ノードとして緑のアクセント色、入力 1 + 出力 2(`Heightmap` / `Mask`)。既定値は `density 8m / coverage 1.0 / rockFill 0.85 / rockHeight 1.5m / heightJitter 0.5 / bumpiness 0.6 / crackDepth 0.3m`。`coverage` を下げると元地形が見える隙間が広がり、`rockFill` を下げると隣接ドーム間にセル境界の溝が通ります。離散粒子的な岩塊散布(崩壊岩)は別ノード(Crumbling)に分ける想定。
- ボリューム雲の `lightSamples` / `lightStepMeters` / `phaseEccentricity` スライダーを動かしてもビューポートが再描画されない不具合を修正しました。`GpuMeshPreview` の dirty チェック対象に新フィールドを追加。`windSpeed = 0` 時は基本的に「マウス操作中だけ再描画」する省エネ挙動なので、新パラメータのキャッシュ値と現在値を比較する経路が抜けていると変更が反映されません。なお自己遮蔽ライトマーチ自体はビューサンプル毎に追加で 6 回程度の密度読みが入るため、再描画 1 フレームあたりのコストは従来比 ~7× 増です。負荷が気になる場合は `Light Samples = 0` で旧挙動(yNorm 上下ランプ)に戻せます。
- ボリューム雲に **太陽方向ライトマーチによる自己遮蔽** と **Henyey-Greenstein 位相関数** を追加し、ボリューム感のある陰影が出るようになりました。これまでは「上下方向の輝度ランプ」だけで雲塊の凹凸が表現できていませんでしたが、各ビューサンプルから太陽方向に数ステップマーチして密度を Beer-Lambert で積分し、サンプル点の照明を `ambient + sunlit × lightTransmittance × phase` に変更。HG の `phase` は太陽方向側の縁を明るく(シルバーライニング)、影側を暗くします。`CloudSettings` に `lightSamples` (既定 6、0 で従来挙動)、`lightStepMeters` (既定 80m)、`phaseEccentricity` (HG g、既定 0.4)を追加。`CloudRenderShaderConstants` を 52 → 56 DWORDs に拡張、ルートシグネチャも合わせて更新。
- `Mask Fluvial` を `Mask Blend` の入力として直接接続できるようにしました。これまではマスクグラフ評価器が「マスク専用ノード(Mask Noise / Mask Blend)」しか辿らず、ハイトフィールド入力を持つ `Mask Fluvial` は途中で空マスクになっていました。`EvaluateMaskGridForNodeCached` に `MaskFluvial` の分岐を追加し、内部でハイトフィールドパイプラインを評価して `grid.mask` を `MaskGrid` として持ち上げる経路を実装。これに合わせて `BuildMeshFromHeightPipelineCached` のパイプライン評価部分を `EvaluateHeightPipelineCached` ヘルパに切り出してメッシュ生成と独立させました。`Mask Blend` の上流ノードフィルタも `Mask Fluvial` を含むよう拡張。キャッシュは既存のハイトフィールドキャッシュをそのまま再利用するので、合成のために再評価が走ることはありません。
- `Mask Fluvial` ノードを含むプロジェクトを保存・再読み込みすると、ノード種別が `Mask Blend` として復元されてしまう不具合を修正しました。原因は `kMaxSerializedNodeKind` が `MaskBlend (=10)` のまま固定で、新規追加した `MaskFluvial (=11)` が読み込み時に `std::clamp` でひとつ手前のノード種別へ詰められていたためです。`IsTerrainNodeKind` の許可リストにも `MaskFluvial` を追加。
- 評価中の「計算中」バッジが、preview 対象のノードだけでなく **その時点で実際にカーネルが走っているノード** を追って表示されるようになりました。ハイトフィールドソースから始まり、上流から下流の各 op、最後に preview 対象、という順でバッジが移動するので、長い評価中にどのノードで時間がかかっているかが視覚的にわかります。実装は `node_graph` に `std::atomic<GraphId>& CurrentlyEvaluatingNodeId()` を追加し、各ノードのキャッシュミスパスでカーネル実行直前に `store()`、`Evaluate()` 終了で 0 にクリア。UI は atomic を読んで一致するノードに badge を出します。すべてキャッシュヒットだった場合は従来どおり preview 対象のみに表示。
- 上部メニューバーから `エクスポート` メニューを削除しました。
- `docs/nodes/` 配下を `heightfield/` と `mask/` に分け、ハイトフィールド系ノードとマスク系ノードのドキュメントをカテゴリ別に整理しました。
- `Mask Fluvial` ノードの簡易ドキュメントを追加し、`Mask Noise` / `Mask Blend` の説明をアルゴリズム・用途・キャッシュ挙動まで含めて拡充しました。
- ノードグラフの右クリック追加メニューを `ハイトフィールド` と `マスク` のカテゴリに分け、ノード数が増えても探しやすい構成にしました。
- グリッド表示設定をアプリ設定ではなくプロジェクト設定として保存するように変更しました。起動直後の新規プロジェクトでは既定どおりグリッドが表示されます。
- ノードグラフ上のノードアイコン色を、ハイトフィールド系は緑、マスク系はオレンジに統一しました。
- ハイトフィールドから川筋マスクを抽出する `Mask Fluvial` ノードを追加しました。入力ハイトフィールドにフロー累積を流し、`Output Curve` に応じて連続的なドレナージマップ(Log)、二値の川筋(Threshold)、線形マップ(Linear)としてマスク出力します。GIS 標準の D8(最急降下、細い線)と MFD(複数方向重み付き分配、面的)を切り替え可能。
  - 既定は **Log カーブ + D8 + 閾値 0%** で、樹枝状の川筋ネットワーク全体が見える GIS 標準のドレナージマップ表示。`Gamma` (既定 0.5) で細い支流の明度を調整、小さいほどコントラストが上がり全枝が見えます。`Threshold` モードに切り替えると従来の閾値ベースの二値川筋抽出になり、`Softness` / `Edge Power` で川縁を整えられます。`Pit Fill Iterations` (既定 8) で局所窪みを埋め、0 にすると湖が残ります。
  - 出力は `Mask` の 1 本のみ(マスク系ノードとして純粋に扱うため、Heightmap パススルー出力は持ちません)。ノード本体を選んだだけで自動的に Mask プレビューに切り替わるよう、Heightmap 出力を持たず Mask 出力のみのノードでは `SetPreviewNode` が `previewField` を Mask にする調整を入れています。
  - 実装: 標高降順に並べたインデックス列を 1 周することで O(N log N) で累積を計算。MFD は重み `slope^p` で下流分配、`MFD Exponent` (既定 4.0) で D8 寄り↔面的の中間を取れます。Pit Fill は「8 近傍がすべて自分以上のセルを min_neighbor + ε に持ち上げる」反復で、深い穴は反復回数で埋めます。Log マッピングは `pow(log(1 + max(0, accum - threshold)) / log(1 + maxAdjusted), gamma)`。
  - 並列化: Pit Fill (Jacobi、行並列)、最大値リダクション、最終マスク変換 (`std::log` / `std::pow` が重いので効果大) を `ParallelForRows` で並列化。インデックスソートも `std::execution::par`。累積ループ自体は標高順依存があるため逐次のままです。
  - キャッシュ: 既存ノードと同じく入力ハッシュ + パラメータハッシュで個別キャッシュ。
  - `.terrainproj` の `nodes[].maskFluvial` に保存。`outputCurve` / `gamma` フィールドが追加。
- ノード関連ドキュメントを `docs/nodes/` 配下へ整理しました。ノード候補一覧は `docs/nodes/node_candidates.md`、Fluvial Erosion 関連資料は `docs/nodes/heightfield/fluvial_erosion/`、Multi-Scale Erosion 関連資料は `docs/nodes/heightfield/multi_scale_erosion/` に移動しています。
- 既存ノードの簡易ドキュメントと `docs/nodes/README.md` の索引を追加しました。`Heightmap Load`、`Shape`、`Heightmap Blur`、`Erosion Noise`、`Mask Noise`、`Mask Blend` の各ノードについて、入出力・主な設定・用途メモを `docs/nodes/heightfield/<node>/` または `docs/nodes/mask/<node>/` 配下へまとめています。
- 天球・雲・遠景フォグの不自然なアートハックを物理寄りに整理しました。多重散乱 LUT を入れたあとも残っていた以下の重ね掛けや強制補正を整理しています。
  - **天球の地平ベール**: 輝度を `lerp(0.56, 0.82, ...)` で強制底上げしていた `paleHaze` を撤去し、地平帯は実際の散乱結果から計算した輝度を中心に微妙に脱飽和+わずかにクール側に寄せるだけにしました。底上げが無くなったため夕焼け時に灰色寄りピンクへ潰れていた挙動が解消し、Mie 強度を上げても暖色が素直に残ります。haze の効きを `dayHaze = smoothstep(0.05, 0.30, sun.y)` に絞り、太陽高度が低い時は完全にスキップするように。地平帯の幅も `abs(ray.y) ≤ 0.20` に狭めました。
  - **下半球フェード**: 水平線下 `smoothstep(0, 0.55, -ray.y)`(約 33°)で groundAlbedo に向けてフェードしていたのを `0.08`(約 4.6°)に縮小。雲フィールドの外側など空+地面が同時に見える構図で、地平直下に大気色が滲みすぎる問題を解消。
  - **太陽グロー**: `pow(cosθ, 256)` で鋭いスポット状の追加グローを足しており、Mie HG (g=0.76) の前方散乱ピークの内側に二重の縁が見えていました。`pow(cosθ, 96)` に緩めて、係数を 0.4 倍することで Mie コーンに自然に重なる柔らかいブルームに変更。
  - **遠景フォグの 0.65 クリップ**: `mesh_preview.hlsl` の aerial perspective が `saturate(fogFactor * 0.65)` で頭打ちになっており、超遠景の地形が完全に空に溶け込まずコントラストが残っていました。クリップを撤去し、消衰係数を `(70e-6, 18e-6)` から `(45e-6, 12e-6)` に下げて、Beer-Lambert の指数減衰だけで決まるように。フォグ色も独自の paleHorizon 補正を外し、空シェーダーと同じ `skyHorizonColor` を直接使うので地平の合流ラインで色がずれなくなります。
  - **雲底のシェーディング**: `lerp(ambient, sunlit, yNorm)` で既に上下の照明グラデーションが付いているのに、追加で `topLight = lerp(0.85, 1.05, yNorm)` を掛けて二重補正していたのを削除。`atmosphereSkyColor × 2.5` を `× 1.5` に下げ(多重散乱 LUT で天頂値自体が以前より明るいため)、Mie 内部多重散乱の脱飽和近似 `0.65` も `0.35` に緩めました。これで夕焼け時の暖色が雲底にも反映されます。
- 上記すべての変更で `SkySettings` / `CloudSettings` のパラメータや UI、`.terrainproj` の保存内容には変更ありません。`atmosphereDensity` / `mieStrength` / `sunGlowStrength` の意味は同じですが、見た目は (特に夕焼け・遠景) かなり変わるので、好みの値は再調整してください。
- `Sun Elevation` スライダーの最低値を `1°` から `-10°` に拡張しました。`0°` (地平線) と地平直下 (夜遷移) の天球・雲の挙動を確認できます。`.terrainproj` ロード時の clamp も同範囲に合わせています。

## 3.2.6 - 2026-05-08 00:58

- `Mie 強度` の既定値を `1.0` から `0.2` に下げました。日中の地平に出る暖色帯や暗い境界は Mie が強いほど目立つため、編集ビューでは低めの Mie を標準にし、強い霞や太陽グローが欲しい場合に上げる運用へ寄せました。

## 3.2.5 - 2026-05-08 00:55

- 太陽の反対側の地平がくっきりした水平線に見えていたため、天球シェーダーの地平ベールを細い線から上下に広がる滑らかな層へ変更しました。地平より下側は少し広めに覆い、暗い境界が大気に埋もれやすくなるよう調整しています。

## 3.2.4 - 2026-05-07 21:52

- 天球シェーダーの地平帯に青白い大気ベールを強めに重ねるよう調整しました。日中の地平に出る橙色の帯や、地平直下の黒っぽい境界を大気の厚みで覆いやすくしています。夜間に不自然に白くならないよう、太陽高度が低いときは効果を抑えます。

## 3.2.3 - 2026-05-07 21:43

- 3D ビューポート左上の `表示` メニューで、チェックボックスを小さなメニュー用トグルに変更しました。`FPSを表示` と `グリッドを表示` を表示モードより上へ移動し、天球モード時は表示モードの下に `雲を描画` を出すように整理しました。

## 3.2.2 - 2026-05-07 21:43

- 天球設定から独立した `遠景フォグ強度` を削除し、遠景フォグと地平ヘイズは `大気厚み (密度)` と `ヘイズ (Mie 強度)` から自動で決まるようにしました。`大気厚み` がプレビュー再描画条件から漏れていて、スライダー操作が即時反映されないことがあった問題も修正しました。空シェーダーの地平付近は低彩度の青白い haze に寄せ、日中の地平に出る橙色の帯を目立ちにくくしました。
- 天球とボリューム雲システムの解説ドキュメント [docs/sky_and_clouds/sky_and_clouds.md](sky_and_clouds/sky_and_clouds.md) を追加しました。Nishita 単散乱 + Hillaire 多重散乱 LUT の物理モデル、太陽自動色、半球 ambient ライティング、雲のレイマーチ、雲影、aerial perspective、各 D3D12 リソースとシェーダーの対応関係、`SkySettings` / `CloudSettings` のパラメータ表、よくあるトラブルシューティングをまとめています。
- 大気の厚みを調整できるパラメータを追加しました。`SkySettings` に `atmosphereDensity` (Rayleigh 散乱係数 β_R の倍率、地球標準 1.0) を追加。`atmosphereDensity` は sky shader / multi-scatter LUT / CPU port すべてで β_R を倍率乗算する形で連動し、薄い大気 (火星っぽい透明感) から濃い大気 (深い青空・強い夕焼け) までシームレスに変化します。遠景フォグは独立スライダーではなく、`atmosphereDensity` と `mieStrength` から自動で決まります。多重散乱 LUT のキャッシュキーに密度を含めるよう拡張。
- 大気密度の追加に伴い `sky.hlsl` 内の二箇所目の `AtmComputeScattering` 呼び出し (地平より下のレイ用) で新しい `density` 引数が抜けていたため、HLSL コンパイルがランタイムで失敗して天球パスが表示されなくなる回帰がありました。引数を追加。
- 雲底が青白く氷のように見えていた問題を修正しました。`atmosphereSkyColor × 2.5` をそのまま底面 ambient に使うと Rayleigh の青を保ったまま増幅されてしまうため、輝度 (NTSC 重み) を計算して 65% グレーに寄せる lerp を入れました。実際の積雲は内部で Mie 散乱を多重に繰り返して色を脱飽和させるので、その効果の近似です。あわせて地面 bounce 寄与を `atmosphereSunColor × 0.5` に上げて、底面が中性〜やや暖色寄りに落ち着くようにしています。夕焼け時はサン透過色が暖色に転ぶので雲底にも橙の bounce が乗ります。
- 天球に Hillaire 2020 の multi-scatter LUT を追加して UE5 寄りの絵に近づけました。32×32 R16G16B16A16_FLOAT の 2D LUT を `shaders/atmosphere_multiscatter.hlsl` の compute shader で生成し、各テクセル (cos sun zenith × altitude) で 64 方向 × 20 ステップを積分して `L_avg / (1 - F_avg)` の等比級数で無限次散乱を外挿します。`AtmComputeScattering` のシグネチャを `(LUT, sampler, useMultiScatter)` を取るよう拡張し、各レイマーチステップで isotropic な多重散乱項を加算。これで日中の地平が単散乱の暖色寄りから青白寄りに大きく改善 (Hillaire 近似で完全には消えませんが UE5 でも残るのと同程度)。LUT は `mieStrength` / `mieEccentricity` 変更時のみ再生成。
- 雲のライティングを輝度スケール混合に修正しました。`atmosphereSunColor` は大気透過率 (0-1 dimensionless)、`atmosphereSkyColor` は天頂散乱輝度 (radiance) で単位が異なるため、底面 ambient の `× 0.4` だけだと極端に青暗くなっていました。`atmosphereSkyColor × 2.5` で天頂値から半球輝度を近似 + `atmosphereSunColor × 0.35` で地面 bounce を加える形にし、雲底が明るく自然な見た目に。夕焼け時も bounce 経由で暖色が雲底に届きます。
- 天球を物理ベースの大気散乱モデルに置き換えました。`SkyMode::Procedural` (手動で天頂/地平/地面/太陽色を指定するグラデーション) を `SkyMode::Atmospheric` に置換。Nishita 単散乱 (Rayleigh + Mie + Henyey-Greenstein 位相) を `shaders/atmosphere.hlsli` の共通ヘッダにまとめ、sky shader と CPU 側 `atmosphere_cpu` 名前空間で同じ式を共有します。太陽の高度 (sun elevation) を変えるだけで「青空 → 黄昏 → 夕焼け → 夜」の遷移が自動的に起こります。雲のレイマーチも `atmosphereSunColor` / `atmosphereSkyColor` を受け取り、夕焼け時に雲が橙色に染まる等の連動が成立します。地形 PBR の hemisphere ambient と direct sun も CPU 側で 4 方向 (zenith / horizon / ground / sun) を毎フレームサンプリングして mesh cbuffer に流すので、シーン全体が一貫した光環境で動きます。
- `SkySettings` の手動色 (zenith / horizon / ground / sun) と `horizonSoftness` を削除し、代わりに `mieStrength` (haze)、`mieEccentricity` (HG g、太陽グロー鋭さ)、`groundAlbedo` の 3 パラメータを追加。`SkyShaderConstants` を 40 → 28 DWORDs に削減、`CloudRenderShaderConstants` は雲の大気連動用 2 色追加で 44 → 52 DWORDs。`.terrainproj` の `settings.sky` も新フォーマットで保存・ロード。
- Nishita 単散乱の典型的アーティファクト「日中でも地平が暖色寄り」を緩和する調整を入れました: `kAtmCameraHeight` を 1m → 500m (海抜 1m から地平方向だと光路が異常に長くて Rayleigh reddening が極端になる)、ビューサンプル数 16 → 32 (近距離の青濃い領域のアンダーサンプリング解消)。Multi-scatter LUT は未実装なので、地平が完全に青白くは見えませんが、実用範囲には収まります。
- 地形の PBR ライティングが天球の色から駆動されるようにしました。これまで mesh shader の ambient (`skyTint`) と sun tint は HLSL ソース内のハードコード値 (`(0.42, 0.45, 0.45)` 等) でしたが、`CloudShadowMeshConstants` cbuffer (b1) に sky の zenith / horizon / ground / sun の 4 色を追加し、地形の半球ライティングを天球設定に連動させました: 法線が上向きの面は zenith 色、地平方向の面は horizon 色、下向きの面は ground 色 (= 地面からの bounce 光) を ambient としてサンプル、direct sun は `skySunColor` を使用、bounce/fill 項も groundColor をベースに。`SkyMode::SolidColor` モードのときは ambient を viewportBackground 色に統一して、太陽色は引き続き編集可能なままにします。これで「夕焼けの空にしたら地形も暖色寄りになる」「青い空の下では地形に青みが乗る」等、天球を変えると一貫してシーン全体が連動します。`CloudShadowMeshConstants` は 32 バイト → 96 バイトに拡張 (CBV は 256 バイト確保なので余裕あり)。
- 天球の地平ソフトネスのデフォルト値を 1.4 → 0.5 に下げました。`pow(ray.y, softness)` で gradient を計算しているため、softness が 1 より大きいと地平色が空高くまで広がり、`ray.y = 0.5` (= 30° 上空) で 60%以上が地平色という不自然な見え方になっていました。0.5 だと同じ位置で約 29% 地平色に落ち、実際の空に近いタイトな地平帯になります。値の意味は変わっていないので、既存プロジェクトのスライダー値はそのまま (新規プロジェクトのみデフォルトが 0.5)。
- 天球の地面方向 (`ray.y < 0`) の色を独立した `groundColor` として設定できるようにしました (GeoGen 風の参考画像に合わせて、地平より下の色を別パラメータに分離)。`SkySettings` に `groundColor` (デフォルト 暗めの青グレー) を追加し、`sky.hlsl` のグラデーションを「ray.y >= 0 で horizon → zenith」「ray.y < 0 で horizon → ground」の二面構成に。`horizonSoftness` は両側で共有するので地平の連続性は保たれます。`SkyShaderConstants` に float4 を 1 つ追加して 36 → 40 DWORDs に拡張、ルートシグネチャの `Num32BitValues` も 40 に。表示設定パネルに「地面色」を追加、`.terrainproj` の `settings.sky.groundColor` に保存します。
- 3D ビューポート左上に `表示` メニューを追加しました。メニュー内でライティングモードを `シンプル` / `天球` から選択でき、`天球` 選択時は `雲を描画` チェックボックスでボリューム雲の表示を切り替えられます。
- `Lighting Mode` と `Sky Mode` をユーザー向け UI では `表示モード` に統合しました。`シンプル` / `PBR` / `天球` の3択にし、左上の表示メニューは軽い切り替え、表示設定パネルは選択中モードに応じた詳細設定を表示する構成にしました。
- 表示設定パネルの詳細設定を `地表` / `太陽` / `影` / `天球` / `ボリューム雲` の順に整理しました。ビューポート左上の表示メニューと表示設定パネルに、FPS 表示のオン/オフを追加しました。
- `.terrainproj` の `settings.display` に表示モードと FPS 表示フラグを保存するようにしました。プロジェクトごとに `シンプル` / `PBR` / `天球`、雲の有効状態、FPS 表示状態を復元できます。
- 天球表示モードと雲表示フラグを `app_settings.json` の保存対象から外し、プロジェクトファイル側の表示設定を優先するようにしました。表示設定パネルのチェックボックスは小さめに表示するよう調整しました。
- 3D ビューポートの既定表示を `シンプル` に変更し、新規起動時は天球と雲をオフにするようにしました。
- 水平に近い視線方向で雲を横切る一直線が見えていた問題を修正しました。原因は雲のレイマーチを `tExit = min(tExit, 50000)` で固定 50km にカットしていたことで、`tEnter > 50000` の grazing 角度のレイは雲が一切描画されないため、画面上に「雲あり / なし」の境界が水平線として現れていました。雲はそもそも垂直スラブ `[altitudeMin, altitudeMax]` と水平 disc `(field radius)` の交差 (= 有限の円柱) で囲まれているので、レイマーチ範囲もこの円柱との交差で求めるように `cloud_render.hlsl` を書き換えました。スラブと disc を別々に解いて (ray-cylinder 解析交差)、両者の `[tEnter, tExit]` の重なりを取ります。これで全レイが自然に bound され、固定の 50km cap を撤去しても問題ないようになりました。
- カメラを水平に近い角度に振ったときの雲に横縞が出ていた問題を修正しました。これまでレイマーチは `(tExit - tEnter) / qualitySamples` で等分していて、水平に近いレイは雲帯を横切る距離が数十 km に伸び、1 サンプル数百 m〜1km とノイズの解像度を大きく下回ってバンディングが発生していました。雲帯厚みを基準にした `idealStep` を計算してステップ数を `qualitySamples`〜`qualitySamples × 4` の範囲で自動調整 (= 水平レイでは自動的に多くサンプリング、cap 4× で性能保護)。さらに per-pixel ハッシュベースのジッターをサンプル位置に加えて、残ったバンディングをノイズに変えます。
- 薄い雲が背景の空より暗く見えていた問題を修正しました。雲シェーダーは `accumulated += lit * dA` でレイマーチを積分していて、これは既に alpha 乗算済み (premultiplied) の色を出します。にもかかわらずブレンド状態が `SRC_ALPHA / INV_SRC_ALPHA` で 2 度目の alpha 乗算をしていたため、たとえば alpha = 0.2 の薄い雲では `0.2 × 0.2 + 0.8 × sky = 0.04 + 0.8 × sky` となって空より暗くなっていました。`ONE / INV_SRC_ALPHA` (premultiplied) に変更して `accumulated + (1 - alpha) × sky` で正しく合成するようにしました。
- 雲のテクスチャに縦/横の継ぎ目が見えていた問題を修正しました (特にフィールド中央 = 地形中心で目立っていました)。`cloud_density.hlsl` の Perlin 生成を周期版 `Perlin3DPeriodic` / `Fbm3DPeriodic` に置き換え、整数座標を `mod period` してから勾配ハッシュを引くようにしました。`Fbm3D` の各オクターブでも周波数倍と同じく period も倍にすることで、`uvw = 0` と `uvw = 1` で勾配配置が一致してテクスチャがシームレスにタイリングします。`SamplerState` の WRAP モードと組み合わさってフィールド内のどこでも継ぎ目が出なくなります。
- 雲が地平線まで無限に続いてしまう問題を修正しました。`CloudSettings` に `fieldRadius` と `fieldFalloff` を追加し、地形の中心 (mesh bounds の中心) を原点に円形のフィールド境界を設けて、その外側では雲の密度を 0 にフェードアウトします。シェーダー (cloud_render / cloud_shadow) の `SampleCloudDensity` で `ComputeFieldFade` を乗算する形。デフォルトは半径 6000m / フェード 2000m なので、典型的な 4-8km 地形だと地形の周りだけに雲が出る形になります。表示設定パネルに「Field Radius」「Field Falloff」を追加し、`.terrainproj` にも保存します。
- レビューフィードバックの反映: (1) 表示設定パネルの `Lighting Mode` から `Shadow Debug` を削除しました。シェーダー側の `DebugShadowColor` 分岐も死コードになるので除去。`.terrainproj` 経由で値 2 が入っていた古いプロジェクトは load 時に 0..1 にクランプされます。 (2) 表示設定パネルの「天球」「ボリューム雲」の前に `ImGui::SeparatorText` を入れて区切りを入れました。雲セクション側のトグルラベルも「ボリューム雲」→「有効」に短縮。 (3) 雲の色が灰色っぽく見えていた問題を直しました。`cloud_render.hlsl` のシェーディングが太陽の高度と垂直プロファイルで二重に減衰させていたため、白を指定しても 50% 程度のグレーに収束していました。シンプルに `cloudColor × lerp(0.85, 1.05, yNorm)` の top-lit ランプだけにして、上面でフルカラー、下面で 85% に落ち着く形に。
- 雲が地形に影を落とすようになりました (Phase 3)。compute shader [shaders/cloud_shadow.hlsl](shaders/cloud_shadow.hlsl) で、地形フットプリント上空に置いた 1024² R8_UNORM の top-down ビューから太陽方向にレイマーチして雲帯の透過率を 2D テクスチャに焼きます。これを mesh shader でサンプルし、地形頂点 (x, z, y) を太陽方向に altitudeMin まで投影した位置で透過率を読んで sun + ambient に乗算します。CloudSettings に Shadow Strength / Resolution / Samples を追加し、UI と `.terrainproj` に保存します。雲影テクスチャは毎フレーム再生成 (Sun direction や Wind offset の変化に追従)、約 0.5ms / 1024². mesh root signature に b1 の root CBV (CloudShadowMeshConstants) と t1 のテクスチャテーブルを追加 (合計 64 DWORD ぴったり)。雲が無効化されたときは 1×1 の白テクスチャをバインドしてシェーダー側でゼロ強度として扱います。
- ボリューム雲を地形深度に対応させました。雲が常に地形の奥側に描画されてしまっていた問題を修正します。深度バッファを `R32_TYPELESS` で再作成して DSV (`D32_FLOAT`) と SRV (`R32_FLOAT`) を併存させ、レンダリング順を **Sky → Mesh → Cloud (depth SRV 読み)** に変更しました。雲のフルスクリーンパスは各ピクセルで深度を `Load` して NDC z から view-space 距離に逆変換し、`tExit` を地形までの距離でクランプします。これで山の手前にも雲が出る (距離が近い山が雲を遮蔽し、遠い山には雲がかかる) ボリューム表現になります。深度バッファは雲パスの後に SRV 状態のまま残し、次フレーム冒頭で `DEPTH_WRITE` に戻します。
- ボリューム雲 (Volumetric Clouds) のレイマーチ描画を追加しました。`GraphSettings::clouds` (`CloudSettings` 構造体) を追加し、3D 密度ボリューム (128³ R8_UNORM、約2MB) を [shaders/cloud_density.hlsl](shaders/cloud_density.hlsl) の compute shader で生成します (Seed 変更時のみ再生成)。フルスクリーン pixel shader [shaders/cloud_render.hlsl](shaders/cloud_render.hlsl) でカメラから雲帯 (Altitude Min..Max) との交差区間を `qualitySamples` (デフォルト 32) サンプルでレイマーチし、Beer-Lambert 透過率を累積。SRC_ALPHA / INV_SRC_ALPHA で空に α ブレンドします。描画順は **Sky → Cloud → Mesh** で、地形が手前で雲を遮蔽する「山が雲を突き抜ける」表現になります。設定パネルに Coverage / Density / Altitude Min/Max / Horizontal Scale / Absorption / Cloud Color / Wind Direction / Wind Speed / Quality (samples) を追加し、`.terrainproj` の `settings.clouds` に保存します。
- 生成天球 (Procedural Sky) モードを追加しました。`SkyMode { SolidColor, Procedural }` を `GraphSettings::sky` に持たせ、Procedural モードでは [shaders/sky.hlsl](shaders/sky.hlsl) のフルスクリーンパスで天頂↔地平のグラデーション + 太陽ディスク + グローを描画します。表示設定パネルに「Sky Mode / 天頂色 / 地平色 / 太陽色 / 太陽サイズ / 地平ソフトネス / 太陽グロー」を追加しました。雲システム (Phase 2/3) の前準備として独立して動く形に分離しています。設定は `.terrainproj` の `settings.sky` に保存されます。SolidColor モードでは従来どおりビューポート背景色が使われます。
- Mask プレビュー専用の軽量メッシュ生成パス `BuildFlatMaskMesh` を追加しました。Mask Noise / Mask Blend のプレビューはハイトフィールドが平面 (y=0) なので、`BuildMeshFromHeightfield` の壁・底面・三角形毎の法線累積・`std::unordered_set` によるエッジ重複除去を全て省略します。頂点・三角形・エッジは規則格子の構造が決まっているのでインデックスで直接書き込み、`ParallelForRows` で並列化します。Preview Resolution = 2048 でメッシュ生成 (Release) が秒オーダーから 100ms 級まで短縮される見込みです。
- マスクノード (`Mask Noise` / `Mask Blend`) の評価結果をノード単位でキャッシュするようにしました。`NodeGraph` に `maskCache_` を追加し、各ノードの (入力ハッシュ, パラメータハッシュ) が変わらない限り再生成しません。これまでマスクプレビューは毎回ゼロから走り直していたため、無関係なパラメータを触っただけでも上流の Perlin / fBM が再計算されていました。`HashMaskNoiseSettings` / `HashMaskBlendSettings` を実際にキャッシュキーとして使い、`ApplyEvaluationResultFrom` でも非同期評価結果と一緒に伝播させます。
- `GenerateMaskNoise` の二重ループを `ParallelForRows` で並列化しました。512² × 4 オクターブで約 100 万回走る Perlin/fBM 評価をスレッド並列で消化します。
- `Mask Noise` ノードに GPU compute バックエンドを追加しました (`MaskNoiseBackend::GpuCompute` を既定値)。`shaders/mask_noise_compute.hlsl` の `CSGenerate` カーネルが CPU 側 `mask_noise::Hash3` / `Gradient2` / `Fade` / `Perlin2D` / `Fbm2D` をそのまま HLSL 移植したもので、出力は CPU 版とほぼ一致します。プロパティパネルの `Backend` ドロップダウンで CPU / GPU を切り替え可能で、`.terrainproj` にも保存します。GPU 経路は MSE と同じく `RunMaskNoiseCompute` をワーカースレッドからメインスレッドへ promise/future で往復させて D3D12 呼び出しをメインスレッドに集約します。シェーダーのコンパイルや実行に失敗した場合は自動で CPU 版にフォールバックします。`HashMaskNoiseSettings` にバックエンド種別も含め、切り替え時にキャッシュが無効化されます。

## 2.0.3 - 2026-05-07 04:24

- ノード描画が入力ピンを1つしか表示していなかったため、`Mask Blend` の2番目の入力 `B` が見えない問題を修正しました。複数入力ピンを持つノードはすべての入力行を表示します。

## 2.0.2 - 2026-05-07 04:14

- 既存プロジェクトを読み込んだあとに新しいノードを追加すると、ノード ID とピン ID が衝突して Dear ImGui の ID conflict 警告が出ることがある問題を修正しました。ノード・ピン・リンクの新規 ID は、読み込み済みグラフ内の最大 ID より後ろから共通採番します。

## 2.0.1 - 2026-05-07 04:02

- 表示設定の既定値を調整し、`Mesh Preview` はオン、`Wireframe` はオフで起動・リセットされるようにしました。
- `Grid` は既定でオンのままにし、オフにしたときは `Grid Cells` / `Grid Cell Size` / `Grid Color` を表示設定から隠すようにしました。

## 2.0.0 - 2026-05-07 03:38

- `Output Mesh` ノードと専用の最終メッシュ評価経路を削除し、プレビューで評価済みのメッシュをそのまま OBJ エクスポートに使う構成へ整理しました。`finalMesh` / `finalDirty` / `EvaluateFinal` / `OutputMeshSettings` を廃止し、グラフの既定プレビューは `Graph` として扱います。
- 新しいマスクノード `Mask Noise` と `Mask Blend` を追加しました。`Mask Noise` は入力なしの Perlin / fBM 生成ノードで、`Seed` / `Octaves` / `Frequency` / `Lacunarity` / `Persistence` / `Simulation Resolution` を調整して 0–1 の Mask Texture を出力します。`Mask Blend` は 2 本の Mask 入力を `Add` / `Multiply` / `Min` / `Max` のいずれかで合成し、`Blend Intensity` で `A` と合成結果の補間量を指定します。どちらか片方の入力が未接続の場合はもう一方をそのまま通します。
- マスクノード (`Mask Noise` / `Mask Blend`) の出力ピンを選択した際のプレビューを、平面メッシュ + Mask テクスチャの形で表示するようにしました。これらのノードはハイトフィールドを持たないため、3D ビューはフラットな板の上に Mask の濃淡を描画し、2D ビューは Mask 値を直接マップ表示します。`HeightfieldPreviewField` に `Mask` を追加し、タイトルにも `Mask Preview` と表示します。
- `NodeGraph::Evaluate` にマスク専用ノードのプレビュー分岐を追加しました。プレビュー対象ノードが `Mask Noise` / `Mask Blend` の場合は通常のハイトフィールドパイプラインを経由せず、`EvaluateMaskAsHeightfield` で Mask グラフを再帰的にたどり (Mask Blend の 2 入力分岐に対応)、フラット平面の `HeightfieldGrid` に Mask 値を載せて描画します。

## 1.0.1 - 2026-05-07 03:02

- 表示設定の `Resolution` と `Simulation Resolution` を、任意数値入力ではなく `128` / `256` / `512` / `1024` / `2048` のプリセット選択に変更しました。

## 1.0.0 - 2026-05-07 02:54

- 旧 SDF 系ノードと互換コードを削除し、評価パイプラインをハイトフィールド専用に整理しました。`Primitive SDF` / `Noise Warp` / `Crack Field`、SDF プレビュー生成、SDF デバッグ OBJ 出力、`Output Mesh` の `Iso Value` は廃止しました。
- 表示設定の `Resolution` 上限を `512` から `2048` に引き上げ、保存済み設定の読み込み時も `2048` まで保持できるようにしました。
- `2Dビュー` が表示用メッシュ解像度ではなく、`Simulation Resolution` の評価済みハイトフィールドを元にマップ表示するようにしました。表示負荷が高い場合は画面密度に合わせて間引きつつ、元のシミュレーション解像度をステータスに表示します。
- `Heightmap Blur` の水平・垂直 Gaussian パスの z ループを `std::execution::par` で並列化しました。Multi-Scale Erosion と同じ `ParallelForRows` ヘルパーを共有します。マルチコア環境で大半径やマルチイテレーション時の評価時間が短縮されます。
- 実験的な `Fluvial Erosion` (KTT) ノードを削除しました。`Multi-Scale Erosion` ノードが本命の浸食ノードとして定着したため、KTT 系の粒子輸送実装、GPU compute 経路、シェーダー (`shaders/fluvial_erosion_compute.hlsl`)、UI、シリアライズ、関連ヘルパー (`KttRandom2`、`ResampleHeightfieldGrid`、`SmoothHeightfieldHeights`、`AddResampledHeightDelta`) をまとめて除去しました。`docs/nodes/heightfield/fluvial_erosion/` のアルゴリズムガイドは履歴として残しています。
- `Multi-Scale Erosion` のプロパティパネルに `Stream Power` / `Thermal` / `Deposition` のセクション区切りを追加しました。3 パスのパラメータ群が視覚的に区別できなかった問題を解消します。
- `Multi-Scale Erosion` のプロパティパネルで `Backend` を一番上に移動し、CPU/GPU の切り替えを最初に確認できるようにしました。
- `Multi-Scale Erosion` に GPU Compute バックエンドを追加しました。`shaders/multi_scale_erosion_compute.hlsl` (Schott et al. の `erosion.glsl` / `thermal.glsl` / `deposition.glsl` を HLSL 移植) を D3D12 compute pipeline で実行します。プロパティの `Backend` で `CPU Reference` / `GPU Compute` を切り替えます。GPU 経路はバックグラウンド評価スレッドからメインスレッドへジョブを積んで D3D12 上で実行する KTT と同じスケジューラを使い、初期化や実行時に失敗すると CPU 版へフォールバックします。
- `Multi-Scale Erosion` のアルゴリズム入門ガイド `docs/nodes/heightfield/multi_scale_erosion/multi_scale_erosion_algorithm_guide.md` を追加しました。KTT のガイドと同じ形式で、SPE / Thermal / Deposition の各パスの直感的な役割、マルチグリッド処理の意味、解像度不変性の仕組み、見た目が崩れたときの調整順を解説しています。
- `Multi-Scale Erosion` にマルチグリッドピラミッド処理を追加し、`Use Multigrid` トグルで切り替え可能にしました (既定 ON)。粗い解像度 (`64`) から目標解像度へ `x2` でアップサンプルしながら段階的に浸食を適用する Schott et al. 論文本来の構成です。粗い段階で大局の谷ネットワークが決まり、細かい段階は細部の追加だけになるので、解像度を変えても大局構造がほぼ変わらなくなります。OFF にすると従来通り入力解像度のみで実行する単一段階モードになります。`Iterations` は Multigrid 有効時は各レベルでの反復数を意味します。
- `Multi-Scale Erosion` の Thermal の `matter` (= `eps × cellArea`) と Deposition の雨量基準 `cellArea` を、実 cellSize ではなく **基準 cellSize 4 m** (= 解像度 512 / 2048 m terrain) で固定するように変更しました。これまでは解像度を変えると Thermal の効きが cellSize² で変わって見た目が大きくドリフトしていた問題を解消します。基準 4 m での既存チューニングはそのままです。なお SPE の `stream` 累積は前進反復のため完全な解像度不変は構造上できず、解像度を上げる場合は `Iterations` の比例的な増量を推奨します。
- `Multi-Scale Erosion` の `Thermal Strength` 上限を `0.001` から `0.01` に引き上げました。タラス崩壊をより強くかけて V 字谷を U 字に丸めたい用途で頭打ちになっていたためです。
- プロパティパネルの float 行に `format` と `sliderFlags` の省略可能引数を追加し、`Multi-Scale Erosion` の `SPE Strength` (`%.5f`)、`Thermal Strength` (`%.6f`)、`Max Stream Power` (`%.0f`) を対数スケールスライダーで調整できるようにしました。`Thermal Noise Wavelength` も `%.4f` 表示にしました。既定値が小さく `%.3f` で常に `0.000` 表示になっていた問題を解消します。
- `Multi-Scale Erosion` の SPE / Thermal / Deposition の各反復で、行 (z 軸) ループを `std::execution::par` で並列化しました。出力配列はピンポンバッファでセル独立なのでデータ競合なし。マルチコア環境で評価時間が大きく短縮されます。
- `Multi-Scale Erosion` の Deposition パスで pit 判定を逆向きに移植していたため、反復するごとに非 pit セルへ sediment が無制限に蓄積し、針状のスパイクが伸び続ける不具合を修正しました。`deposition.glsl` の `if (!CheckPit(p)) sed = 0;` に合わせて、局所最小 (pit) のみが sediment を保持し、それ以外のセルは毎反復クリアされるようにしました。
- プロパティ行の「既定値に戻す」ボタンのツールチップに、戻し先の既定値を表示するようにしました。
- F12 でスクリーンショットを保存したあと、保存したファイルを Explorer で選択表示するようにしました。

- 新しい浸食ノード `Multi-Scale Erosion` を追加しました。Schott et al. "Terrain Amplification using Multi-scale Erosion" (SIGGRAPH 2024, MIT) のコンピュートシェーダー 3 本 (Stream Power Erosion / Thermal / Deposition) を CPU 移植し、グリッドベースの河川浸食・タラス崩壊・土砂堆積を 1 ノードに束ねました。出力ピンは `Heightmap` / `Flows` / `Deposits` の 3 つで、KTT (粒子ベース) と並んで使い分けられます。詳細は `docs/nodes/heightfield/multi_scale_erosion/multi_scale_erosion_node.md` を参照してください。

## 0.17.2 - 2026-05-06 00:42

- `Erosion Noise` ノードの既定値を調整しました。

## 0.17.1 - 2026-05-06 00:42

- プロジェクト読み込み時に `Erosion Noise` ノードが `Shape` ノードとして復元される不具合を修正しました。

## 0.17.0 - 2026-05-06 00:35

- 実験用ノード `Erosion Noise` を追加しました。clayjohn の Shadertoy "Eroded Terrain Noise" (MtGcWh) を移植し、入力ハイトフィールドの勾配を方向ヒントにして手続き的な侵食ノイズを上乗せします。シミュレーションではない疑似侵食フィルターです。
- `Erosion Noise` の既定値と強度スケールを Shadertoy の `MtGcWh` に近づけ、強度をメートル直指定ではなく元地形の高さレンジに対する割合として扱うようにしました。

## 0.16.6 - 2026-05-05 21:22

- ノード出力クリック時にプレビューだけを切り替え、ノード選択が同時に切り替わらないようにしました。

## 0.16.5 - 2026-05-05 21:19

- ノードのホバー枠と選択枠を水色で表示するようにしました。
- 出力ラベルにマウスオーバーしている間は、そのノードのホバー枠が反応して見えないようにしました。

## 0.16.4 - 2026-05-05 21:14

- ノード入出力ラベルを通常時は暗めのグレー、ホバー時は白、プレビュー選択中の出力はピン種別色で表示するようにしました。
- ノードのホバー枠色を通常枠色にそろえ、出力ラベル操作時にノード枠が反応して見えないようにしました。

## 0.16.3 - 2026-05-05 21:08

- ノード出力ラベルにマウスオーバーしたとき、ラベルを明るく表示するようにしました。
- ノード出力クリックはプレビュー対象の切り替えだけを行い、ノード選択はプロパティ表示だけを切り替えるようにしました。

## 0.16.2 - 2026-05-05 20:58

- パラメータ値が既定値から変更されているときも、項目ラベルを太字風に表示しないようにしました。

## 0.16.1 - 2026-05-05 19:44

- ウィンドウが非アクティブになるなど一時的にレイアウト可能幅が小さくなったフレームで、右ペイン幅やインスペクタ高さの保存値が縮まないようにしました。

## 0.16.0 - 2026-05-05 17:21

- デバッグ用の `Shape` ノードを追加し、`Hemisphere` と `Pyramid` のハイトフィールドを生成できるようにしました。
- `Shape` ノードに `Scale (m)`、`Relative Height (%)`、`Shape Type` を追加し、既存の地形処理ノードへ接続できるようにしました。

## 0.15.55 - 2026-05-05 17:07

- パラメータが既定値のとき、既定値に戻すボタンのアイコンを薄い色で表示するようにしました。

## 0.15.54 - 2026-05-05 16:35

- ハイトフィールドプレビューの側面と底面を高さ `0m` まで落とすようにし、グリッド面と底面の高さが揃うようにしました。

## 0.15.53 - 2026-05-05 16:31

- 3Dプレビューのグリッドで通常線を先に描き、中心軸を最後に描くことで、交差部分で軸色が通常グリッド線に混ざって見える問題を修正しました。

## 0.15.52 - 2026-05-05 16:25

- 3Dビューのグリッド描画をGPUプレビュー経路へ一本化し、通常描画側のグリッドフォールバックを削除しました。
- `Mesh Preview` がオフでもハイトフィールドプレビューは表示されるようにし、ハイトフィールドが非表示になる問題を修正しました。
- 3Dビュー上のグリッド情報表示を、現在の `Grid Cells` と `Grid Cell Size` に合わせるようにしました。

## 0.15.51 - 2026-05-05 16:17

- 3DプレビューのGPUグリッドで、中心のX軸を赤、Z軸を青で表示するようにしました。
- 通常描画とGPU描画でグリッド中心軸の色が揃うようにしました。

## 0.15.50 - 2026-05-05 16:12

- 表示設定に `Grid Cells` と `Grid Cell Size (m)` を追加し、3Dプレビューのグリッドのマス数と1マスの長さを変更できるようにしました。
- グリッドのデフォルトを `10 x 10`、1マス `100m`、色 `0.2` のグレーにしました。
- GPUプレビューと通常プレビューのグリッド範囲が一致するようにしました。

## 0.15.49 - 2026-05-05 16:07

- 3Dプレビューのグリッド描画をワイヤーフレーム用パイプラインから分離し、通常の深度テストでハイトフィールドに自然に隠れるようにしました。
- グリッドがハイトフィールド側面や底面の上に重なって見える問題を抑えました。

## 0.15.48 - 2026-05-05 16:03

- グリッド色対応でメッシュプレビューのシェーダー定数が増え、GPUプレビューがCPU描画へフォールバックしてハイトマップ表示が重くなる問題を修正しました。
- グリッド色は既存の描画定数を使って渡すようにし、GPUプレビューの軽い描画経路を維持するようにしました。

## 0.15.47 - 2026-05-05 16:00

- 起動直後などメッシュが空の状態で、3Dプレビューのグリッド描画が毎フレーム再生成されて重くなる問題を修正しました。
- GPUプレビューが空メッシュなどで描画できない場合も、軽量な通常描画でグリッドを表示するようにしました。

## 0.15.46 - 2026-05-05 15:56

- ハイトフィールドが読み込まれていない状態でも、3Dプレビューにグリッドを表示するようにしました。
- 表示設定に `Grid Color` を追加し、3Dプレビューのグリッド色を変更して保存できるようにしました。

## 0.15.45 - 2026-05-05 07:20

- `Fluvial Erosion` のCPU処理で、元ハイトマップへの局所的な押し戻しが強すぎて太い塊状の地形になる問題を修正しました。
- 山体保護は低周波のマクロ形状だけを弱く戻す処理に絞り、KTT らしい流路の削れ方を潰しにくくしました。

## 0.15.44 - 2026-05-05 07:15

- `Fluvial Erosion` のCPUマルチスケール反復数で、KTT HDA の `VoxelScale` に近づけるため `Feature Size` ではなく参照ディテールサイズ基準でスケールするようにしました。
- 解像度や `Feature Size` によって細部レベルの反復数が過剰になり、大きな山体が消えやすくなる問題を抑えました。
- 元ハイトマップから局所的に沈み込みすぎたセルを戻す保護を追加し、流路は残しながら地形全体のシルエットを維持しやすくしました。

## 0.15.43 - 2026-05-05 07:07

- `Fluvial Erosion` のCPUマルチスケール処理で、入力解像度より大きい内部レベルが同じ解像度へクランプされたときに重複実行される問題を修正しました。
- 特に256解像度で256レベルの侵食が二重にかかり、大きな山体が消えやすくなる問題を抑えました。

## 0.15.42 - 2026-05-05 07:02

- `Fluvial Erosion` のCPUマルチスケール合成で、大きな山体の低周波形状が削れすぎて消える問題を抑えるマクロ形状保護を追加しました。
- KTT HDA の `Smooth_Flows` / `Add_Detail_Pass` が元地形を参照して戻す性質に近づけるため、侵食量に応じて元地形の大きなシルエットを復元する処理を追加しました。

## 0.15.41 - 2026-05-05 06:56

- `Fluvial Erosion` の粒子輸送で、KTT HDA と異なる移動先セルの重複停止を外し、開始セルだけを同一反復内の重複判定に使うようにしました。
- KTT の `Transport_Particles` に近い流路継続になりやすいよう、未使用になっていた内部変数を整理しました。

## 0.15.40 - 2026-05-05 06:52

- `Fluvial Erosion` のCPUマルチスケール反復数を、KTT HDA の `VoxelScale` と `Erosion_Duration_Coefficient` を使う式に近づけました。
- 粒子輸送の最大ステップ数を KTT の `Channel_Length / (dx / Detail_Scale)` に近い計算へ変更し、細かい解像度で流路が伸びやすくしました。

## 0.15.39 - 2026-05-05 06:33

- `Fluvial Erosion` のCPU処理に、KTT HDA の `Update_Forces` に近い `wear` フィードバック付きの力場更新を追加しました。
- 粒子の開始位置をKTTの `Transport_Particles` に近い散布式へ変更し、同一反復内の重複侵食を抑えるマスクを追加しました。
- 削れた流路が次の粒子を誘導しやすくし、フラクタルな支流状の侵食が出やすい構成へ近づけました。

## 0.15.38 - 2026-05-05 06:21

- `Fluvial Erosion` の初期状態で地形の起伏が大きく均されすぎる問題を修正しました。
- KTT の粒子輸送から遠い独自のグリッド侵食パスを初期計算から外し、低解像度スケールの内部強度を下げました。

## 0.15.37 - 2026-05-05 06:07

- 互換性よりKTT寄せを優先し、`Fluvial Erosion` の保存データと設定構造からKTTのUIに存在しない `Backend`、`Use Advanced`、`Sediment Capacity`、`Deposition Rate`、`Level Strength`、`Seed` を削除しました。
- `Fluvial Erosion` の内部マルチスケール強度、土砂容量、堆積率、乱数シードは、ユーザー向けパラメータではなく固定の内部係数として扱うようにしました。

## 0.15.36 - 2026-05-05 06:07

- `Fluvial Erosion` のプロパティ表示から、KTT のUIパラメータではない `Backend`、`Use Advanced`、`Sediment Capacity`、`Deposition Rate`、`Seed`、`Large/Medium/Detail Scale`、`Level Strength` を外しました。
- `Channeling`、`Friction`、角度系、`Erosion Granularity`、`Flow Volume`、`Small Channel Influence`、`Sediment Velocity` は常に計算へ反映されるようにしました。
- `Erosion Granularity` の初期値をKTTの表示例に合わせて `10` に戻しました。
- `Fluvial Erosion` は、GPU実装がKTT寄せのCPU処理へ追従するまで内部的にCPU処理を使うようにしました。

## 0.15.35 - 2026-05-05 05:57

- `Fluvial Erosion` のCPUディテール復元を各スケールごとの加算から最終段の一回だけに変更し、元地形ノイズが重なってKTTらしさから外れる問題を抑えました。
- ディテール復元を侵食の出た領域へ寄せ、平坦部へ細かいザラつきが乗りすぎないようにしました。

## 0.15.34 - 2026-05-05 05:46

- `Fluvial Erosion` のCPU処理に、KTT HDA の `Smooth_Flows` / `Add_Detail_Pass` に近い元地形ディテール差分の復元パスを追加しました。
- 各スケールで平滑化した元地形との差分を侵食後へ薄く戻し、KTT から遠ざかる過剰な丸まりを抑えました。

## 0.15.33 - 2026-05-05 05:36

- `Fluvial Erosion` のCPU粒子輸送で、KTTの重複防止マスク相当の処理を粒子ステップごとに適用してしまい、初速0の粒子がほぼ動かなくなる問題を修正しました。

## 0.15.32 - 2026-05-05 05:26

- KTT HDA 内の `Transport_Particles` OpenCL カーネルを読み直し、CPU粒子輸送を KTT の前後サンプル平均、`carry`、`Channeling`、前後高さクランプの式へ近づけました。
- `Erosion Granularity` と `Small Channel Influence` の粒子発生判定を、KTT の `Granularity_Threshold` 式に近い形へ変更しました。

## 0.15.31 - 2026-05-05 05:12

- `docs/nodes/heightfield/fluvial_erosion/fluvial_erosion_hda_notes.md` の Terrain Editor 向け方針を、簡易MVPではなく、KTT の挙動と調整感へかなり近づける実装方針として書き直しました。

## 0.15.30 - 2026-05-05 05:02

- `Fluvial Erosion` のCPU flow accumulationをD8風の単一下流セルから、下がっている複数近傍へ勾配重みで分配する multi-flow 風の処理へ変更しました。
- CPUグリッド侵食パスも複数下流セルへ侵食・堆積を分配するようにし、格子方向へ寄った単線流路ではなく、面状の流れと枝分かれが出やすいようにしました。

## 0.15.29 - 2026-05-05 04:48

- `Fluvial Erosion` に KTT の UI に合わせた `Geological Age`、`Flow Volume`、`Small Channel Influence` を追加しました。
- `Feature Size` を通常表示へ移し、`Large Scale` / `Medium Scale` / `Detail Scale` は Advanced 側の補助パラメータとして整理しました。
- CPU侵食で `Geological Age` を成熟度、`Flow Volume` を水量、`Small Channel Influence` を浅い細リルの出やすさとして反映するようにしました。
- Advanced OFF でも通常表示の `Feature Size` と `Channel Length` が計算へ反映されるようにしました。

## 0.15.28 - 2026-05-05 04:35

- `Fluvial Erosion` のデフォルトを、丸くなりすぎないよう detail レベルと粒子密度を少し戻し、浅い micro pass を追加して斜面の細いリルを出しやすくしました。

## 0.15.27 - 2026-05-05 04:22

- `Fluvial Erosion` の新規ノード出力を `Heightmap` / `Deposits` / `Flows` に整理し、役割が曖昧になっていた `Fluvial Mask` 出力を外しました。
- 古いプロジェクト読み込み時に、`Fluvial Mask` 出力を補完せず、`Deposits` と `Flows` のみを補完するようにしました。

## 0.15.26 - 2026-05-05 04:10

- `Fluvial Erosion` の初期値とCPU侵食係数を穏やかにし、低流量の斜面で粒子が出すぎないようにして、細い縦筋と過剰な削り込みを抑えました。

## 0.15.25 - 2026-05-05 03:58

- `Fluvial Erosion` の `Large Scale` / `Medium Scale` / `Detail Scale` がドラッグ中に元の値へ戻り、変更できないように見える問題を修正しました。

## 0.15.24 - 2026-05-05 03:45

- `Fluvial Erosion` の `Flows` 出力を粒子が実際に通過した累積フィールドに変更し、`Deposits` 出力には粒子の減速・停止・出口で残った土砂も反映して、両方の見え方が同じ流路模様に寄りすぎないようにしました。

## 0.15.23 - 2026-05-05 03:34

- `Fluvial Erosion` に `Deposits` と `Flows` の出力ピンを追加し、2Dビューとプレビューで堆積フィールドと流量フィールドを確認できるようにしました。

## 0.15.22 - 2026-05-05 03:23

- `Fluvial Erosion` のCPU処理で、粒子輸送の削り込みが各レベル開始時の最低高さを下回らないようにしました。

## 0.15.21 - 2026-05-05 03:16

- `Fluvial Erosion` のCPU粒子輸送で、粒子位置の周囲4セルへ侵食、堆積、移動可能な土砂、マスクをbilinearに散布するようにしました。

## 0.15.20 - 2026-05-05 03:08

- `Fluvial Erosion` のCPU処理をKTT寄せの内部フィールド構成へ近づけ、流量、堆積、侵食、移動可能な土砂を粒子輸送中に保持するようにしました。
- `Fluvial Erosion` のCPU粒子輸送で、堆積した土砂を後続の粒子が再輸送できるようにしました。

## 0.15.19 - 2026-05-05 02:54

- 3DプレビューのグリッドをGPUメッシュプレビューの深度バッファで描画し、ハイトフィールドとの前後関係が自然になるようにしました。

## 0.15.18 - 2026-05-05 02:44

- `Fluvial Erosion` の GPU Compute で侵食の変化が弱すぎたため、GPU 側のグリッド侵食反復数と流量ウェイトを調整しました。

## 0.15.17 - 2026-05-05 02:34

- `Fluvial Erosion` の Backend 表記から `GPU Compute (planned)` を外し、GPU が使えない場合は CPU へフォールバックする説明に更新しました。

## 0.15.16 - 2026-05-05 02:24

- 2Dビューが Heightmap メッシュの側面・底面頂点を含む頂点数で判定してしまい、各ノードの結果を表示できなくなっていた問題を修正しました。

## 0.15.15 - 2026-05-05 02:16

- ノードネットワークの右クリック追加メニューに角丸と余白を追加し、項目の間隔を広げました。

## 0.15.14 - 2026-05-05 02:07

- Heightfield 入出力の `Heightmap Blur` ノードを追加しました。
- `Heightmap Blur` に `Radius`、`Strength`、`Iterations` のプロパティを追加し、ハイトマップを滑らかに調整できるようにしました。

## 0.15.13 - 2026-05-05 01:49

- 新規ノード追加時に、既存ノードが初期位置へ戻って重なってしまう問題を修正しました。
- `Fluvial Erosion` の GPU Compute がCPU側のグリッド侵食段階より強くかかりすぎていたため、反復回数と流量重みをCPU側の基準に近づけました。
- GPU Compute のグリッド侵食に下流セルへの堆積を追加し、単純に高さが下がるだけになりにくいようにしました。
- 自動評価と重複していた `ビルド > グラフを評価` メニューを削除しました。

## 0.15.12 - 2026-05-05 01:48

- `Import Heightmap` で 16-bit grayscale、32-bit float grayscale、16-bit color の高さ情報を8bitへ潰さず読み込むようにしました。
- ハイトマップ読み込み結果のメッセージに、読み込んだビット深度・形式を表示するようにしました。

## 0.15.11 - 2026-05-05 01:34

- `Fluvial Erosion` の GPU Compute に D8 近似の flow accumulation パスを追加しました。
- GPU侵食シェーダで flow accumulation の重みを使い、水が集まる筋ほど侵食が強くなるようにしました。
- GPU実行時に flow accumulation 用の追加バッファと compute pipeline state を使うようにしました。

## 0.15.10 - 2026-05-05 01:18

- `GPU Compute` がマルチレベル処理を迂回していた問題を修正しました。
- GPU実行時も `Level Strength` の各レベルごとに計算し、低解像度の大きな浸食から高解像度の細部まで差分を重ねるようにしました。

## 0.15.9 - 2026-05-05 01:05

- `GPU Compute` の本番ハイトフィールド評価をメインスレッドへスケジュールする仕組みを追加しました。
- バックグラウンド評価スレッドはGPUジョブをキューへ積み、メインループがD3D12上で実行して結果を返すようにしました。
- 終了時にGPUジョブ待ちで固まらないよう、シャットダウン待機中もGPUジョブを処理するようにしました。

## 0.15.8 - 2026-05-05 00:52

- `GPU Compute` 選択時に、バックグラウンド評価スレッドから D3D12 の本番GPU評価を実行しない安全柵を追加しました。
- GPU本番評価がメインスレッド実行スケジューラへ移るまで、非同期評価中は `CPU Reference` へフォールバックするようにしました。

## 0.15.7 - 2026-05-05 00:43

- `Fluvial Erosion` の GPU Compute シェーダで、セルサイズ、Wear Angle、Max Erosion Angle、Sediment Capacity、Channeling を使うようにしました。
- GPU 版の侵食量計算を CPU 側のグリッド同期パスへ近づけ、CPU/GPU 差分検証へ進めやすくしました。

## 0.15.6 - 2026-05-05 00:32

- `Fluvial Erosion` の `GPU Compute` を本番のハイトフィールド評価へ接続しました。
- GPU 実行時はハイトフィールドを GPU バッファへ転送し、compute shader を複数回 dispatch して、高さとマスクを CPU 側へ読み戻すようにしました。
- GPU 実行に失敗した場合は `CPU Reference` へフォールバックするようにしました。

## 0.15.5 - 2026-05-05 00:16

- `Fluvial Erosion` の GPU Compute 経路で、小さなハイトフィールドを GPU バッファへ転送し、compute shader を dispatch して読み戻す自己診断を追加しました。
- `GPU Compute` 選択時の状態表示を、シェーダ準備だけでなく dispatch 実行確認まで含む表示に更新しました。

## 0.15.4 - 2026-05-04 23:59

- `Fluvial Erosion` の GPU Compute 実装へ向けて、D3D12 compute shader、root signature、compute pipeline state の初期化を追加しました。
- `GPU Compute` 選択時に compute shader の準備状態をプロパティへ表示するようにしました。
- 現時点では GPU 実行はまだ CPU Reference へフォールバックします。

## 0.15.3 - 2026-05-04 23:46

- `Fluvial Erosion` に GPU Compute へ移行しやすいグリッド同期更新パスを追加しました。
- 各レベルの前半を決定的なグリッドベースの侵食・堆積処理にし、後半の粒子パスを減らして、CPU/GPUで同じ処理構造へ寄せました。

## 0.15.2 - 2026-05-04 23:36

- `Fluvial Erosion` に `Backend` を追加し、将来の `GPU Compute` 実装へ向けた切り替えの土台を作りました。
- 現時点の `GPU Compute` は未実装のため、選択時も `CPU Reference` へフォールバックする旨をUIに表示するようにしました。
- `.terrainproj` の保存・読み込みに `backend` を追加しました。
- `docs/nodes/heightfield/fluvial_erosion/fluvial_erosion_node.md` に CPU/GPU 一致方針を追記しました。

## 0.15.1 - 2026-05-04 23:22

- `Fluvial Erosion` のプロパティを Basic と Advanced に整理し、通常操作では `Large Scale` / `Medium Scale` / `Detail Scale` の3つでスケール別強度を調整できるようにしました。
- `Use Advanced` を追加し、無効時は角度、摩擦、粒度などの詳細パラメータを計算に使わず、安定した内部デフォルトを使うようにしました。
- `.terrainproj` の保存・読み込みに `useAdvancedParameters` を追加しました。

## 0.15.0 - 2026-05-04 23:08

- `Fluvial Erosion` に 6 段階の `Level Strength` を追加し、低解像度から高解像度へスケール別に浸食差分を重ねるマルチレベル処理を導入しました。
- 低い Level は大きな谷筋や流域、高い Level は細かいリルや表面ディテールに効くよう、スケールごとに Feature Size、Channel Length、Channeling、Granularity を調整するようにしました。
- `.terrainproj` の保存・読み込みに `levelStrengths` を追加しました。

## 0.14.5 - 2026-05-04 22:39

- ハイトフィールドメッシュに外周の断面ポリゴンと底面を追加し、前面・背面・側面を持つ地形ブロックとして表示・出力できるようにしました。

## 0.14.4 - 2026-05-04 22:31

- プレビュー評価中のノードに `計算中` / `計算待ち` バッジを表示し、どのノードの計算が走っているかわかりやすいようにしました。

## 0.14.3 - 2026-05-04 22:27

- プレビュー評価をバックグラウンドで実行するようにし、ノード選択やパラメータ変更時にUIが固まりにくいようにしました。
- 評価中に追加の変更が入った場合は古い結果を捨て、最新のグラフ状態で再評価するようにしました。
- ステータスバーに `計算中` / `計算待ち` を色付きで表示し、評価処理の状態がわかりやすいようにしました。

## 0.14.2 - 2026-05-04 22:21

- `PBR Preview` のデフォルト光量、環境光、影の強さ、地形色を調整し、影色が青く浮きすぎないようにしました。
- `PBR Preview` のシェーダで影側の空色成分を抑え、反射光と彩度補正を加えて地形色へ馴染みやすくしました。

## 0.14.1 - 2026-05-04 22:17

- シャドウマップで使うライト定数のメモリ配置を HLSL と一致させ、`Shadow Debug` の範囲判定がずれて影が落ちない問題を修正しました。

## 0.14.0 - 2026-05-04 20:51

- 表示設定に `Lighting Mode` を追加し、`Simple` と `PBR Preview` を切り替えられるようにしました。
- `PBR Preview` 選択時に、太陽方向、太陽光強度、環境光、影の強さ、シャドウマップ解像度、シャドウバイアス、Albedo を調整できるようにしました。
- 太陽方向からのシャドウマップを生成し、地形確認時に落ち影を含む陰影プレビューを表示するようにしました。
- シャドウマップの効き方を確認できる `Shadow Debug` 表示を追加しました。
- Inspector のタブ順を調整し、`表示設定` を `統計` より左に表示するようにしました。
- 3Dビューのマウスホイール操作を表示倍率変更ではなく、カメラ距離を変えるドリー操作に変更しました。
- マスク確認時は従来のシンプルなマスク表示を優先し、地形確認時だけ PBR Preview のライティングを適用するようにしました。

## 0.13.3 - 2026-05-04 20:26

- スクリーンショット保存処理を `src/screenshot_capture.cpp` / `src/screenshot_capture.h` に分離し、`main.cpp` の責務を軽くしました。

## 0.13.2 - 2026-05-04 20:18

- タブ描画の共通処理を整理し、3D/2D ビュー、ノードネットワーク、Inspector のタブスタイルを同じヘルパーで扱うようにしました。

## 0.13.1 - 2026-05-04 19:59

- スクリーンショット保存で、DPI スケーリング時に左上だけが保存される問題を避けるため、DWM のウィンドウ矩形を使って画面から切り出す方式へ変更しました。

## 0.13.0 - 2026-05-04 19:59

- F12 キーでアプリウィンドウ全体を PNG スクリーンショットとして保存できるようにしました。
- スクリーンショットはプロジェクト保存済みの場合はプロジェクトファイルと同じフォルダへ、未保存の場合は `screenshots` フォルダへ保存するようにしました。

## 0.12.1 - 2026-05-04 18:07

- `2Dビュー` でマウスホイールによる拡大縮小、ドラッグによるパン、ダブルクリックによる表示リセットをできるようにしました。
- 2Dビューのズームとパンをアプリ設定へ保存するようにしました。

## 0.12.0 - 2026-05-04 17:40

- 左側のプレビュー領域を `3Dビュー` と `2Dビュー` のタブに分けました。
- `2Dビュー` で、選択中の `Heightmap` または `Fluvial Mask` 出力を 2D マップとして確認できるようにしました。

## 0.11.2 - 2026-05-04 17:27

- 右上のノードエリアをタブバー化し、`ノードネットワーク` をタブとして扱うようにしました。

## 0.11.1 - 2026-05-04 17:16

- Inspector のプロパティで、デフォルト値から変更されている項目のラベルを太字風に表示するようにしました。

## 0.11.0 - 2026-05-04 17:10

- `Import Heightmap` に `Simulation Resolution` を追加し、地形処理に使う内部ハイトフィールド解像度を表示設定から分離しました。
- 表示設定の `Resolution` は計算済みハイトフィールドをメッシュ化する密度だけに使い、表示解像度の変更で侵食計算結果が変わらないようにしました。
- 計算済みハイトフィールドから表示用メッシュをリサンプリングし、マスク値も表示解像度へ補間するようにしました。

## 0.10.0 - 2026-05-04 16:57

- 地形系ノードにノード単位のハイトフィールドキャッシュを追加し、入力と設定が変わっていない `Import Heightmap` と `Fluvial Erosion` の結果を再利用するようにしました。
- 同じノード内の `Heightmap` と `Fluvial Mask` の出力切り替えでは再計算せず、既存の評価結果を使って表示だけ切り替えるようにしました。

## 0.9.2 - 2026-05-04 16:41

- 出力ピン選択時の強調表示を、役割色を変えずに太字風の表示へ変更しました。
- Heightmap 系のピンとリンクを緑、Mask 系のピンとリンクをオレンジに統一しました。

## 0.9.1 - 2026-05-04 16:41

- 選択中の出力ピンラベルを強調表示し、現在プレビューしている出力が分かりやすくなるようにしました。
- グラフ評価にかかった時間を Stats とステータスバーに表示するようにしました。

## 0.9.0 - 2026-05-04 16:35

- `Fluvial Erosion` の出力に `Heightmap` と `Fluvial Mask` の 2 つのピンを用意し、出力ピンをクリックしてプレビュー対象を切り替えられるようにしました。
- `Fluvial Mask` プレビューでは、地形メッシュ上に侵食・堆積マスクを色付きで表示するようにしました。

## 0.8.0 - 2026-05-04 16:07

- Fluvial Erosion に sediment capacity と deposit field を追加し、粒子が削った土砂を保持して谷底や緩斜面へ堆積できるようにしました。
- Fluvial Erosion に `Sediment Capacity (%)` と `Deposition Rate (%)` のプロパティを追加しました。

## 0.7.1 - 2026-05-04 16:07

- `docs/nodes/heightfield/fluvial_erosion/fluvial_erosion_node.md` に Fluvial Erosion ノードの更新履歴と現状アップデートを追記しました。

## 0.7.0 - 2026-05-04 16:03

- Terrain Editor の追加ノードを地形用の `Import Heightmap` と `Fluvial Erosion` に絞り、旧 Rock Generator 由来のノードを追加メニューとプロジェクト読み込み対象から外しました。
- Stats の出力表示を `Output Mesh` から `Terrain Mesh` に変更しました。

## 0.6.0 - 2026-05-04 15:49

- Fluvial Erosion に flow accumulation / drainage area を追加し、流量が多い場所ほど侵食が強くなるようにしました。
- Fluvial Erosion を粗い谷形成パスと細かいチャンネル形成パスのマルチスケール処理へ変更しました。

## 0.5.1 - 2026-05-04 15:49

- Fluvial Erosion ノードの現状プロセスと今後追加すべきアルゴリズムを `docs/nodes/heightfield/fluvial_erosion/fluvial_erosion_node.md` として整理しました。

## 0.5.0 - 2026-05-04 15:32

- ノードプロパティのラベルにマウスオーバーしたとき、項目の意味や調整のヒントを角丸で少し明るいツールチップとして表示するようにしました。
- Fluvial Erosion のプロパティラベルに距離、割合、角度などの単位を表示するようにしました。

## 0.4.4 - 2026-05-04 15:24

- ハイトマップ読み込みノードの表示名を `Import Heightmap` に変更しました。
- Inspector の File 入力欄が横幅いっぱいに伸びないように調整しました。

## 0.4.3 - 2026-05-04 14:04

- プレビューと出力メッシュの解像度上限を 512 に引き上げました。
- ハイトフィールドグリッド生成側の上限も 512 に揃え、256 より上の解像度が実際のメッシュへ反映されるようにしました。

## 0.4.2 - 2026-05-04 14:04

- プレビューと出力メッシュの解像度上限を 256 に引き上げました。

## 0.4.1 - 2026-05-04 14:04

- 地形メッシュの凹凸が読みやすくなるように、プレビューのライティングと陰影を調整しました。

## 0.4.0 - 2026-05-04 14:04

- KTT を参考にしたノード候補一覧を `docs/nodes/node_candidates.md` として追加しました。
- KTT Fluvial Erosion の内容と Terrain Editor 向け実装メモを `docs/nodes/heightfield/fluvial_erosion/fluvial_erosion_hda_notes.md` として追加しました。
- CPU 版の Fluvial Erosion ノードを追加し、Heightmap Load から HeightField として接続して地形メッシュへ反映できるようにしました。
- Fluvial Erosion の主要パラメータを Inspector で編集し、プロジェクト保存/読み込みに対応しました。

## 0.3.3 - 2026-05-04 07:14

- Voxels 表示モードと関連する表示設定を削除し、メッシュ表示のみに整理しました。

## 0.3.2 - 2026-05-04 07:10

- Raymarch プレビュー、GPU SDF プレビュー、関連シェーダーと表示設定を削除しました。
- README を現在のハイトフィールド中心の機能構成に合わせて更新しました。

## 0.3.1 - 2026-05-04 07:01

- ハイトマップ読み込みノードの Scale、Relative Vertical、Offset の編集範囲を地形向けに調整しました。
- Surface と Wireframe を同時に有効にしたときも、地形メッシュの面が表示されるようにしました。

## 0.3.0 - 2026-05-04 06:57

- ハイトフィールドを 1 unit = 1 m として描画するようにしました。
- 地形向けにカメラの初期距離と操作範囲を広げ、グリッドを 100 m 間隔の 20 x 20 表示に変更しました。
- 新規プロジェクトの初期状態をノードなしにしました。

## 0.2.3 - 2026-05-04 06:52

- ビューポート中央に表示していたワイヤー立方体のプレースホルダーを削除しました。

## 0.2.2 - 2026-05-04 06:52

- ハイトフィールドの高さを、スケールに Relative Vertical の値を 100 で割った倍率を掛けた値として扱うようにしました。

## 0.2.1 - 2026-05-04 06:42

- 変更履歴のバージョン見出しに時刻を分単位まで記録する運用へ変更しました。

## 0.2.0 - 2026-05-04 06:42

- ハイトマップ画像を読み込み、地形メッシュとしてビューポートに表示できるようにしました。
- Slice 表示と関連する表示設定を削除しました。

## 0.1.0 - 2026-05-04 06:42

- Terrain Editor としてプロジェクトを開始しました。
- ハイトマップ読み込みノードを追加し、ファイルパス、スケール、相対垂直スケール、垂直オフセットを設定できるようにしました。
