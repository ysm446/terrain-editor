# UI スタイルガイド

作成日時: 2026-08-11 18:34
更新日時: 2026-08-31 12:19

UI を一貫したアートスタイルに保つためのルール。
**UI（見た目・レイアウト・配色・部品）を変更するときは、必ずこのドキュメントに従う。**
ここに書かれていない見た目を新しく足したくなった場合は、まず「既存の部品で表現できないか」を検討し、
どうしても必要なときは実装と同時にこのドキュメントを更新する。

関連ドキュメント: [ショートカット一覧](../ui/shortcuts.md) / [スクリーンショット](../ui/screenshot.md) / [AGENTS.md](../../AGENTS.md) の「UI 実装」節。

---

## 0. 基本原則

1. **既存のヘルパーを使う。** プロパティ 1 行を描くのに `ImGui::SliderFloat` を直接呼ばない。
   [PropertyWidgets.h](../../src/ui/PropertyWidgets.h) の `DrawProperty*Row` を使う。
   ヘルパーがラベル列・入力幅・既定値ボタン・ツールチップ・Undo・変更通知をまとめて面倒を見る。
2. **幅を親いっぱいに伸ばさない。** ボタン・プルダウン・入力欄は内容に合う固定幅、
   または既存 UI と揃う幅を使う（AGENTS.md「UI 実装」）。`SetNextItemWidth` を必ず指定する。
