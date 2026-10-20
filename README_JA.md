[中文](README_zh-CN.md) | [English](README.md) | **[日本語](README_JA.md)**

# jietuba — Windows 向けスクリーンショット・OCR・ピン留め・翻訳・クリップボードツール

[![build](https://img.shields.io/github/actions/workflow/status/1003129155/jietuba/cpp.yml?branch=master2&label=build&style=flat-square)](https://github.com/1003129155/jietuba/actions/workflows/cpp.yml) [![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) [![release](https://img.shields.io/github/v/release/1003129155/jietuba?style=flat-square)](https://github.com/1003129155/jietuba/releases/latest) ![platform](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D4?style=flat-square)

[Windows 版をダウンロード](https://github.com/1003129155/jietuba/releases/latest) · [ソースからビルド](#source-setup) · [開発とテスト](#development)

<img width="1391" height="844" alt="21" src="https://github.com/user-attachments/assets/5eb5e82a-7a5f-4c60-a877-679d1b29cf0c" />


## 概要

jietuba は Windows 向けの無料・オープンソースのスクリーンショットツールです。領域/ウィンドウキャプチャ、スクロール（長い）スクリーンショット、注釈、OCR 文字認識、翻訳、画像のピン留め、GIF/MP4 録画、画面への直接注釈、QR コード/バーコード読み取り、PDF エクスポート、そして完全なクリップボード履歴管理を備えています。すべてローカルで動作します。

MP4 領域録画はフレームレートとビットレートの指定、一時停止/再開、マウスポインター、システム音声に対応しています。一般的な 5.1/7.1 再生デバイスの音声はステレオに変換します。マイク録音は含まれません。

画面注釈では、操作中の画面に直接書き込めます。専用のグローバルホットキー（設定 → ショートカット → 画面注釈、既定は未設定）またはトレイメニューの「画面注釈」から開始します。ペン、蛍光ペン、矢印、番号、矩形、楕円、テキスト、消しゴムは、キャプチャ編集と同じツール・同じ設定です。ツールを選ぶまでマウスは下のウィンドウをそのまま操作でき、Esc または「透過」ボタンで透過に戻ると、元のウィンドウにフォーカスが戻ります（スライドショーのページ送りもそのまま使えます）。画面ごとにレイヤーを持ち、元に戻すは描いた順に画面をまたいで働きます。描画は非表示にしたり、まとめて消去（元に戻せます）したりできます。スクリーンショットと GIF/MP4 録画には描画が入り、注釈ツールバーは入りません。画面上の蛍光ペンは一定の半透明で色を付けるので、下の文字はそのまま読めます。ワンクリックで切り替わるホワイトボード、ポインターを光らせクリックごとに波紋を出すカーソルハイライトも使えます。設定 → 画面注釈 で、開始時の状態、ツールバーの位置、終了時に描画を残すか、カーソルハイライトの大きさとクリックの波紋を選べます。

jietuba は C++（Qt6）で実装され、画像処理・クリップボード操作・PP-OCR エンジンなどのコアは Rust で実装しています。Windows x86_64 および ARM64 に対応。以前の Python（PySide6）実装は [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) ブランチにアーカイブ済みで、今後は更新しません。

すぐに使える Windows 版の配布パッケージと、ソースからのビルド方法を用意しています。

---

## 主な特長

「スクリーンショット ⇄ クリップボード ⇄ ピン留め」がなめらかにつながる三方向連携。

- **コピー元を問わず自動で履歴に**：自分で撮ったスクリーンショットだけでなく、コピーしたテキスト・画像・ファイル・HTMLはすべて同じクリップボード履歴に自動で入ります。グループ分けして永久保存でき、ワンクリックでエクスポート、複数デバイス間で共有できます

- **履歴 → ピン留め → 作業を継続**：履歴内のどの画像もワンクリックで画面最前面にピン留めできます。いつでも見比べたり、拡大縮小したり、注釈を続けたり、再OCRもできます

- **細部まで磨き込まれた機能**：スクロールキャプチャは上下左右全方向をミリ秒単位で正確に結合。マルチモニター・マルチDPI環境でもシームレスにキャプチャし、さまざまなエンコードの画像をエクスポートできます。各機能は有料ソフトに匹敵するか、それ以上です

- **メモリ使用量**：厳格なメモリ設計により、連続した複雑な操作でも常駐メモリを非常に低いレベルに抑えられます（多くの場合、わずか十数MB、場合によっては数MBレベル）

- **快適なUI**：負荷の高いシーンを最適化し、CPU使用率も低減。低スペックPCでも快適に動作します

- **データの安全性**：すべての機能が端末上だけで完結し、データ収集も裏でのネット通信もありません。データはすべてローカルに保存されます。唯一ネット接続が必要なのは翻訳機能で、これもユーザーが自分で選んだ翻訳APIを能動的に使ったときだけ通信します

---

## ダウンロードと起動

Windows x86_64 版および ARM64 版の配布パッケージは、そのまま実行できます。ランタイムや開発環境のインストールは不要です。

1. [Releases ページ](https://github.com/1003129155/jietuba/releases/latest)を開き、端末に合わせて `-x64.zip` または `-arm64.zip` で終わるファイルをダウンロードします。
2. パッケージは 2 種類あります。ZIP 全体を展開し、中の exe をダブルクリックして起動します。
   - **完全版** `jietuba_pp-…zip`：PP-OCR エンジンとモデルを同梱し、どの Windows でも OCR が使えます。`jietuba_pp.exe` と `models/` フォルダを同じ階層に置いてください。
   - **軽量版** `jietuba_lite-…zip`：PP-OCR を含まない単一 exe で、サイズが小さくなります。既定では Windows 11 の Snipping Tool に内蔵された OCR だけを使います。Snipping Tool がない PC や、11.2308 より古く「テキスト アクション」がない PC では OCR を使えません。Snipping Tool を更新するか、フル版をお使いください。exe の横に PP-OCR コンポーネントを置くと PP-OCR も使えます。[PP-OCR コンポーネント](#ppocr-pack)を参照してください。
3. どちらも既定では Snipping Tool の OCR（高速で対応言語が多い）を優先します。エンジンは設定の「OCR設定」で切り替えられます。
4. アプリにはデジタル署名がないため、ブラウザーからダウンロードすると Windows の警告が表示される場合があります。表示された場合は「詳細情報」をクリックし、「実行」を選択すると起動できます。

2 つの版は設定とクリップボード履歴を共有するので切り替えて使えますが、同時に起動しないでください（既定のホットキーが競合します）。

更新機能を含む最初のリリースをインストールした後、**バージョン情報 → 更新を確認 → 更新して再起動**で更新できます。対応する GitHub Release ZIP をダウンロードし、メイン EXE のみを置換して再起動します。設定、クリップボード履歴、モデルなどのファイルは保持されます。録画、書き出し、保存を終了してからインストールしてください。直前の EXE は `.jietuba-update/` に保存されます。[復元手順](rust_libs/updater/README.md)を参照してください。2.1.8 より前の Python 版はワンクリック更新で C++ 版に置き換わり、設定・クリップボード履歴・モデルはそのまま保持されます。

<a id="ppocr-pack"></a>

### PP-OCR コンポーネント（軽量版）

軽量版には PP-OCR の前処理・後処理は含まれますが、ONNX Runtime とモデルは含まれません。もっとも簡単なのは、設定の「PP-OCR モデルの管理」からダウンロードする方法です。ONNX Runtime は PyPI で配布されている Microsoft の `onnxruntime` wheel から取得し、ダウンロード後にハッシュを検証します。モデルは PP-OCRv6、PP-OCRv5、各種言語別の認識モデルから選べ、ローカルの ONNX ファイルを追加することもできます。ダウンロード、追加、切り替えに再起動は不要です。

手動で exe と同じ階層の `models/` フォルダーに置くこともできます。`PP-OCRv6_det_small.onnx` と `PP-OCRv6_rec_small.onnx`（リポジトリの [models/](models/) にあります）、および ONNX Runtime 1.24 以上の CPU 専用 `onnxruntime.dll` です。

## コード署名ポリシー

Windows 版リリースに含まれる `jietuba_pp.exe` と `jietuba_lite.exe` は、どちらも GitHub Actions がこのリポジトリの [cpp/](cpp/) の C++ ソースから自動ビルドし、SignPath で署名します。

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

- コミッター・レビュアー・承認者：[@1003129155](https://github.com/1003129155)
- 外部からの貢献は Pull Request で受け付け、メンテナーのレビュー後にのみマージします。

### プライバシー

ユーザーが明示的に要求しない限り、本プログラムはネットワーク上の他のシステムに情報を送信しません。

- スクリーンショット、OCR、クリップボード履歴はすべてこの PC 上で処理・保存されます。
- 翻訳を使うと、翻訳対象のテキストが設定で選んだ翻訳サービス（DeepL、Google、Azure、Amazon、Baidu、Volcengine、Tencent Cloud、Youdao、DeepSeek、または自分で設定した OpenAI 互換エンドポイント）に、ご自身の API キーで送信されます。そのサービスのプライバシーポリシーが適用されます。
- **バージョン情報 → 更新を確認**で更新を確認・インストールすると、リリース情報の取得とパッケージのダウンロードのために GitHub に接続します。

---

<a id="source-setup"></a>

## ソースからビルド

Visual Studio の「C++ によるデスクトップ開発」（CMake を含む）、Rust MSVC ツールチェーン、Qt 6.11 の MSVC SDK が必要です。`cpp/` で PowerShell から実行します。

```powershell
python -m venv .tools
.\.tools\Scripts\python.exe -m pip install py7zr==1.1.4
.\.tools\Scripts\python.exe scripts\install_qt.py   # Qt SDK を .sdk/ にダウンロード
.\build.cmd                                         # ビルド・テストし dist/Release に配置
```

Qt 6 の MSVC x64 SDK を持っている場合は `.\scripts\build.ps1 -QtRoot C:\Qt\6.11.0\msvc2022_64` を使います。`-SkipTests` でテストを省略、`-Configuration Debug` で Debug ビルドです。ビルド後は `run.cmd` で起動できます。

リリース用の単一ファイル版は Qt と MSVC ランタイムを静的リンクし、システム DLL だけに依存します。まず `.\scripts\build_static_qt.ps1` で Qt の公式ソースから静的 SDK をビルドし、続けて次を実行します。

```powershell
.\build.cmd -Static          # 軽量版：dist/Release-static/jietuba_lite.exe
.\build.cmd -Static -Full    # フル版：dist/Release-static-full/jietuba_pp.exe（ONNX Runtime を内蔵し、既定の PP-OCR モデルを同梱）
```

ARM64 版は x64 上でクロスコンパイルします。先に x64 の静的 Qt をビルドし、`.\scripts\build_static_qt.ps1 -Arch arm64` と `.\build.cmd -Static -Arch arm64` を実行します。Visual Studio の MSVC ARM64 ビルドツールと `rustup target add aarch64-pc-windows-msvc` が必要です。バージョン番号はリポジトリ直下の `VERSION` ファイルから取得します。

PP-OCR モデルはリポジトリの [models/](models/) にあります（`PP-OCRv6_det_small.onnx` と `PP-OCRv6_rec_small.onnx`）。Snipping Tool OCR はローカルの Windows 11 Snipping Tool のファイルを使い、初回使用時に `%LOCALAPPDATA%\Jietuba\oneocr` へ自動コピーするため、追加のダウンロードは不要です。

---

<a id="development"></a>

## 開発とテスト

`.\build.cmd` はビルドしてすべてのテストを実行します。名前が `_desktop` で終わるテストは実際のデスクトップでキーボードとマウスの入力を注入し、グローバルホットキーを登録し、画面をキャプチャするため、実行中はマウスとキーボードに触れないでください。デスクトップを必要としないテストだけを実行するには次のようにします。

```powershell
$env:PATH = "$PWD\.sdk\6.11.0\msvc2022_64\bin;$env:PATH"
ctest --test-dir build\msvc -C Release --output-on-failure -E "_desktop$|_windows_parity$"
```

テストプログラムは一時的なサンドボックスで動作し、お使いの設定、クリップボード履歴、ログを読み書きしません。ほかに分析ビルド（`scripts\check.ps1`：AddressSanitizer、コード分析、リーク検出）とカバレッジ（`scripts\coverage.ps1`）があります。

[C++ CI](.github/workflows/cpp.yml) はすべてのプルリクエストでビルドしてテストを実行し、変更したコードの行カバレッジを確認します。`rust_libs/` を変更したときは [Rust CI](.github/workflows/rust.yml) が Rust の単体テストとサードパーティライセンスの再確認を行います。

リリースは [build.yml](.github/workflows/build.yml) で行います。タグは `release-<VERSION ファイルのバージョン>` でなければなりません。[lite.yml](.github/workflows/lite.yml) が x64 と ARM64 の軽量版・フル版をビルドし、署名後にそれぞれ `jietuba_lite-<tag>-<arch>.zip` と `jietuba_pp-<tag>-<arch>.zip` を作成します。

---

## ディレクトリ構造

```text
# プロジェクトルート
├── README.md / README_zh-CN.md / README_JA.md   # 英語・中国語・日本語のドキュメント
├── VERSION                  # バージョン番号（CMake とリリースワークフローが参照）
├── 托盘.ico                 # アプリと更新機能のアイコン
├── licenses/                # 配布パッケージに同梱するサードパーティライセンス
│
├── cpp/                     # C++（Qt6）実装
│   ├── src/                 # ソース（<パッケージ>/<モジュール> 構成）
│   │   ├── annotation/      # 画面注釈 — 画面ごとのレイヤー、透過の切り替え、画面をまたぐ元に戻す
│   │   ├── barcode/         # QR コード・バーコード認識（zxing-cpp）
│   │   ├── canvas/          # キャンバス — 編集の中核、元に戻す、レイヤー
│   │   ├── capture/         # キャプチャ — スクリーンショット、ウィンドウ・要素の検出
│   │   ├── clipboard/       # クリップボード管理 — 履歴、グループ/クイック起動、インポート/エクスポート、検索
│   │   ├── core/            # コア — 起動、ログ、リソース、テーマ、国際化、ホットキー
│   │   ├── gif/             # GIF/MP4 録画、GIF 編集、再生、書き出し
│   │   ├── ocr/             # Snipping Tool OCR と PP-OCR
│   │   ├── pin/             # ピン留め — 最前面表示、編集、OCR、翻訳
│   │   ├── settings/        # 設定の一元管理
│   │   ├── stitch/          # 長いスクリーンショットの結合
│   │   ├── text_recognition/ # OCR 結果のレイアウト
│   │   ├── tools/           # 描画ツール — ペン、矩形、矢印、テキスト、モザイクなど
│   │   ├── translation/     # 複数プロバイダーの翻訳と読み上げ
│   │   ├── ui/              # UI コンポーネント、キャプチャ画面、ツールバー、設定画面
│   │   └── video/           # MP4 録画子プロセス
│   ├── native/              # rust_libs の C インターフェース（静的リンク）
│   ├── resources/           # 既定設定、UI 翻訳（XML と .qm）、ログ文言
│   ├── scripts/             # ビルド、Qt SDK の準備、翻訳のコンパイル
│   └── tests/               # QtTest のテストと照合データ
│
├── rust_libs/               # Rust ライブラリ（cpp/native 経由で C++ に静的リンク）
│   ├── gifrecorder/         # GIF/動画合成エンコーダー
│   ├── longstitch/          # 長いスクリーンショット結合アルゴリズム
│   ├── pyclipboard/         # クリップボード操作
│   ├── ppocr_rust/          # PP-OCR (PaddleOCR) ONNX 認識エンジン
│   ├── hdrcapture/          # HDR デスクトップキャプチャ
│   ├── updater/             # メイン EXE だけを置き換える独立した更新機能
│   └── video_recorder/      # Media Foundation 録画と WASAPI システム音声の取得
│
├── models/                  # PP-OCR ONNX モデル（フル版に同梱）
│   ├── PP-OCRv6_det_small.onnx   # 文字検出モデル (DBNet)
│   └── PP-OCRv6_rec_small.onnx   # 文字認識モデル (CRNN/CTC)
│
└── svg/                     # SVG アイコン
```

## モジュール詳細

### annotation/ — 画面注釈モジュール

操作中の画面に直接書き込みます。画面ごとに全画面の透明レイヤーを置き、GIF 録画の描画レイヤー（`GifDrawingView`）とキャプチャのツールバー（`Toolbar`、レイアウト `toolbar_layout::ANNOTATION`）を流用するため、ツール・設定パネル・元に戻すは同じものです。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
annotation/
├── screen_annotation.h/.cpp   # ScreenAnnotation — セッション：画面ごとのレイヤー、透過/描画/非表示、画面をまたぐ元に戻す、フォーカスの返却；ScreenAnnotationShortcutHandler — 描画中のキー
├── annotation_view.h/.cpp     # AnnotationView — 1 画面分の描画レイヤー。キャンバスのホストとして共有ツールバーを CanvasView に渡す
├── annotation_toolbar.h/.cpp  # AnnotationToolbar — 透過・消去・非表示・終了を備えたフローティングツールバー。フォーカスを奪わず、キャプチャと録画に写らない
└── cursor_highlight.h/.cpp    # CursorHighlight と PointerLight — ポインターの周りの光とクリックの波紋
```

</details>

**主な機能：**
- 透過中のレイヤーは `WS_EX_TRANSPARENT` を持ち、マウスは下のウィンドウに届きます。ツールを選ぶとマウスを受け取り、透過に戻るとき元の前面ウィンドウにフォーカスを返します
- 画面ごとにキャンバスと元に戻すスタックを持ち、セッションは各操作がどの画面を変えたかを記録するため、元に戻す/やり直しは描いた順に画面をまたいで働きます。消去は 1 回で元に戻せる操作です
- 画面の抜き差しや解像度の変更に合わせてレイヤーを追加・削除・調整し、ツールバーは常に見える画面に残ります
- キャプチャの前に透過へ戻り、そのフレームが画面に出るのを待ちます。キャプチャには描画が入り、ヒット判定用のレイヤーは入りません。ツールバーは `WDA_EXCLUDEFROMCAPTURE` でキャプチャと録画から除外します
- レイヤーには描画の下にキャプチャ画像がないため、蛍光ペンは下の内容に乗算できません。代わりに不透明度 35% で重ね、一本の線は一度に描いて色むらを出さず、矩形の蛍光は縁と内側を一つの形で塗ります
- 描いたばかりの図形は選択しないので、画面や録画にハンドルが映りません。図形はそのツールでクリックすれば選択できます
- ホワイトボードは描画の下に敷く白一色の面で、クリックごとにオンとオフが切り替わります。透過に戻るとデスクトップが見え、次のツールでボードが戻ります
- カーソルハイライトは入力フックのマウスイベントからポインターとボタンを受け取るので、透過中も働き、動かない間は何も動かず、クリックも取りこぼしません。描画中は円盤が線と同じ再描画で動き、スクリーンショットには入りません

---

### barcode/ — バーコードモジュール

スクリーンショットの選択範囲にある QR コードやバーコードを読み取ります。結果ウィンドウでは、左側のスクリーンショット上に各コードの位置を示し、右側に番号順で内容を一覧表示します。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
barcode/
├── reader.h/.cpp         # read_codes / DecodedCode — zxing-cpp でデコードし、内容・形式・輪郭を読み順で返す
├── result_window.h/.cpp  # BarcodeResultWindow — 結果ウィンドウ。スクリーンショットと結果一覧を同じ番号で対応付け
└── barcode.h             # show_barcode_result — スキャンの入口：選択範囲のコードを解読して結果ウィンドウを開く（ツールバーのスキャンボタン）
```

</details>

- QR コード、Data Matrix、Aztec、PDF417 と、Code 128・EAN/UPC などの一般的な一次元バーコードに対応
- 選択範囲内の複数のコードを一度に読み取り。どちら側でコードにホバーしても両方がハイライト
- ワンクリックでコピー。http(s) リンクはブラウザで直接開ける

---

### canvas/ — キャンバスモジュール

シーン管理、ビューレンダリング、アイテム選択、アンドゥ/リドゥ機能を備えたグラフィックス編集キャンバスシステム。
![jietuba_gif_20260404_000903](https://github.com/user-attachments/assets/5318b991-b0de-46a2-9c0e-d75eeae2a827)

<details>
<summary>ディレクトリ構造を表示</summary>

```text
canvas/
├── scene.h/.cpp                  # CanvasScene — QGraphicsScene継承のキャンバスシーン
├── view.h/.cpp                   # CanvasView — QGraphicsView継承のキャンバスビュー
├── selection_model.h/.cpp        # SelectionModel — 選択グラフィックスアイテムの管理
├── undo.h/.cpp                   # CommandUndoStack — アンドゥ/リドゥスタック
├── smart_edit_controller.h/.cpp  # SmartEditController — 選択/編集モード切替
├── smart_selection_anim.h/.cpp   # SmartSelectionAnimator — ウィンドウ切り替え時の選択範囲の補間
├── handle_editor.h/.cpp          # LayerEditor / EditHandle — コントロールポイントドラッグ編集
├── gestures.h/.cpp               # マウスジェスチャの状態機械 — 文字の端ドラッグ、ラバーバンド選択、保留中のクリック編集
├── handle_overlay.h/.cpp         # HandleOverlayWidget — 編集ハンドル専用の合成レイヤー、シーン全体の再描画を回避
├── canvas_host.h/.cpp            # CanvasHost — キャンバスビューをホストするウィンドウがビューに提供する機能（ツールバー、拡大鏡など）。スクリーンショット窓とピン窓で共用
└── items/
    ├── drawing_items.h/.cpp      # RectItem / EllipseItem / NumberItem と、描画アイテム共通の DrawingItemMixin
    ├── stroke_item.h/.cpp        # StrokeItem — ペン/蛍光ペンのストローク、変わった部分・露出した部分のブロックだけ再描画
    ├── background_item.h/.cpp    # BackgroundItem — 選択領域の背景
    ├── mosaic_item.h/.cpp        # MosaicItem — モザイクアイテム
    ├── spotlight_item.h/.cpp     # SpotlightItem / SpotlightCurtain — スポットライトの穴と共有の幕
    ├── selection_item.h/.cpp     # SelectionItem — 選択境界表示
    ├── arrow_item.h/.cpp         # ArrowItem — 矢印アイテム、9 種類の軸・先端・輪郭のジオメトリ
    ├── text_item.h/.cpp          # TextItem — テキストアイテム、縁取り/影/背景と三状態の操作枠
    ├── watermark_item.h/.cpp     # WatermarkItem / WatermarkStyle — 選択範囲に重ねる透かし：タイル、ひし形グリッド、または 7 か所の位置、複数行と時刻に対応
    └── item_reference.h          # ItemReference / ItemLease — アイテムの所有権と寿命：シーンが追加済みのアイテムを、アンドゥコマンドが取り外したアイテムを保持
```

</details>

**主な機能:**
- Qt Graphics View Framework ベースのキャンバスシステム
- フリーハンド、矩形、楕円、矢印、テキスト、番号などの図形アイテムに対応
- 完全なアンドゥ/リドゥ（QUndoStack ベース）
- スマート編集コントローラーで選択モードと描画モードをシームレスに切り替え
- コントロールポイントエディターで図形の変形が可能

---

### capture/ — キャプチャモジュール

スクリーンキャプチャとスマートウィンドウ検出。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
capture/
├── capture_service.h/.cpp           # CaptureService — スクリーンショットコアロジック
├── display_watcher.h/.cpp           # DisplayChangeWatcher — ディスプレイの接続・解像度・HDR が変わったらキャプチャセッションを裏で再構築
├── system_cursor.h/.cpp             # SystemCursor — キャプチャ時のマウスポインターを記録し画像に描き込む
├── quick_capture_controller.h/.cpp  # QuickCaptureController — 修飾キー＋ドラッグ撮影とアクション実行
├── window_finder.h/.cpp             # WindowFinder — スマートウィンドウ選択、カーソル位置検出
└── msaa_element_finder.h/.cpp       # MSAAElementFinder — ポイント検出：バックグラウンドスレッドが MSAA でポインター下のコントロールと親チェーンを取得し、スマート選択に使う
```

</details>

**主な機能:**
- 全画面・領域キャプチャ
- スマートウィンドウ検出と選択（ウィンドウの影を自動で除外）
- カーソル位置のウィンドウ検出
- ポインター下のコントロールをポイント単位で検出（MSAA）。スマート選択は階層ごとに吸着し、最初のフレームから選択範囲を表示

---

### clipboard/ — クリップボード管理モジュール

クリップボード履歴マネージャーで、controllers・core・services・ui の4層構成です。
テキスト、画像、HTML、ファイルに対応し、グループと内容を管理する独立した3ペイン管理ウィンドウも備えています。
![jietuba_gif_20260404_001128](https://github.com/user-attachments/assets/b0a116e8-d944-43c9-b895-e6fc10d8c08a)

<details>
<summary>ディレクトリ構造を表示</summary>

```text
clipboard/
├── controllers/                        # 制御層 — 履歴読み込み、貼り付け処理、メニュー、選択状態
│   ├── clipboard_controller.h/.cpp     # ClipboardController — 読み込み、貼り付け、コンテキストメニュー
│   ├── selection_manager.h/.cpp        # SelectionManager — リスト選択状態管理
│   ├── context_menu_controller.h/.cpp  # ClipboardContextMenuController — コンテキストメニューのデータと動作の組み立て
│   ├── foreground_tracker.h/.cpp       # ForegroundWindowTracker — 貼り付け先ウィンドウを記憶
│   └── paste_keystroke.h/.cpp          # 対象ウィンドウにフォーカスを戻してから Ctrl+V を送信
├── core/                               # データ層 — pyclipboard ラッパー、モデル、グループ種別
│   ├── manager.h/.cpp                  # ClipboardManager — 保存、監視、貼り付け API
│   ├── models.h/.cpp                   # ClipboardItem / Group データモデル
│   └── text_transform.h/.cpp           # プレーンテキスト変換 — 「特殊貼り付け」用の副作用のない純関数群
├── services/                           # サービス層 — file payload、グループ規則、入出力、保存ロジック
│   ├── file_payload_service.h/.cpp     # file 型 JSON payload と旧形式互換
│   ├── group_service.h/.cpp            # グループのアイコン・命名・削除確認補助
│   ├── import_export_service.h/.cpp    # テキスト項目の CSV インポート/エクスポート
│   └── manage_dialog_service.h/.cpp    # 管理ウィンドウの保存ロジック
└── ui/
    ├── layout_scale.h/.cpp             # 管理ウィンドウ共通のサイズ定数
    ├── image_item_actions.h/.cpp       # 画像項目の読み込みと保存、両ウィンドウで共用
    ├── group_kind_style.h/.cpp         # グループ種別の識別色と線画アイコン。サイドバーの行と種別カードで共用
    ├── dialogs/
    │   └── manage_dialog.h/.cpp        # グループ・内容・入出力を扱う3ペイン管理ウィンドウ
    ├── forms/
    │   ├── group_form.h/.cpp
    │   ├── text_content_form.h/.cpp
    │   ├── file_content_form.h/.cpp
    │   ├── import_export_form.h/.cpp
    │   ├── group_icon_picker.h/.cpp
    │   ├── form_widgets.h/.cpp
    │   └── image_content_form.h/.cpp
    ├── menus/
    │   └── action_menu.h/.cpp
    ├── mixins/
    │   └── frameless_mixin.h/.cpp
    ├── panels/
    │   └── setting_panel.h/.cpp
    ├── resources/
    │   └── emoji_data.h/.cpp
    ├── theme/
    │   ├── themes.h/.cpp
    │   ├── theme_styles.h/.cpp
    │   └── chevron.h/.cpp              # 色とサイズごとに描画してキャッシュするドロップダウン矢印画像（スタイルシート用）
    ├── widgets/
    │   ├── group_bar.h/.cpp
    │   ├── item_delegate.h/.cpp
    │   ├── preview_popup.h/.cpp
    │   ├── quick_edit_popup.h/.cpp
    │   ├── manage_rows.h/.cpp
    │   └── reorder_list.h/.cpp
    └── windows/
        ├── clipboard_window.h/.cpp     # 履歴ウィンドウ、検索、プレビュー、高速貼り付け
        └── pin_window.h/.cpp           # 履歴項目からピンを作成
```

</details>

**主な機能:**
- システムクリップボードの変更を監視し、自動で履歴を保存
- テキスト、画像、HTML、ファイル等に対応
- 通常グループ、クイック起動グループ、フィルターグループ（特定の種類の履歴を常に表示）、お気に入り、検索に対応
- 独立した3ペイン管理ウィンドウでグループ・テキスト内容・ファイル内容を編集可能
- テキスト項目の CSV インポート/エクスポートに対応
- 複数のテーマ（ライト/ダークなど）に対応
- ショートカットで履歴をすばやく貼り付け
- 大きな画像や長文のプレビューポップアップに対応
- 既定で Win+V が Windows のクリップボード履歴の代わりにこのクリップボードを開く（クリップボードのホットキー 3 枠目。空にすると Windows に戻る）

---

### core/ — コアモジュール

ログ、リソースローディング、テーマ管理、国際化、ホットキー等のインフラ。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
core/
├── bootstrap.h/.cpp            # PreloadManager — 起動ブートストラップ、環境初期化、DPI、シングルインスタンス
├── elevation.h/.cpp            # 管理者として実行 — 権限昇格の状態判定、スタートアップ用タスク
├── logger.h/.cpp               # Logger — ファイル＋コンソールログ（debug/info/warning/error/exception）
├── crash_handler.h/.cpp        # install_crash_hooks() — グローバル例外キャッチ
├── resource_manager.h/.cpp     # ResourceManager — SVG/画像リソースローディング
├── theme.h/.cpp                # ThemeManager — アプリテーマカラー管理
├── ui_scale.h/.cpp             # UIScaleManager — ツールバー/パネル/ポップアップ共通の拡大率
├── i18n.h/.cpp                 # I18nManager / XmlTranslator / tr() — 国際化
├── shortcut_manager.h/.cpp     # HotkeySystem / ShortcutManager — グローバル＆アプリ内ホットキー
├── global_hotkey.h/.cpp        # グローバルホットキーのキー名表と登録方法の判定：システム登録・フック引き継ぎ・不可
├── input_hub.h/.cpp            # プロセス共通のグローバル入力：フックは GUI スレッド外で動き、イベントを Qt シグナルで通知
├── input_engine.h/.cpp         # 入力ステートマシン — ジェスチャー、ホットキー、ショートカット記録、監視
├── input_hooks.h/.cpp          # 専用スレッド上の低レベルマウス・キーボードフックと前面ウィンドウフック
├── quick_capture_input.h/.cpp  # グローバルマウスショートカットのドラッグ入力（共通入力ハブを利用）
├── last_capture_region.h/.cpp  # 「前回の選択範囲を復元」用のプロセス内メモリ
├── save.h/.cpp                 # SaveService — ファイル保存サービス（高品質 PDF 出力対応）
├── export.h/.cpp               # ExportService — 画像エクスポート
├── clipboard_utils.h/.cpp      # copy_image_to_clipboard() — 画像をクリップボードにコピー
├── platform_utils.h/.cpp       # DPI設定、AppUserModelID、Windows APIユーティリティ
├── qt_utils.h/.cpp             # safe_disconnect() — Qtシグナル安全切断
├── constants.h/.cpp            # グローバル定数（フォント、パス等）
├── background_tasks.h/.cpp     # バックグラウンド保存の追跡
├── update_cache.h/.cpp         # 更新プログラムの一時ファイルとキャッシュのクリーンアップ
├── updater_process.h/.cpp      # Rust 更新プログラムとの非同期 JSONL 通信
├── update_controller.h/.cpp    # 更新ダウンロードと安全な再起動の調整
├── update_checker.h/.cpp       # GitHub 最新リリースの非同期取得とバージョン比較
├── ui_theme.h/.cpp             # UIThemeManager — アプリ窓と Qt ネイティブ部品のライト/ダーク外観
├── east_asian_width.h/.cpp     # is_east_asian_wide — コードポイントが全角・幅広（East Asian Width W/F）かどうか
├── east_asian_width.inc        # East Asian Width テーブル（スクリプトで生成）
├── hooks.h                     # HookList — オブジェクトの拡張ポイントに付けるコールバック。登録順に呼び出し、ID で個別に削除可能
├── html_entities.inc           # HTML 名前付き文字参照テーブル（スクリプトで生成）
├── html_unescape.h/.cpp        # html_unescape — HTML の名前付き・数値文字参照をデコード
├── http_client.h/.cpp          # http_request — システムの WinHTTP とプロキシ設定を使う同期通信。HTTP エラー・タイムアウト・ネットワーク障害を区別し、キャンセル可能
├── native.h                    # Rust ライブラリへの C インターフェース（cpp/native）の宣言
├── py_case_tables.inc          # 大文字小文字の変換テーブル（スクリプトで生成）
├── py_text.h/.cpp              # UTF-16 単位ではなくコードポイントで数える文字列関数：長さ、切り出し、空白判定、strip
└── serial_executor.h           # SerialExecutor — 専用スレッドで投入順に 1 つずつタスクを実行
```

</details>

**主な機能:**
- ファイルとコンソールに出力する統一ログシステム
- 未処理の例外を自動で捕捉するグローバルクラッシュ処理
- SVG・画像リソースの統一ロード
- アプリ全体のライト/ダークテーマ管理
- XML 形式の多言語国際化システム（中・英・日・韓）
- グローバルホットキー（Windows API）とアプリ内ショートカットの管理
- ファイル保存・エクスポートサービス（高品質 PDF 出力を含む）
- 管理者として実行：タスク スケジューラのタスクで UAC なしにサインイン時に起動し、通常起動は管理者インスタンスに引き継ぐ
- システム HTTP リクエスト（WinHTTP、システムのプロキシを使用）と Rust ライブラリの C インターフェース宣言

---

### gif/ — GIF録画モジュール

画面録画、編集、再生、GIF/動画エクスポート。

録画ツールバーで **MP4** を選択すると、H.264 動画を直接保存できます。1–60 fps、0.5–50 Mbps の目標ビットレート、システム音声とマウスポインターを設定できます。利用可能ならハードウェアを優先し、Windows のソフトウェアエンコーダーにフォールバックします。録画は独立したワーカープロセス（同じ exe を `--video-recorder-worker` 付きで起動）が担当し、フレームを逐次保存します。GUI 側のプロセスには動画フレームを蓄積しません。停止後は MP4 の処理完了を待ちます。MP4 は GIF の編集・再生に進まず直接保存されます。映像は SDR で出力され（HDR ディスプレイでは DXGI で取得して SDR に変換するため、HDR 動画にはなりません）、システム音声は既定の再生デバイス（最大 8 チャンネル、ステレオにダウンミックス）から取得し、マイクは含みません。

Media Foundation と H.264/AAC が必要で、Windows N では Media Feature Pack が必要な場合があります。FFmpeg や追加コーデックは同梱しません。
<img width="766" height="630" alt="image" src="https://github.com/user-attachments/assets/8653fffb-b419-4584-ab4b-9fe95bb9f246" />

<details>
<summary>ディレクトリ構造を表示</summary>

```text
gif/
├── record_window.h/.cpp        # GifRecordWindow / AppState — ステートマシンコーディネーター（3層ウィンドウ）
├── overlay.h/.cpp              # CaptureOverlay / OverlayMode — キャプチャオーバーレイ、領域調整
├── drawing_view.h/.cpp         # GifDrawingView / GifDrawingScene — 録画中描画
├── record_toolbar.h/.cpp       # RecordToolbar — 開始/一時停止/停止コントロール
├── frame_recorder.h/.cpp       # FrameRecorder / FrameData / CursorSnapshot — フレームサンプリング
├── video_recorder.h/.cpp       # VideoRecorder — MP4 録画ワーカーの有界 JSONL 制御
├── video_settings.h/.cpp       # RecordingOptions — 動画のフレームレート・ビットレート・音声設定
├── playback_engine.h/.cpp      # PlaybackEngine / PlayState — フレーム再生＆プレビュー
├── playback_controller.h/.cpp  # PlaybackController — 再生UI＆エクスポート管理
├── playback_toolbar.h/.cpp     # PlaybackToolbar / RangeSlider — プログレスバー、速度調整
├── composer.h/.cpp             # ComposeWorker / ComposerProgressDialog — GIF/動画合成
├── cursor_overlay.h/.cpp       # CursorOverlay — カーソルレンダリング＆クリックアニメーション
├── _widgets.h/.cpp             # ClickMenuButton / svg_icon() — カスタムウィジェット
└── gifrecorder.h/.cpp          # ネイティブ gifrecorder の C++ ハンドル — フレームストア、フレームデコーダー、録画セッション、カーソルスプライト
```

</details>

**主な機能:**
- 状態機械で録画フローを管理（範囲選択 → 録画 → 再生 → エクスポート）
- 3 層ウィンドウ構成：オーバーレイ（範囲選択）、描画レイヤー（注釈）、ツールバーレイヤー
- フレームのサンプリング録画（カーソル取得とクリックアニメーションに対応）
- 再生プレビュー（範囲トリミングと速度調整に対応）
- GIF または動画へエクスポート（Rust ライブラリ gifrecorder を使用）

---

### ocr/ — OCRモジュール

文字認識管理。Windows の Snipping Tool に内蔵された OCR と PP-OCR の 2 つのエンジンに対応。

<img width="580" height="505" alt="image" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />

<details>
<summary>ディレクトリ構造を表示</summary>

```text
ocr/
├── ocr_manager.h/.cpp        # OCRManager — エンジン選択・読み込み・解放、統一認識インターフェース
├── ppocr_models.h/.cpp       # PP-OCR モデル：models/ フォルダー、内蔵カタログ、追加、削除、アップデーター経由のダウンロード
├── snipping_tool_ocr.h/.cpp  # SnippingToolOcr — Snipping Tool の検出、OCR ファイルのコピー、余白追加と認識
└── bundled_runtime.h/.cpp    # bundled_runtime — 完全版 exe に内蔵した onnxruntime.dll。起動時に models/ にない、または使えない場合に書き出す
```

</details>

- エンジンとモデルの利用可否を自動で検出
- 設定で「自動 / Windows Snipping Tool / PP-OCR」を選択。自動では Snipping Tool があればそれを、なければ PP-OCR を使う
- Snipping Tool OCR：Windows 11 の Snipping Tool に付属の認識コンポーネントを直接呼び出す。高速で対応言語が多い（韓国語、ロシア語、タイ語など）。Windows 11 の Snipping Tool が必要
- PP-OCR：ppocr_rust エンジン（純 Rust + ONNX Runtime、PP-OCR det + rec）。どの Windows でも動作。完全版はランタイムとモデルを同梱し、軽量版は設定からダウンロード
- PP-OCR モデル：完全版は PP-OCRv6 small を同梱。設定の「PP-OCR モデルの管理」から内蔵カタログの他のモデル（PP-OCRv6 tiny/medium、PP-OCRv5、韓国語・ラテン文字・キリル文字・タイ語・ギリシャ語の認識）をダウンロードでき、ファイルのハッシュは固定。ローカルの ONNX モデルも試験認識に通れば追加できる。モデルはプログラム横の `models/` に置く。選んだモデルがないときは別のモデルに切り替えず、モデル不足として報告する
- 認識と解放は排他。エンジン切り替え時に旧エンジンを解放し、新エンジンは次回の認識時に読み込む
- シングルトンパターン、統一認識インターフェース

---

### pin/ — ピンモジュール

スクリーンショットを画面にピン留め。編集、ズーム、OCR、翻訳対応。

<img width="737" height="657" alt="image" src="https://github.com/user-attachments/assets/827b912c-11ac-4692-b3f6-826561957615" />

<details>
<summary>ディレクトリ構造を表示</summary>

```text
pin/
├── pin_window.h/.cpp             # PinWindow — ドラッグ可能、ズーム可能、常に最前面の画像ウィンドウ
├── pin_canvas_view.h/.cpp        # PinCanvasView — ピンキャンバスビュー
├── pin_canvas.h/.cpp             # ピンキャンバスオブジェクト
├── pin_manager.h/.cpp            # PinManager — 全ピンウィンドウ管理（シングルトン）
├── pin_toolbar.h/.cpp            # PinToolbar — ピンツールバー
├── pin_controls.h/.cpp           # PinControlButtons — 閉じる、編集、コピーボタン
├── pin_hover.h/.cpp              # PinHoverControls — ホバーボタンとツールバーの表示判定
├── pin_context_menu.h/.cpp       # PinContextMenu — 右クリックメニュー
├── pin_border_overlay.h/.cpp     # PinBorderOverlay — ボーダーエフェクトオーバーレイ
├── pin_click_through_bar.h/.cpp  # PinClickThroughBar — マウス透過中の移動・不透明度・終了バー
├── pin_ocr_manager.h/.cpp        # PinOCRManager / PinOcrThread — 非同期OCR認識
├── pin_shortcut.h/.cpp           # PinShortcutController — 通常/編集モードショートカット
├── pin_thumbnail.h/.cpp          # PinThumbnailMode — サムネイルモード
├── pin_translation.h/.cpp        # PinTranslationHelper — 翻訳ヘルパー
├── pin_image_transform.h/.cpp    # PinImageTransform — 回転、反転等
└── ocr_text_layer.h/.cpp         # OCRTextLayer / OCRTextItem — OCRテキストレイヤー表示
```

</details>

**主な機能:**
- スクリーンショットを最前面にピン留めし、ドラッグ移動とホイールでの拡大縮小に対応
- ピン上で直接描画・編集
- OCR を統合し、選択できる文字レイヤーを表示
- 翻訳を統合し、ピン内の文字を直接翻訳
- 通常モードと編集モードのショートカットに対応

---

### settings/ — 設定モジュール

統一された設定管理システム。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
settings/
├── color_formats.h/.cpp      # 拡大鏡のカラー形式テンプレート：描画・読み込み・保存
├── settings_transfer.h/.cpp  # 設定画面の項目（ツールバーの並びを含む）を JSON ファイルへエクスポート／インポート
├── special_paste.h/.cpp      # 特殊貼り付けメニューの既定項目・順序・有効状態
├── tool_settings.h/.cpp      # ToolSettingsManager / ToolSettings — ツールの色、サイズ、ホットキー設定
└── settings_accessors.inc    # QSettings の 1 行アクセサー（手書き。追加時は末尾の 3 つの検索関数にも登録）
```

</details>

**主な機能:**
- シングルトンの設定マネージャー
- 各描画ツールの色、線幅、フォントサイズなどのパラメーターを管理
- QSettings に永続化

---

### stitch/ — 長いスクリーンショット結合モジュール
![jietuba_gif_20260404_001930](https://github.com/user-attachments/assets/a9720f08-5128-447d-b425-6d0640272e6a)

<details>
<summary>ディレクトリ構造を表示</summary>

```text
stitch/
├── auto_scroll.h/.cpp                  # AutoScroller — 自動スクロール：結合結果から歩幅を決め、末尾かマウス移動で停止
├── change_watch.h/.cpp                 # ChangeWatch — キャプチャ範囲をバックグラウンドで監視し、画面が変わるたびにキャプチャ
├── incremental.h/.cpp                  # IncrementalStitcher — バックグラウンド結合とプレビュー縮小画像
├── jietuba_long_stitch_unified.h/.cpp  # 結合インターフェース（Rust の longstitch を呼び出す）
├── scroll_window.h/.cpp                # ScrollCaptureWindow — スクロールキャプチャウィンドウ
└── scroll_toolbar.h/.cpp               # スクロールキャプチャツールバー
```

</details>

**主な機能:**
- ページをスクロールしてキャプチャ（横方向・縦方向に対応）
- 戻りスクロールでは位置合わせだけを行い結果を短くしない。開始位置を越えると反対側に継ぎ足すため、ページ途中から始めても両方向に伸ばせる
- 自動スクロール：カーソルをキャプチャ領域の中央に置くと自動でスクロールし、最後に達するかマウスを動かすと停止
- トリミング：切りたい位置まで戻りスクロールし、ワンクリックで現在の画面より上または下を削除
- ファイル一覧や表のように数文字しか違わない行でも位置を合わせられる。一度に大きくスクロールしすぎて結合できないときは、無理に結合せず戻るよう促す
- 画像マッチングに基づくスマート結合アルゴリズム（重なり領域を検出）
- 統一された長いスクリーンショットのインターフェース（Rust ライブラリ longstitch で高速化）

---

### text_recognition/ — テキスト認識モジュール

スクリーンショットの選択範囲の文字を認識して表示だけを行います（翻訳はしません）。認識はバックグラウンドスレッドで行い、結果ウィンドウは最初に「認識中…」と表示し、完了後に編集可能なテキストボックスへ文字を出して、ワンクリックでコピーできます。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
text_recognition/
├── layout.h/.cpp         # layout — 認識した行をテキストに整形：折り返し行の結合、CJK 文字間の空白除去、句読点の全角・半角変換
├── placement.h/.cpp      # placement — 結果ウィンドウの配置位置（位置の計算だけで、ウィンドウには触れない）
├── recognizer.h/.cpp     # RecognizeThread — ワーカースレッドで 1 枚の画像を認識して結果を報告
├── result_window.h/.cpp  # TextRecognitionWindow — 認識結果ウィンドウ：編集可能なテキストボックス、コピーボタン、整形メニュー
└── text_recognition.h    # recognize_async / show_text_recognition — 入口：バックグラウンド認識、結果ウィンドウの表示、結果のコピー
```

</details>

- バックグラウンド認識で UI を止めない。認識は中断できないネイティブ呼び出しで、キャンセルは結果を報告しなくするだけ
- 整形：折り返し行の結合、CJK 文字間の空白除去、句読点の全角・半角変換（ウィンドウ内で切り替え可能）
- 結果ウィンドウは認識領域の近くに、カーソルを避けて配置
- テキストは編集でき、ワンクリックでコピー

---

### tools/ — 描画ツールモジュール

各種描画ツールの実装。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
tools/
├── base.h/.cpp            # Tool / ToolContext — 抽象基底クラス
├── controller.h/.cpp      # ToolController — ツール切替＆状態管理
├── action.h/.cpp          # ActionTools — コピー、保存、キャンセルアクション
├── pen.h/.cpp             # PenTool — フリーハンド描画
├── rect.h/.cpp            # RectTool — 矩形（塗りつぶし/アウトライン）
├── ellipse.h/.cpp         # EllipseTool — 楕円
├── arrow.h/.cpp           # ArrowTool — 矢印
├── text.h/.cpp            # TextTool — テキスト
├── number.h/.cpp          # NumberTool — 自動インクリメント番号
├── highlighter.h/.cpp     # HighlighterTool — 蛍光ペン
├── mosaic.h/.cpp          # MosaicTool — モザイク
├── spotlight.h/.cpp       # SpotlightTool — スポットライト（枠の外を暗くする）
├── watermark.h/.cpp       # WatermarkTool — 透かし（追加・文字とスタイルの変更・前回の有無の記録）
├── cursor.h               # CursorTool — カーソル/選択
├── eraser.h/.cpp          # EraserTool — 消しゴム
└── cursor_manager.h/.cpp  # CursorManager — カーソルスタイル管理
```

</details>

**主な機能:**
- 統一されたツール基底クラス構成（Tool → 各ツール）
- 描画ツール：ペン、矩形、楕円、矢印、テキスト、番号、蛍光ペン、モザイク、透かし、カーソル、消しゴムなど
- ツールコントローラーがツールの切り替えとマウスイベントの振り分けを担当
- ツールコンテキスト（ToolContext）がシーン、ビュー、設定などの依存を注入

---

### translation/ — 翻訳モジュール

DeepL、Google、Azure、Amazon、Baidu、Volcengine、Tencent Cloud、Youdao、DeepSeek、および自分で設定した OpenAI 互換エンドポイントに対応したマルチプロバイダー翻訳サービス。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
translation/
├── provider.h/.cpp                      # TranslationProvider / ProviderMetadata — プロバイダー抽象契約
├── registry.h/.cpp                      # ProviderRegistry — プロバイダー登録とファクトリ
├── service.h/.cpp                       # TranslationService — プロバイダー選択と翻訳オーケストレーション
├── models.h/.cpp                        # TranslationRequest / TranslationResult — プロバイダー非依存モデル
├── worker.h/.cpp                        # TranslationWorker — 全プロバイダー共通の Qt ワーカー
├── providers/                           # 各翻訳プロバイダー実装
│   ├── deepl.h/.cpp                     # DeepL
│   ├── google.h/.cpp                    # Google
│   ├── azure.h/.cpp                     # Azure
│   ├── amazon.h/.cpp                    # Amazon
│   ├── baidu.h/.cpp                     # Baidu
│   ├── tencent.h/.cpp                   # Tencent Cloud
│   ├── volcengine.h/.cpp                # Volcengine（火山引擎）
│   ├── youdao.h/.cpp                    # Youdao（有道）
│   ├── custom_llm.h/.cpp                # カスタム OpenAI 互換サービス（Ollama、LM Studio など）
│   ├── deepseek.h/.cpp                  # DeepSeek (LLM)
│   ├── openai_compatible.h/.cpp         # OpenAI 互換インターフェースの基底クラス
│   └── common.h                         # プロバイダー共通のレスポンス解析とエラー本文の取得
├── smart_translation_controller.h/.cpp  # SmartTranslationController — ワンキー選択テキスト検出＆ポップアップルーティング
├── translation_popup.h/.cpp             # TranslationPopup — コンパクト翻訳ポップアップ（選択テキスト/手入力）
├── deepl_service.h/.cpp                 # DeepLService / DeepLResult — 旧版 DeepL API 非同期翻訳
├── languages.h                          # translation_languages / source_languages — 対応言語リスト・言語コード
├── translation_manager.h/.cpp           # TranslationManager — 翻訳ウィンドウマネージャー（シングルトン）
├── translation_dialog.h/.cpp            # TranslationDialog — 翻訳結果ウィンドウ
├── speech_service.h/.cpp                # SpeechService / SapiBackend — 翻訳結果の読み上げ。システムの SAPI を直接使い、言語で音声を選び、同時に読み上げるのは 1 つだけ
└── ui/
    └── widgets.h/.cpp                   # 翻訳ウィジェット
```

</details>

**主な機能:**
- プラグイン可能なマルチプロバイダー構成をレジストリで一元管理
- ワンキー：選択テキストを自動検出して翻訳ポップアップへ
- 選択テキスト／手入力の2モード対応コンパクトポップアップ
- 非同期翻訳でUIをブロックしない
- 翻訳結果のコピーに対応
- 翻訳結果をシステム音声で読み上げ（言語に合わせて音声を選択、同時に読み上げるのは 1 つだけ）

---

### translations/ — 言語リソース

多言語の UI 文言。`cpp/resources/translations/` にあります。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
translations/
├── app_zh.xml / app_en.xml  # 中国語 / 英語ソースファイル
├── app_ja.xml / app_ko.xml  # 日本語 / 韓国語ソースファイル
├── app_zh_TW.xml            # 繁体字中国語（台湾）ソースファイル
└── app_*.qm                 # コンパイル済み Qt バイナリ（app_zh.qm 等）
```

</details>

`.xml` = 編集可能なソースファイル、`.qm` = Qt ランタイムで読み込むコンパイル済みファイル。UI テキストを追加・変更したら、5 つの `.xml` すべてに同じコンテキストのエントリを追加し（複数行の文言は実際の改行で書く）、`scripts/compile_translations.py` を実行して再コンパイルしてください。ログの文言は `resources/core/log_translations.json` を直接編集します。

---

### ui/ — UIモジュール

共通UIコンポーネントライブラリ。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
ui/
├── toolbar.h/.cpp                   # Toolbar / ToolbarDragHandle — ドラッグ可能なツールバー基底クラス
├── toolbar_layout.h/.cpp            # スクリーンショット / ピンツールバーのボタン配置（順序・表示方法）の正規化と読み書き
├── toolbar_layout_dialog.h/.cpp     # ToolbarLayoutDialog — スクリーンショット / ピンツールバーの配置編集ダイアログ
├── reorderable_rows.h/.cpp          # DragGrip / DraggableRow / ReorderableRowList — ドラッグで並べ替えられる行リスト
├── tray_menu.h/.cpp                 # CustomTrayMenu — システムトレイメニュー
├── screenshot_window.h/.cpp         # ScreenshotWindow — フルスクリーンキャプチャウィンドウ
├── quick_capture_overlay.h/.cpp     # 通常キャプチャの選択枠・座標・拡大鏡を再利用する透明レイヤー
├── update_dialog.h/.cpp             # 更新内容とダウンロード進捗
├── dialogs.h/.cpp                   # StandardDialog — 確認、警告、情報、エラーダイアログ
├── toast.h/.cpp                     # Toast — カーソル横に出る、フォーカスを奪わない一行通知
├── submenu_position.h/.cpp          # カスケードサブメニューの展開方向と親項目への吸着
├── magnifier.h/.cpp                 # MagnifierOverlay — ピクセルレベル拡大鏡
├── color_picker_button.h/.cpp       # ColorPickerButton — カラー選択ボタン
├── hotkey_edit.h/.cpp               # HotkeyEdit — グローバルホットキーエディター
├── inapp_key_edit.h/.cpp            # InAppKeyEdit — アプリ内ショートカットエディター
├── key_chip.h/.cpp                  # KeyChipLineEdit / StatusIcon — ショートカットのキー表示と状態アイコン
├── mask_overlay.h/.cpp              # マスクオーバーレイヤー
├── selection_overlay.h/.cpp         # SelectionOverlayWidget — マスク上に重なる選択装飾レイヤー
├── base_settings_panel.h/.cpp       # BaseSettingsPanel / StepperWidget — 設定パネル基底クラス
├── paint_settings_panel.h/.cpp      # PaintSettingsPanel — ブラシ設定パネル
├── shape_settings_panel.h/.cpp      # ShapeSettingsPanel — 形状設定パネル
├── text_settings_panel.h/.cpp       # TextSettingsPanel — テキスト設定パネル
├── arrow_settings_panel.h/.cpp      # ArrowSettingsPanel — 矢印設定パネル
├── number_settings_panel.h/.cpp     # 番号ツール設定パネル
├── mosaic_settings_panel.h/.cpp     # モザイクツール設定パネル
├── watermark_settings_panel.h/.cpp  # 透かしパネル：文字・位置・スタイル（不透明度/サイズ/間隔/角度/グリッド線/色）・フォント
├── watermark_input_dialog.h/.cpp    # WatermarkInputDialog — 複数行の透かし入力、時刻の挿入とプレビュー
├── initial_selection.h/.cpp         # InitialSelectionGate — 最初のポイント検出が返る（最大 50 ms）まで撮影ウィンドウの表示を待ち、最初のフレームから選択範囲を出す
│
├── fluent_lite/                     # Fluent スタイル軽量コンポーネントライブラリ
│   ├── buttons.h/.cpp               # ボタン
│   ├── cards.h/.cpp                 # カード
│   ├── icons.h/.cpp                 # アイコン
│   ├── inputs.h/.cpp                # 入力欄
│   ├── labels.h/.cpp                # ラベル
│   ├── navigation.h/.cpp            # ナビゲーション
│   ├── segmented.h/.cpp             # セグメントコントロール
│   ├── switch.h/.cpp                # スイッチ
│   ├── theme.h/.cpp                 # テーマ
│   ├── titlebar.h/.cpp              # タイトルバー
│   ├── frameless.h/.cpp             # フレームレスウィンドウ
│   ├── text_context_menu.h/.cpp     # テキスト右クリックメニュー
│   └── fluent_lite.h/.cpp           # まとめ用ヘッダーとチェックボタン
│
├── settings_ui/                     # アプリ設定ダイアログ
│   ├── dialog.h/.cpp                # SettingsDialog — タブ式設定ダイアログ
│   ├── components.h/.cpp            # SettingCardGroup / ToggleSwitch — 設定コンポーネント
│   ├── page_appearance.cpp          # 外観設定（テーマ、言語）
│   ├── page_capture.cpp             # キャプチャ設定
│   ├── page_quick_actions.cpp       # クイック動作設定（確認・結果ウィンドウの省略）
│   ├── color_format_dialog.h/.cpp   # ColorFormatDialog — 拡大鏡のカラー形式を管理するダイアログ
│   ├── ppocr_model_dialog.h/.cpp    # PpocrModelDialog — PP-OCR モデルのダウンロード・追加・削除
│   ├── page_clipboard.cpp           # クリップボード設定
│   ├── special_paste_dialog.h/.cpp  # SpecialPasteDialog — 特殊貼り付けメニューの並び替えとオン／オフ
│   ├── page_hotkey.cpp              # ホットキー設定
│   ├── page_mouse.cpp               # マウスショートカット設定
│   ├── page_pin.cpp                 # ピン画像設定（動作、外観、マウス透過）
│   ├── page_translation.cpp         # 翻訳設定
│   ├── provider_fields.cpp          # 翻訳サービスの設定項目の読み書き（宣言ベース）
│   ├── page_log.cpp                 # ログ設定
│   ├── page_developer.cpp           # 開発者設定
│   ├── page_misc.cpp                # その他設定
│   ├── page_about.cpp               # アバウトページ
│   ├── mouse_binding_editor.h       # MouseBindingEditor — マウスショートカットの修飾キーとジェスチャーの選択
│   └── pages.h                      # 各設定ページの構築関数の宣言
│
├── welcome/                         # 初回起動ウェルカムウィザード（6ページガイド）
│   ├── wizard.h/.cpp                # WelcomeWizard — ウィザードメインウィンドウ
│   ├── base_page.h/.cpp             # BasePage — ウィザードページ基底クラス
│   ├── page1_welcome.cpp            # ウェルカムページ
│   ├── page2_screenshot.cpp         # スクリーンショットホットキー設定ページ
│   ├── page3_clipboard.cpp          # クリップボードホットキー設定ページ
│   ├── page5_translation.cpp        # 翻訳機能説明ページ
│   ├── page6_finish.cpp             # 完了ページ
│   ├── page_hotkeys.cpp             # グローバルホットキーページ — 6 つのキーは同一の衝突領域、まとめて設定・重複判定
│   └── pages.h                      # ウィザード各ページの宣言
│
└── selection_info/                  # 選択情報UI
    ├── controller.h/.cpp            # 選択情報コントローラー
    ├── panel.h/.cpp                 # 選択情報パネル（サイズ、座標）
    ├── border_shadow.h/.cpp         # 選択ボーダーシャドウエフェクト
    ├── lock_ratio.h/.cpp            # アスペクト比ロック
    └── rounded_corners.h/.cpp       # 角丸スクリーンショット
```

</details>

**主な機能:**
- ドラッグ可能なツールバー基底クラス（すべてのツールバーが継承）
- 範囲選択の描画と操作を処理する全画面スクリーンショットウィンドウ
- 拡大鏡やカラーピッカーなどの細かな UI 部品
- 軽量な Fluent スタイルのコンポーネントライブラリ（ボタン、カード、ナビゲーション、セグメント、スイッチなど）
- 完全なアプリ設定ダイアログ（外観・キャプチャ・ホットキー・翻訳などの複数ページ）
- 初回起動のウェルカムウィザード（6 ページの案内フロー）
- 選択範囲情報パネル（サイズ表示、縦横比ロック、角丸など）

---

### video/ — MP4 録画ワーカー

MP4 録画のワーカープロセス。同じ exe を `--video-recorder-worker` 付きで起動し、ネイティブレコーダーはこのプロセス内だけで動くため、フレームは GUI プロセスに入りません。コマンドは標準入力の JSON 行で受け、イベントは標準出力の JSON 行で返します。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
video/
└── worker.h/.cpp  # worker_main — MP4 録画プロセスの入口。標準入力で JSON 行のコマンドを受け、標準出力へ JSON 行のイベントを返し、フレームはネイティブレコーダー内に留める
```

</details>

- H.264 動画を 1–60 fps、目標ビットレート 0.5–50 Mbps で録画
- システム音声（既定の再生デバイス）とマウスポインターは任意。映像は SDR
- MP4 の処理完了後に保存成功を報告

---

### tests/ — テストモジュール

QtTest によるテスト。名前が `_desktop` で終わるテストは実際のデスクトップで、それ以外はオフスクリーンで実行できます。すべてのテストプログラムが `test_sandbox.cpp` をリンクし、設定・データ・ログは一時フォルダーに置かれます。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
tests/
├── test_barcode.cpp               # QR コード・バーコードの認識
├── test_bundled_runtime.cpp       # 完全版に内蔵したランタイムの書き出し
├── test_canvas_items.cpp          # キャンバスのアイテム：コントロールポイント、枠線、外観設定
├── test_canvas_parity.cpp         # キャンバス図形のジオメトリ、ピクセル、アンドゥ、所有権
├── test_canvas_view.cpp           # キャンバスビュー
├── test_capture_finders.cpp       # 実デスクトップでのウィンドウ検索とシステムカーソル（デスクトップテスト）
├── test_clipboard.cpp             # クリップボードのロジックとコンテキストメニュー
├── test_clipboard_backend.cpp     # クリップボードのバックエンド：データベースとシステムクリップボードの監視
├── test_clipboard_controller.cpp  # クリップボードウィンドウのコントローラー：ページング、フィルター、グループ、貼り付け
├── test_clipboard_manage.cpp      # クリップボード管理ウィンドウ
├── test_clipboard_selection.cpp   # クリップボードリストのキーボード・マウス選択
├── test_clipboard_ui.cpp          # クリップボードのバックエンド・ウィンドウ・設定の連携
├── test_clipboard_widgets.cpp     # クリップボードの部品：行描画、プレビュー、歯車メニュー、テーマ
├── test_clipboard_window.cpp      # クリップボードウィンドウのクリック、キー、メニュー
├── test_core.cpp                  # コア：グローバルホットキー、ログ文言、管理者タスク、単一インスタンスの引き継ぎ
├── test_freehand.cpp              # ペンストロークの平滑化、スナップ、分割再描画
├── test_gif.cpp                   # GIF/MP4 の録画・再生・エクスポート
├── test_hdr_capture.cpp           # HDR キャプチャ（デスクトップテスト）
├── test_http_providers.cpp        # HTTP クライアントとアドレスを設定できる翻訳プロバイダー
├── test_logger.cpp                # ログ
├── test_main_app.cpp              # メインアプリ：トレイ、ホットキー、撮影フロー、グローバル入力（デスクトップテスト）
├── test_msaa_element_finder.cpp   # ポイント検出と最初のフレームの選択範囲
├── test_ocr_manager.cpp           # OCR エンジンとモデルの管理
├── test_parity.cpp                # 選択状態、直近の領域、ツールバー配置、拡大率の丸め、設定の既定値と永続化
├── test_pin.cpp                   # ピン留め：回転、OCR 座標の対応付け、ウィンドウの動作
├── test_platform.cpp              # プラットフォーム関数：プロセス、DPI、自動起動、クラッシュログ
├── test_ppocr_models.cpp          # PP-OCR モデルのダウンロード・追加・カタログ
├── test_result_windows.cpp        # OCR とコード読み取りの結果ウィンドウ
├── test_sandbox.cpp               # 全テストプログラムにリンク：設定・データ・ログを一時フォルダーに向ける
├── test_screenshot_window.cpp     # スクリーンショットウィンドウ
├── test_scroll_capture.cpp        # スクロールキャプチャ
├── test_settings_window.cpp       # 設定ウィンドウ
├── test_shortcut_manager.cpp      # ショートカット管理とグローバルフック
├── test_smart_selection.cpp       # 実ウィンドウでのスマート選択（デスクトップテスト）
├── test_stitch.cpp                # 長いスクリーンショットの結合
├── test_text_recognition.cpp      # テキスト認識：整形とウィンドウ配置
├── test_translation.cpp           # 翻訳プロバイダー、レジストリ、サービス
├── test_translation_speech.cpp    # 翻訳の読み上げ（偽のバックエンドを使い、システム音声には触れない）
├── test_translation_ui.cpp        # 翻訳ウィンドウとポップアップ
├── test_ui_widgets.cpp            # 共通 UI 部品：ツールバー、設定パネル、ホットキー編集など
├── test_update.cpp                # 更新確認とワンクリック更新（偽の更新ツールを使用）
├── clipboard_test_support.h       # クリップボードテスト共通の準備コード
├── fake_updater.cpp               # テストで更新ツールの代わりをする偽プログラム
├── leak_check.h/.cpp              # リーク検出モード：テストを 3 回実行し、デバッグヒープでリークを検出
└── fixtures/                      # 期待結果データ（コミット済み、再生成しない）
```

</details>

---

## ライセンス

著作権と MIT ライセンスは [LICENSE](LICENSE) に、Qt などのサードパーティコンポーネントについては [THIRD-PARTY-NOTICES.md](cpp/THIRD-PARTY-NOTICES.md) に記載しています。