3. **色は直書きしない。** テーマから引く（[4. 配色とテーマ](#4-配色とテーマ)）。
   直書きが許されるのは、テーマに依存しない意味色（評価状態のパルス、ステータスの状態色など）だけ。
4. **ユーザーに見える文字列は必ず `Tr()` を通す。** 内部 ID・JSON キー・ログは英語固定。
5. **数値には必ず既定値がある。** 既定値は設定構造体のデフォルト（`rock::XxxSettings{}.field`）を渡し、
   ハードコードした数値リテラルを既定値として渡さない。

---

## 1. 言語とローカライズ

- 切り替えは [Localization.h](../../src/ui/Localization.h) の `Tr(english, japanese)`。既定は日本語。
  `設定 > 言語` で切り替わり、`data/app_settings.json` に保存される。
- **翻訳するもの**: メニュー、タブ名、セクション見出し、ボタン、ツールチップ、説明文、状態メッセージ。
- **翻訳しないもの**:
  - **ノードのパラメータ名**（`Scale (m)`、`SPE Strength`、`Thermal Strength` など）。
    アルゴリズムや論文の用語に対応する識別子なので英語のまま。実際
    [NodeProperties.cpp](../../src/ui/NodeProperties.cpp) の行ラベルは 100% 英語で、
    説明はすべてツールチップ側に日本語を用意している。
  - ノード種別名（`Multi-Scale Erosion` など）、`Heightmap` / `Mask` / `Path` のような制作ツールの慣用語。
  - ウィジェット ID（`"MseSpeStrength"` など）、`MarkGraphChanged` に渡す変更理由、デバッグログ。
- 一方、**設定パネル側のラベルは翻訳してよい**（`Tr("Terrain Boundary", "地形境界")` など）。
  ノードのパラメータではなくアプリの機能名だから。
- 翻訳するラベルを ImGui のウィンドウ／タブ／ヘッダーに使うときは、**言語を切り替えても状態が飛ばないよう
  `###` で安定 ID を付ける**。`StableImGuiLabel(label, stableId)` を使う。

```cpp
const std::string header = StableImGuiLabel(Tr("Sun and Shadows", "太陽と影"), "SkySunAndShadowsHeader");
if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) { ... }
```

- 日本語 UI では専門用語を無理に和訳しない。文章（ツールチップ・説明）は自然な日本語にする。
- 英語 UI に日本語が混ざらないよう、追加した範囲は英語表示でも一度確認する。

### フォント

[AppFonts.cpp](../../src/ui/AppFonts.cpp) が Meiryo → Yu Gothic Medium → MS Gothic の順で
**18.0px** の日本語グリフ付きフォントを読み込む。フォントサイズは変えない。
大きさを変えたいときは `ImGui::SetWindowFontScale()` を局所的に使い、**必ず 1.0 に戻す**。
既定のスケール値は次のとおり（勝手に増やさない）。

| 用途 | スケール |
| --- | --- |
| タブ見出し | 1.08 |
| ノードのタイトル | 1.10 |
| 既定値リセットのアイコン | 1.25（`GetFontSize()` 比） |

---

## 2. 画面レイアウト

全体は 1 枚のフルスクリーン ImGui ウィンドウ `Terrain Editor Shell`（装飾なし・メニューバーのみ）で、
[main.cpp](../../src/main.cpp) の `DrawUi()` が次の構造を組む。ドッキングは使わない。

```
+--------------------------------------------------------------+
| メニューバー  ファイル / 編集 / 表示 / 設定                   |
+-------------------------------+------------------------------+
| Left Work Column              | Right Work Column            |
|  3D / 2D ビュータブ           |  ノードネットワーク           |
|  --- 水平スプリッター ---     |  --- 水平スプリッター ---     |
|  デバッグログ（任意表示）      |  インスペクター（タブ群）      |
+-------------------------------+------------------------------+
| ステータスバー                                                |
+--------------------------------------------------------------+
        ↑ 垂直スプリッター（MainLayoutSplitter）
```

- ペインの最小幅・最小高さは必ず `std::clamp` で守る（各ペイン 160〜180px 以上）。
  ウィンドウが小さくてレイアウトが成立しないときは、**サイズを保存しない**（`mainLayoutCanFit` の判定）。
- ペインサイズはスプリッターを離したタイミングで `data/app_settings.json` へ保存する。
- ステータスバー高さは `GetTextLineHeight() + 16.0f`。固定ピクセルで書かない。
- 新しい常設パネルは**インスペクターのタブとして追加する**のが既定。フローティングウィンドウは
  `表示` メニューから開く補助的なもの（ヘルプ、デバッグログ）だけに留める。

### インスペクターのタブ

`プロパティ / 設定 / 天球 / 雲 / 水面 / カメラ / エクスポート / 環境設定` の順。
タブは `PushTabHeaderStyle()` → `BeginStyledTabItem(Tr(...), "安定ID")` →
`BeginScrollableInspectorTabContent(...)` の型を守る。中身は必ずスクロール可能にする。

`TabHeaderStyle` の既定値: `FramePadding = (12, 5)`, `ItemInnerSpacing = (5, 5)`, `fontScale = 1.08`。

### メニューバー

`DrawUi()` 内で以下を push してから描く。個別のメニューで別の値を積まない。

| StyleVar | 値 |
| --- | --- |
| WindowPadding | (12, 8) |
| FramePadding | (12, 9) |
| ItemSpacing | (12, 8) |
| ItemInnerSpacing | (10, 7) |
| SelectableTextAlign | (0.0, 0.5) |

メニュー項目にショートカットがある場合は `ImGui::MenuItem(Tr(...), "Ctrl+S")` の第 2 引数で表示し、
[docs/ui/shortcuts.md](../ui/shortcuts.md) とアプリ内ヘルプ（`kHelp*Entries`）にも同時に追記する。

### スプリッター

`DrawVerticalSplitter` / `DrawHorizontalSplitter` を使う。仕様は共通。

- 線は **1px**、当たり判定は **9px**（見た目より掴みやすくする）。
- 色: 通常はテーマ色 `border`、ドラッグ中は `accent`。
- ホバー／ドラッグ中はカーソルを `ResizeEW` / `ResizeNS` にする。
- ドラッグ開始時に ID を `g_activeLayoutSplitterId` に持ち、他のペインへ吸い付かないようにする。

---

## 3. プロパティ行（最重要）

すべての設定値は **2 列テーブルの 1 行** として描く。左がラベル、右がウィジェット。

```cpp
if (!ImGui::BeginTable("MaskNoiseRows", 2, ImGuiTableFlags_SizingStretchProp))
{
    return false;
}
ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 200.0f);
ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
// ... DrawProperty*Row を並べる ...
ImGui::EndTable();
```

- テーブル ID はノード名 + `Rows`（`"MseRows"` など）。フラグは `ImGuiTableFlags_SizingStretchProp` 固定。
- **ラベル列の幅**は用途ごとに決まっている。新しい値を勝手に足さない。

| 場所 | ラベル列幅 |
| --- | --- |
| 設定系パネル（設定 / 天球 / 雲 / 水面 / カメラ） | 112.0 |
| ノードプロパティ（短いラベル: Heightmap Load, Shape, Blur） | 184.0 |
| ノードプロパティ（標準） | 200.0 |
| ノードプロパティ（長いラベル: 侵食系・Mask Fluvial など） | 210.0 |

- ウィジェットを描く前に、値を必ず `std::clamp` で有効範囲へ丸める（プロジェクト読み込み直後の
  範囲外値を UI 側で吸収するため）。スライダーに渡す min/max と同じ値を使う。

### 行ヘルパーの使い分け

| ヘルパー | 用途 |
| --- | --- |
| `DrawPropertyFloatRow` | 実数。スライダー + 数値入力 + リセット。既定。対数スケールは `ImGuiSliderFlags_Logarithmic` |
| `DrawPropertyFloatInputRow` | 実数だがスライダーが無意味な値（座標など）。入力 + リセット |
| `DrawPropertyPercentRow` | 内部 0〜1 の値を 0〜100% で見せる。ラベルは `Xxx (%)` |
| `DrawPropertyIntRow` | 整数。スライダー + 入力 + リセット |
| `DrawPresetIntRow` | 解像度など離散値のみ許す整数。コンボで最近傍プリセットに丸める |
| `DrawPropertyBoolRow` | チェックボックス。`compact` で FramePadding を (4, 2) に詰められる |
| `DrawPropertyComboRow` | 列挙。`"A\0B\0\0"` 形式 |
| `DrawPropertyPathRow` | ファイルパス。入力欄 + `参照` ボタン（ダイアログ） |
| `DrawColorRgbRow` | RGB カラー |
| `DrawReadOnlyFloatRow` | 表示専用の値 |
| `DrawTimeOfDayRow` | 時刻（`h:mm` 表記） |
| `DrawCameraFloatRow` | カメラ用。グラフを dirty にせず Undo も積まない実数行 |

### ウィジェット幅（固定値。変えない）

| 部品 | 幅 |
| --- | --- |
| スライダー | 残り幅から入力欄とリセットボタンを引いた値を **80〜180** にクランプ |
| 実数入力欄 | 76.0 |
| 整数入力欄 | 58.0 |
| コンボ | `min(220.0, 残り幅)` |
| プリセットコンボ | 110.0 |
| パス入力欄 | 残り幅から参照ボタンを引いて **120〜260** にクランプ |
| `参照` ボタン | 82.0 |
| カラーピッカー | 356.0 |
| リセットボタン | `GetFrameHeight()` の正方形 |

### 既定値に戻すボタン

数値・色の行には必ず `DrawResetToDefaultButton` を右端に置く。

- アイコンは **`↺`**（フォントサイズ 1.25 倍で中央に描画）。
- 現在値が既定値と一致していれば `ImGuiCol_TextDisabled`、違えば `ImGuiCol_Text` で描く。
  **これが「既定値から変更されているか」の唯一の視覚表現**であり、ラベル側の色は変えない
  （`DrawPropertyLabel` の第 3 引数は現状使われていない）。
- ツールチップに既定値そのものを出す:「既定値に戻す / 既定値: 0.500」。
- 押されたら `PushUndoSnapshot()` → 値を代入 → `MarkGraphChanged(reason)`。

### ツールチップ

- ラベルに付ける。`DrawProperty*Row` の `tooltip` 引数へ `Tr(english, japanese)` を渡すだけ。
- 表示は `ImGuiHoveredFlags_DelayShort`（即座には出さない）。
- 折り返し幅は `GetFontSize() * 24.0f`。角丸 6.0、背景 `(0.22, 0.22, 0.22, 0.97)`。
- **内容の書き方**: 「そのパラメータが何をするか」+「大きくすると／小さくするとどうなるか」。
  1〜3 文。単位・標準値・他パラメータとの関係があれば書く。計算コストが増えるなら明記する。
  改行で段落を分けてよい（フォールバック挙動の説明など）。

### セクション見出し

パラメータが多いノード／パネルはグループに分ける。ノードプロパティでは
**テーブルの行として左に薄いラベル、右に区切り線**を出すのが型。

```cpp
const auto sectionHeader = [](const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SeparatorText(label);
};
```

設定系パネルではテーブル内で `ImGui::SeparatorText(Tr(...))` を直接呼ぶ。
パネル全体は `ImGui::CollapsingHeader(安定ラベル, ImGuiTreeNodeFlags_DefaultOpen)` で包み、
`BeginChild` の幅は `GetContentRegionAvail().x - 10.0f`（ヘッダー右の余白）。

### 変更通知と Undo

行ヘルパーが内部でやってくれるので**自分で書くのは呼び出し側の後始末だけ**。

- ヘルパーが `true`（編集確定）を返したら `EvaluateGraph()` を呼ぶ。
- `MarkGraphChanged(reason)` の `reason` は **英語の短文**で、値ごとに固有にする。
  例: `"Multi-scale erosion SPE strength changed"`, `"Clouds toggled"`。デバッグログに出る。
- Undo は「スライダーを掴んだ瞬間に `BeginPropertyUndoEdit()`、離したら `CommitPropertyUndoEdit()`」、
  離散的な変更（リセットボタン、コンボ選択、参照ダイアログ）は `PushUndoSnapshot()`。
  ドラッグ 1 回で Undo が 1 段になるようにする。
- Undo を積みたくない行（表示専用設定・カメラ）は `recordUndo = false` を渡す。

---

## 4. 配色とテーマ

テーマは `assets/ui_themes/*.json`。既定は `road_editor_dark`（表示名 `Default Dark`）。
`設定 > UIテーマ` で切り替わる。実装は [UiTheme.cpp](../../src/ui/UiTheme.cpp)。

- JSON の `colors` のうち、ImGui の色名（`WindowBg`, `Button`, ...）は `ImGuiStyle` に流し込まれる。
  **それ以外のキーはアプリ独自色**として `namedColors` に入り、`g_themeManager.AppColor(name, fallback)`
  で取得する。
- **`AppColor` を呼ぶときは必ずフォールバック色を渡す。** テーマがその色を定義していなくても壊れないこと。
- 新しい独自色を足すときは `assets/ui_themes/default_dark.json` に必ず追加する
  （他テーマは差分だけなので任意）。

### アプリ独自色

| キー | 用途 |
| --- | --- |
| `accent` | アクセント。トーストの枠線、ドラッグ中のスプリッター |
| `accentText` | ビューポートオーバーレイの文字色 |
| `mutedText` | 補助テキスト |
| `border` | 区切り線、ステータスバー上端、非アクティブなスプリッター |
| `viewportBg` / `viewportGrid` | 3D ビューの背景とグリッド |
| `surfaceFill` / `surfaceWire` / `surfacePoint` | 地形サーフェスの描画色 |
| `nodeEditorBg` / `nodeGridDot` | ノードエディターの背景とドット |
| `panelHeader` | パネル見出し |
| `modeButtonActive` / `modeButtonActiveHovered` / `modeButtonIdle` / `modeButtonIdleHovered` | モード切り替えボタン |

### 意味を持つ色（テーマに依存しない）

これらは動作状態を表すのでテーマで変えず、直書きのままにする。

| 状態 | 色 |
| --- | --- |
| 評価中 / 実行中 | オレンジ `(0.90, 0.72, 0.34)` |
| 未評価 (Dirty) | オレンジ `(0.90, 0.64, 0.30)` |
| 評価済み | グリーン `(0.54, 0.78, 0.58)` |
| 待機を促す警告 | 黄 `(1.00, 0.85, 0.20)` |
| 収集中などの進行 | 緑 `(0.40, 1.00, 0.50)` |

---

## 5. ノードエディター

[main.cpp](../../src/main.cpp) の `DrawRockNode()` が唯一のノード描画パス。ノードごとに分岐させない。

### 形状（固定値）

| 項目 | 値 |
| --- | --- |
| ノード幅 | 250.0 |
| 角丸 | 8.0 |
| パディング | (12, 10, 12, 10) |
| 枠線 | 通常 1.0 / 選択時 1.8 |
| 背景色 | `(0.080, 0.080, 0.080, 0.98)` |
| 枠色 | 通常 `(0.22, 0.22, 0.22)` / ホバー・選択時 `(0.24, 0.72, 0.92)` |
| ピン行の高さ | 24.0 |
| ピンの丸 | 半径 4.3、線幅 1.6、当たり判定 14x20 |
| グリッドドット間隔 | 24.0（ズームアウト時は自動で間引く） |

ノードの影は `DrawRockNodeShadows()` が 4 層の矩形（spread 10/7/4/2、alpha 3/5/7/10）で描く。

### 色分け

**ノードのアイコン色 = カテゴリ色**（`NodeAccentColor`）:

| カテゴリ | 色 |
| --- | --- |
| Heightfield | 緑 `(0.42, 0.70, 0.50)` |
| Mask | オレンジ `(0.92, 0.56, 0.24)` |
| Color | 青紫 `(0.44, 0.50, 0.96)` |
| Path | シアン `(0.42, 0.78, 0.92)` |

**ピンの色 = データ型**（[NodePins.cpp](../../src/ui/NodePins.cpp) の `PinTypeColor`）:

| 型 | 色 |
| --- | --- |
| HeightField | `(0.70, 0.93, 0.78)` |
| Mask | `(0.82, 0.64, 0.36)` |
| ColorTexture | `(0.54, 0.60, 1.00)` |
| Path | `(0.42, 0.78, 0.92)` |
| Mesh / その他 | `(0.52, 0.58, 0.56)` |

リンクの色は接続元ピンの型色を使う。ピンのラベルは通常 `(0.62, 0.64, 0.62)`、
ホバーで `(0.94, 0.94, 0.92)`、プレビュー選択中はピンの型色（さらに 0.7px ずらした二重描画で太く見せる）。

### 評価状態の表示

`DrawNodeEvaluationPulse()` がノード枠の外側に脈動するリングを描く。

- **Processing**（ワーカースレッドが今そのノードを計算中）: 明るい黄緑、速い脈動（5.2 rad/s）、太さ 2.2。
- **Pending**（別の評価の後ろで待機中）: 落ち着いた緑、遅い脈動（2.7 rad/s）、太さ 1.6。
- ズームに追従させるため、線幅・角丸・オフセットはすべて `screenScale` 倍する。

### 新しいノードを足すとき

1. `NodeCategory` を適切に設定する（アイコン色とメニューの分類が自動で決まる）。
2. `追加` メニューの該当カテゴリ（ハイトフィールド / マスク / カラー / パス）に `addNodeMenuItem` を足す。
3. `InitialNodePosition()` に初期配置を追加する（既存ノードと重ならない位置）。
4. `NodeProperties.h/.cpp` に `DrawXxxProperties` を足し、`DrawNodePropertiesPanel` の `switch` に繋ぐ。
5. [docs/nodes/README.md](../nodes/README.md) に per-node ドキュメントを追加する。

---

## 6. ビューポートとオーバーレイ

- ビューポート上に浮かせる UI は**左上から 14px, 12px** の位置に置く。ボタンサイズは 54x28。
- オーバーレイのボタンは背景を半透明の暗色にしてビューを隠さない:
  通常 `IM_COL32(8, 10, 10, 176)` / ホバー `(32, 38, 36, 220)` / 押下 `(54, 70, 62, 235)`、
  文字色はテーマ色 `accentText`。
- ポップアップは `WindowPadding (12, 10)`, `ItemSpacing (7, 8)`, `FramePadding (10, 6)`。
  トグル項目は `Selectable` + 自前描画のチェックボックス（13px 角、チェック色 `(91, 177, 232)`）で、
  `ImGuiSelectableFlags_NoAutoClosePopups` を付けて連続操作できるようにする。
- ビューポート系のショートカットは、**マウスがそのビューの上にあるときだけ**効かせる。
  単独キーのショートカットは `io.WantTextInput` が真のときは無効にする。

---

## 7. 通知とステータス

### トースト

右下に積み上がる一時通知（[main.cpp](../../src/main.cpp) の `DrawToastNotifications`）。

- 位置は右下から margin 18px、間隔 8px。**最大 4 件**（超えたら古いものから捨てる）。
- 既定の表示時間は約 6 秒。残り 0.45 秒でフェードアウト。
- 角丸 8.0、パディング (16, 12)、枠線はテーマ色 `accent`、背景は `PopupBg`。
- ホバー中は消えない（寿命を 1.5 秒延長する）。左クリックで関連フォルダーを開き、右クリックで即消す。
- 本文は 1 行目にタイトル、以降は `TextDisabled` で詳細。
- **成功・完了の通知に使う**。エラーやユーザーの確認が要るものはトーストにしない。
- スクリーンショット撮影フレームでは描画しない（写り込み防止）。

### ステータスバー

`状態 | プレビュー段階 | 評価時間 | プロジェクト状態 [| エクスポート状態]` を 1 行で表示する。
先頭の状態語（`Processing` / `Pending` / `Dirty` / `Evaluated`）だけを状態色で塗り、残りは通常色。

### デバッグログ

`表示 > デバッグログ` で左カラム下部に開く。開発者向け情報はトーストではなくここへ出す。

---

## 8. 新しい UI を足すときのチェックリスト

- [ ] 既存の `DrawProperty*Row` / `BeginStyledTabItem` / スプリッターで表現できないか検討したか
- [ ] ウィジェット幅を明示し、親いっぱいに伸ばしていないか
- [ ] ラベル列幅は表の値のどれかに一致しているか
- [ ] ユーザーに見える文字列を `Tr()` に通したか。ノードのパラメータ名は英語のままか
- [ ] 翻訳されるヘッダー／タブに `###安定ID` を付けたか
- [ ] 数値行に既定値（`Settings{}.field`）とツールチップ、リセットボタンがあるか
- [ ] 値を描画前に `std::clamp` したか
- [ ] `MarkGraphChanged` の理由文字列が固有か。編集確定時に `EvaluateGraph()` を呼んでいるか
- [ ] Undo がドラッグ 1 回で 1 段になるか
- [ ] 色をテーマから引いたか。独自色なら `default_dark.json` に追加したか
- [ ] ショートカットを足したなら [shortcuts.md](../ui/shortcuts.md) と `表示 > ヘルプ` の両方を更新したか
- [ ] 英語 UI / 日本語 UI の両方で表示を確認したか
- [ ] ウィンドウを最小サイズまで縮めてもレイアウトが壊れないか
- [ ] `docs/changelog.md` の `## 未リリース` に日本語で追記し、必要ならバージョンを上げたか
- [ ] このドキュメントに書かれていない見た目を足したなら、ここも更新したか
