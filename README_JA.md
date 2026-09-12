[中文](README.md) | [English](README_EN.md) | **[日本語](README_JA.md)**

# スクリーンショット＆クリップボード管理ソフト — jietuba

[Windows 版をダウンロード](https://github.com/1003129155/jietuba/releases/latest) · [ソースから実行](#source-setup) · [開発とテスト](#development)

## 概要

Windows x86_64 および ARM64 向けのスクリーンショット・クリップボード管理アプリケーションです。UI は PySide6、画像処理・クリップボード操作・OCR などは Rust で実装しています。領域キャプチャ、ウィンドウスマート検出、GIF録画、長いスクリーンショットの結合、OCR文字認識、画像ピン留め、翻訳、モザイク、PDFエクスポート機能を備え、完全なクリップボード履歴管理システムを搭載しています。

すぐに使える Windows 版の配布パッケージと、ソースからの実行方法を用意しています。

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

Windows x86_64 版および ARM64 版の配布パッケージは、そのまま実行できます。Python、Rust、開発環境のインストールは不要です。

1. [Releases ページ](https://github.com/1003129155/jietuba/releases/latest)を開き、端末に合わせて `-x64.zip` または `-arm64.zip` で終わるファイルをダウンロードします。
2. ZIP 全体を展開し、`jietuba_pp.exe` と `models/` フォルダを同じ階層に置きます。
3. `jietuba_pp.exe` をダブルクリックして起動します。OCR モデルは配布パッケージに同梱されています。
4. アプリにはデジタル署名がないため、ブラウザーからダウンロードすると Windows の警告が表示される場合があります。表示された場合は「詳細情報」をクリックし、「実行」を選択すると起動できます。

---

<a id="source-setup"></a>

## ソースから実行

実行に必要な依存パッケージは、すべて [requirements.txt](requirements.txt) で一括インストールできます。

### ワンクリックセットアップ

1. まず Python 3.11 x64（Python Launcher を含む）をインストールします。
2. 本リポジトリをダウンロード・展開するかクローンし、プロジェクトルートの [setup.bat](setup.bat) をダブルクリックします。

### 手動セットアップ

Python Launcher を含む Python 3.11 x64 をインストールしてから、プロジェクトルートで Windows コマンドプロンプト（CMD）を開き、順に実行してください。

**1. 仮想環境の作成と有効化**

```bat
py -3.11 -m venv venv311
call venv311\Scripts\activate.bat
```

**2. 実行時依存パッケージのインストール**

```bat
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

**3. アプリの実行**

```bat
cd main
python main_app.py
```

OCR モデルの `PP-OCRv6_det_small.onnx` と `PP-OCRv6_rec_small.onnx` は、リポジトリの [models/](models/) に同梱されています。OCR を使う際は、このディレクトリをそのまま保持してください。

### Rust 拡張パッケージ

以下の 4 パッケージは `requirements.txt` に含まれ、実行時依存パッケージと一緒にインストールされます。個別のライブラリとしても利用でき、ソースコードは [rust_libs/](rust_libs/) にあります。PyPI の配布名と Python の import 名の対応は次のとおりです。

| pip パッケージ名 | import 名 | バージョン | 機能 |
|------|------|------|------|
| [`j-gif`](https://pypi.org/project/j-gif/) | `gifrecorder` | 0.3.0 | GIF/動画合成エンコーダー |
| [`j-stitch`](https://pypi.org/project/j-stitch/) | `longstitch` | 0.4.0 | 長いスクリーンショット結合アルゴリズム |
| [`j-clipboard`](https://pypi.org/project/j-clipboard/) | `pyclipboard` | 0.4.0 | クリップボード操作 |
| [`j-ppocr`](https://pypi.org/project/j-ppocr/) | `ppocr_rust` | 0.2.0 | PP-OCR (PaddleOCR) ONNX 文字認識エンジン（純 Rust + ONNX Runtime、det/rec モデルが必要） |

ビルド済み wheel は Windows x86_64 および ARM64 向けです。各パッケージの Python バージョン指定は `>=3.11` で、Rust バインディングでは `abi3-py311` を有効にしています。詳細は各パッケージの `pyproject.toml` と `Cargo.toml` を参照してください。

---

<a id="development"></a>

## 開発とテスト

仮想環境を有効にした状態で、プロジェクトルートから開発用依存パッケージをインストールし、テストを実行します。

```bat
python -m pip install -r requirements-dev.txt
python -m pytest main/tests -c main/tests/pytest.ini
```

[テストディレクトリ](main/tests/)には、キャプチャ、クリップボード、モザイク編集、ピン留め画像のズーム、GIF 再生、OCR テキストレイヤーなどのユニットテスト・統合テストがあります。[CI 設定](.github/workflows/ci.yml)では Windows + Python 3.11 環境で静的解析、テスト、カバレッジ検査を実行します。実行結果は [GitHub Actions](https://github.com/1003129155/jietuba/actions/workflows/ci.yml) で確認できます。

Windows 版のビルドは `python build_with_ocr_onefile.py` で実行できます。生成物は `dist/jietuba_pp.exe` と `dist/models/` です。自動リリースの設定は [build.yml](.github/workflows/build.yml) を参照してください。

---

## ディレクトリ構造

<details>
<summary>ディレクトリ構造を表示</summary>

```text
# プロジェクトルート
├── README.md / README_EN.md / README_JA.md             # 中国語・英語・日本語ドキュメント
├── pyproject.toml                                      # Pythonプロジェクトのメタデータと依存関係
├── requirements.txt                                   # 実行時依存パッケージ
├── requirements-dev.txt                               # テスト・ビルド用依存パッケージ
├── build_with_ocr_onefile.py                           # PyInstaller単一ファイルビルドスクリプト
│
├── main/                    # Python メインプログラム
│   ├── main_app.py          # アプリエントリポイント：システムトレイ、グローバルホットキー、ライフサイクル管理
│   ├── compile_translations.py  # 翻訳コンパイラ（.xml → .qm）
│   ├── scripts/             # 補助スクリプト — 翻訳プロバイダー比較
│   │
│   ├── canvas/              # キャンバスモジュール — グラフィックス編集コア
│   ├── capture/             # キャプチャモジュール — スクリーンキャプチャ＆ウィンドウ検出
│   ├── clipboard/           # クリップボードモジュール — 履歴、グループ/クイック起動、入出力、検索
│   ├── core/                # コアモジュール — ブートストラップ、ログ、リソース、テーマ、i18n、ホットキー
│   ├── gif/                 # GIFモジュール — 画面録画、編集、再生、エクスポート
│   ├── ocr/                 # OCRモジュール — PP-OCR 文字認識
│   ├── pin/                 # ピンモジュール — スクリーンショットピン留め、編集、OCR、翻訳
│   ├── settings/            # 設定モジュール — 統一設定管理
│   ├── stitch/              # 結合モジュール — スクロールキャプチャ、自動結合
│   ├── tools/               # ツールモジュール — ペン、矩形、矢印、テキスト、モザイク等
│   ├── translation/         # 翻訳モジュール — マルチプロバイダー翻訳サービス
│   ├── translations/        # 言語リソース — 中国語/英語/日本語/韓国語
│   ├── ui/                  # UIモジュール — 共通UIコンポーネントライブラリ
│   └── tests/               # テストモジュール — ユニットテスト＆統合テスト
│
├── rust_libs/               # Rustライブラリソースコード（ソースからビルド可能）
│   ├── gifrecorder/         # GIF/動画合成エンコーダーソース
│   ├── longstitch/          # 長いスクリーンショット結合アルゴリズムソース
│   ├── pyclipboard/         # クリップボード低レベル操作ソース
│   └── ppocr_rust/          # PP-OCR (PaddleOCR) ONNX 認識エンジンソース
│
├── models/                  # PP-OCR ONNX モデル（OCR に必須）
│   ├── PP-OCRv6_det_small.onnx   # テキスト検出モデル (DBNet)
│   └── PP-OCRv6_rec_small.onnx   # テキスト認識モデル (CRNN/CTC)
│
└── svg/                     # SVGアイコンリソース
```

</details>

---

## モジュール詳細

### canvas/ — キャンバスモジュール

シーン管理、ビューレンダリング、アイテム選択、アンドゥ/リドゥ機能を備えたグラフィックス編集キャンバスシステム。
![jietuba_gif_20260404_000903](https://github.com/user-attachments/assets/5318b991-b0de-46a2-9c0e-d75eeae2a827)

<details>
<summary>ディレクトリ構造を表示</summary>

```text
canvas/
├── scene.py                 # CanvasScene — QGraphicsScene継承のキャンバスシーン
├── view.py                  # CanvasView — QGraphicsView継承のキャンバスビュー
├── selection_model.py       # SelectionModel — 選択グラフィックスアイテムの管理
├── undo.py                  # CommandUndoStack — アンドゥ/リドゥスタック
├── smart_edit_controller.py # SmartEditController — 選択/編集モード切替
├── handle_editor.py         # LayerEditor / EditHandle — コントロールポイントドラッグ編集
└── items/
    ├── drawing_items.py     # StrokeItem / RectItem / EllipseItem / ArrowItem / TextItem / NumberItem
    ├── background_item.py   # BackgroundItem — 選択領域の背景
    ├── mosaic_item.py       # MosaicItem — モザイクアイテム
    └── selection_item.py    # SelectionItem — 選択境界表示
```

</details>

---

### capture/ — キャプチャモジュール

スクリーンキャプチャとスマートウィンドウ検出。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
capture/
├── capture_service.py       # CaptureService — スクリーンショットコアロジック
└── window_finder.py         # WindowFinder — スマートウィンドウ選択、カーソル位置検出
```

</details>

---

### clipboard/ — クリップボード管理モジュール

Ditto風のクリップボード履歴マネージャーで、controllers・core・services・ui の4層構成に再編されています。
テキスト、画像、HTML、ファイルに対応し、グループと内容を管理する独立した3ペイン管理ウィンドウも備えています。
![jietuba_gif_20260404_001128](https://github.com/user-attachments/assets/b0a116e8-d944-43c9-b895-e6fc10d8c08a)

<details>
<summary>ディレクトリ構造を表示</summary>

```text
clipboard/
├── __init__.py
├── controllers/             # 制御層 — 履歴読み込み、貼り付け処理、メニュー、選択状態
│   ├── clipboard_controller.py   # ClipboardController — 読み込み、貼り付け、コンテキストメニュー
│   ├── selection_manager.py      # SelectionManager — リスト選択状態管理
│   └── __init__.py
├── core/                    # データ層 — pyclipboard ラッパー、モデル、グループ種別
│   ├── manager.py           # ClipboardManager — 保存、監視、貼り付け API
│   ├── models.py            # ClipboardItem / Group データモデル
│   ├── enums.py             # GroupType 定義
│   └── __init__.py
├── services/                # サービス層 — file payload、グループ規則、入出力、保存ロジック
│   ├── file_payload_service.py   # file 型 JSON payload と旧形式互換
│   ├── group_service.py          # グループのアイコン・命名・削除確認補助
│   ├── import_export_service.py  # テキスト項目の CSV インポート/エクスポート
│   └── manage_dialog_service.py  # 管理ウィンドウの保存ロジック
├── ui/
│   ├── dialogs/
│   │   └── manage_dialog.py      # グループ・内容・入出力を扱う3ペイン管理ウィンドウ
│   ├── forms/
│   │   ├── group_form.py
│   │   ├── text_content_form.py
│   │   ├── file_content_form.py
│   │   ├── import_export_form.py
│   │   └── group_icon_picker.py
│   ├── mixins/
│   │   └── frameless_mixin.py
│   ├── panels/
│   │   └── setting_panel.py
│   ├── resources/
│   │   └── emoji_data.py
│   ├── theme/
│   │   ├── themes.py
│   │   └── theme_styles.py
│   ├── widgets/
│   │   ├── draggable_list_widget.py
│   │   ├── group_bar.py
│   │   ├── item_delegate.py
│   │   ├── item_widget.py
│   │   └── preview_popup.py
│   └── windows/
│       ├── clipboard_window.py   # 履歴ウィンドウ、検索、プレビュー、高速貼り付け
│       └── pin_window.py         # 履歴項目からピンを作成
```

</details>

**主な機能:**
- システムクリップボードの変更を監視し、自動で履歴を保存
- テキスト、画像、HTML、ファイル等に対応
- 通常グループ、クイック起動グループ、お気に入り、検索に対応
- 独立した3ペイン管理ウィンドウでグループ・テキスト内容・ファイル内容を編集可能
- テキスト項目の CSV インポート/エクスポートに対応
- テーマ切替、ショートカットによる高速貼り付け、大画像/長文プレビューに対応

---

### core/ — コアモジュール

ログ、リソースローディング、テーマ管理、国際化、ホットキー等のインフラ。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
core/
├── bootstrap.py             # PreloadManager — 起動ブートストラップ、環境初期化、DPI、シングルインスタンス
├── logger.py                # Logger — ファイル＋コンソールログ（debug/info/warning/error/exception）
├── crash_handler.py         # install_crash_hooks() — グローバル例外キャッチ
├── resource_manager.py      # ResourceManager — SVG/画像リソースローディング
├── theme.py                 # ThemeManager — アプリテーマカラー管理
├── i18n.py                  # I18nManager / XmlTranslator / tr() — 国際化
├── shortcut_manager.py      # HotkeySystem / ShortcutManager — グローバル＆アプリ内ホットキー
├── save.py                  # SaveService — ファイル保存サービス（高品質 PDF 出力対応）
├── export.py                # ExportService — 画像エクスポート
├── clipboard_utils.py       # copy_image_to_clipboard() — 画像をクリップボードにコピー
├── platform_utils.py        # DPI設定、AppUserModelID、Windows APIユーティリティ
├── qt_utils.py              # safe_disconnect() — Qtシグナル安全切断
├── log_translations/        # 各モジュールのログ翻訳ヘルパー
└── constants.py             # グローバル定数（フォント、パス等）
```

</details>

---

### gif/ — GIF録画モジュール

画面録画、編集、再生、GIF/動画エクスポート。
<img width="766" height="630" alt="image" src="https://github.com/user-attachments/assets/8653fffb-b419-4584-ab4b-9fe95bb9f246" />
<details>
<summary>ディレクトリ構造を表示</summary>

```text
gif/
├── record_window.py         # GifRecordWindow / AppState — ステートマシンコーディネーター（3層ウィンドウ）
├── overlay.py               # CaptureOverlay / OverlayMode — キャプチャオーバーレイ、領域調整
├── drawing_view.py          # GifDrawingView / GifDrawingScene — 録画中描画
├── drawing_toolbar.py       # GifDrawingToolbar — 描画ツールバー
├── record_toolbar.py        # RecordToolbar — 開始/一時停止/停止コントロール
├── frame_recorder.py        # FrameRecorder / FrameData / CursorSnapshot — フレームサンプリング
├── playback_engine.py       # PlaybackEngine / PlayState — フレーム再生＆プレビュー
├── playback_controller.py   # PlaybackController — 再生UI＆エクスポート管理
├── playback_toolbar.py      # PlaybackToolbar / RangeSlider — プログレスバー、速度調整
├── composer.py              # _ComposeWorker / ComposerProgressDialog — GIF/動画合成
├── cursor_overlay.py        # CursorOverlay — カーソルレンダリング＆クリックアニメーション
└── _widgets.py              # ClickMenuButton / svg_icon() — カスタムウィジェット
```

</details>

---

### ocr/ — OCRモジュール

PP-OCR による文字認識管理。

<img width="580" height="505" alt="image" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />

<details>
<summary>ディレクトリ構造を表示</summary>

```text
ocr/
└── ocr_manager.py           # OCRManager — ppocr_rust (PP-OCR) による文字認識
```

</details>

- ppocr_rust エンジン（純 Rust + ONNX Runtime、PP-OCR det + rec）を使用。推論はネイティブスレッドで実行され UI をブロックしない
- 中国語/英語/日本語認識
- シングルトンパターン、統一認識インターフェース

---

### pin/ — ピンモジュール

スクリーンショットを画面にピン留め。編集、ズーム、OCR、翻訳対応。

<img width="737" height="657" alt="image" src="https://github.com/user-attachments/assets/827b912c-11ac-4692-b3f6-826561957615" />

<details>
<summary>ディレクトリ構造を表示</summary>

```text
pin/
├── pin_window.py            # PinWindow — ドラッグ可能、ズーム可能、常に最前面の画像ウィンドウ
├── pin_canvas_view.py       # PinCanvasView — ピンキャンバスビュー
├── pin_canvas.py            # ピンキャンバスオブジェクト
├── pin_manager.py           # PinManager — 全ピンウィンドウ管理（シングルトン）
├── pin_toolbar.py           # PinToolbar — ピンツールバー
├── pin_controls.py          # PinControlButtons — 閉じる、編集、コピーボタン
├── pin_context_menu.py      # PinContextMenu — 右クリックメニュー
├── pin_border_overlay.py    # PinBorderOverlay — ボーダーエフェクトオーバーレイ
├── pin_ocr_manager.py       # PinOCRManager / _OCRThread — 非同期OCR認識
├── pin_shortcut.py          # PinShortcutController — 通常/編集モードショートカット
├── pin_thumbnail.py         # PinThumbnailMode — サムネイルモード
├── pin_translation.py       # PinTranslationHelper — 翻訳ヘルパー
├── pin_image_transform.py   # PinImageTransform — 回転、反転等
└── ocr_text_layer.py        # OCRTextLayer / OCRTextItem — OCRテキストレイヤー表示
```

</details>

---

### settings/ — 設定モジュール

<details>
<summary>ディレクトリ構造を表示</summary>

```text
settings/
└── tool_settings.py         # ToolSettingsManager / ToolSettings — ツールの色、サイズ、ホットキー設定
```

</details>

---

### stitch/ — 長いスクリーンショット結合モジュール
![jietuba_gif_20260404_001930](https://github.com/user-attachments/assets/a9720f08-5128-447d-b425-6d0640272e6a)

<details>
<summary>ディレクトリ構造を表示</summary>

```text
stitch/
├── jietuba_long_stitch_unified.py   # 結合インターフェース（Rust の longstitch を呼び出す）
├── scroll_window.py                 # ScrollCaptureWindow — スクロールキャプチャウィンドウ
└── scroll_toolbar.py                # スクロールキャプチャツールバー
```

</details>

---

### tools/ — 描画ツールモジュール

<details>
<summary>ディレクトリ構造を表示</summary>

```text
tools/
├── base.py                  # Tool / ToolContext — 抽象基底クラス
├── controller.py            # ToolController — ツール切替＆状態管理
├── action.py                # ActionTools — コピー、保存、キャンセルアクション
├── pen.py                   # PenTool — フリーハンド描画
├── rect.py                  # RectTool — 矩形（塗りつぶし/アウトライン）
├── ellipse.py               # EllipseTool — 楕円
├── arrow.py                 # ArrowTool — 矢印
├── text.py                  # TextTool — テキスト
├── number.py                # NumberTool — 自動インクリメント番号
├── highlighter.py           # HighlighterTool — 蛍光ペン
├── mosaic.py                # MosaicTool — モザイク
├── cursor.py                # CursorTool — カーソル/選択
├── eraser.py                # EraserTool — 消しゴム
└── cursor_manager.py        # CursorManager — カーソルスタイル管理
```

</details>

---

### translation/ — 翻訳モジュール

DeepL / Google / Azure / Amazon などに対応したマルチプロバイダー翻訳サービス。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
translation/
├── provider.py              # TranslationProvider / ProviderMetadata — プロバイダー抽象契約
├── registry.py              # ProviderRegistry — プロバイダー登録とファクトリ
├── service.py               # TranslationService — プロバイダー選択と翻訳オーケストレーション
├── models.py                # TranslationRequest / TranslationResult — プロバイダー非依存モデル
├── worker.py                # TranslationWorker — 全プロバイダー共通の Qt ワーカー
├── providers/               # 各翻訳プロバイダー実装
│   ├── deepl.py             # DeepL
│   ├── google.py            # Google
│   ├── azure.py             # Azure
│   └── amazon.py            # Amazon
├── smart_translation_controller.py # SmartTranslationController — ワンキー選択テキスト検出＆ポップアップルーティング
├── translation_popup.py     # TranslationPopup — コンパクト翻訳ポップアップ（選択テキスト/手入力）
├── deepl_service.py         # DeepLService / TranslationThread — 旧版 DeepL API 非同期翻訳
├── languages.py             # SupportedLanguages — 対応言語リスト・言語コード
├── translation_manager.py   # TranslationManager — 翻訳ウィンドウマネージャー（シングルトン）
├── translation_dialog.py    # TranslationDialog — 翻訳結果ウィンドウ
└── ui/
    ├── dialog.py            # 翻訳ダイアログUI
    └── widgets.py           # 翻訳ウィジェット
```

</details>

**主な機能:**
- プラグイン可能なマルチプロバイダー構成（DeepL / Google / Azure / Amazon）をレジストリで一元管理
- ワンキー：選択テキストを自動検出して翻訳ポップアップへ
- 選択テキスト／手入力の2モード対応コンパクトポップアップ
- 非同期翻訳でUIをブロックしない
- 翻訳結果のコピーに対応

---

### translations/ — 言語リソース

<details>
<summary>ディレクトリ構造を表示</summary>

```text
translations/
├── app_zh.xml / app_en.xml  # 中国語 / 英語ソースファイル
├── app_ja.xml / app_ko.xml  # 日本語 / 韓国語ソースファイル
└── app_*.xml.qm             # コンパイル済み Qt バイナリ（app_zh.xml.qm 等）
```

</details>

`.xml` = 編集可能なソースファイル、`*.xml.qm` = Qtランタイムで読み込むコンパイル済みファイル。変更後は `compile_translations.py` を実行して再コンパイルしてください。

---

### ui/ — UIモジュール

共通UIコンポーネントライブラリ。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
ui/
├── toolbar.py               # Toolbar / _DragHandle — ドラッグ可能なツールバー基底クラス
├── tray_menu.py             # TrayMenu — システムトレイメニュー
├── screenshot_window.py     # ScreenshotWindow — フルスクリーンキャプチャウィンドウ
├── dialogs.py               # StandardDialog — 確認、警告、情報、エラーダイアログ
├── magnifier.py             # MagnifierOverlay — ピクセルレベル拡大鏡
├── color_picker_dialog.py   # ColorPickerDialog — カスタムHSVカラーピッカー
├── color_picker_button.py   # ColorPickerButton — カラー選択ボタン
├── hotkey_edit.py           # HotkeyEdit — グローバルホットキーエディター
├── inapp_key_edit.py        # InAppKeyEdit — アプリ内ショートカットエディター
├── mask_overlay.py          # マスクオーバーレイヤー
├── base_settings_panel.py   # BaseSettingsPanel / StepperWidget — 設定パネル基底クラス
├── paint_settings_panel.py  # PaintSettingsPanel — ブラシ設定パネル
├── shape_settings_panel.py  # ShapeSettingsPanel — 形状設定パネル
├── text_settings_panel.py   # TextSettingsPanel — テキスト設定パネル
├── arrow_settings_panel.py  # ArrowSettingsPanel — 矢印設定パネル
├── number_settings_panel.py # 番号ツール設定パネル
├── mosaic_settings_panel.py # モザイクツール設定パネル
│
├── fluent_lite/             # Fluent スタイル軽量コンポーネントライブラリ
│   ├── buttons.py / cards.py / icons.py / inputs.py  # ボタン、カード、アイコン、入力欄
│   ├── labels.py / navigation.py / segmented.py      # ラベル、ナビゲーション、セグメントコントロール
│   ├── switch.py / theme.py / titlebar.py            # スイッチ、テーマ、タイトルバー
│   ├── frameless.py         # フレームレスウィンドウ
│   └── text_context_menu.py # テキスト右クリックメニュー
│
├── settings_ui/             # アプリ設定ダイアログ
│   ├── dialog.py            # SettingsDialog — タブ式設定ダイアログ
│   ├── components.py        # SettingCardGroup / ToggleSwitch — 設定コンポーネント
│   ├── page_appearance.py   # 外観設定（テーマ、言語）
│   ├── page_capture.py      # キャプチャ設定
│   ├── page_clipboard.py    # クリップボード設定
│   ├── page_hotkey.py       # ホットキー設定
│   ├── page_translation.py  # 翻訳設定
│   ├── page_log.py          # ログ設定
│   ├── page_developer.py    # 開発者設定
│   ├── page_misc.py         # その他設定
│   ├── page_about.py        # アバウトページ
│   └── mock_config.py       # MockConfig — テスト用モック設定
│
├── welcome/                 # 初回起動ウェルカムウィザード（6ページガイド）
│   ├── wizard.py            # WelcomeWizard — ウィザードメインウィンドウ
│   ├── base_page.py         # BasePage — ウィザードページ基底クラス
│   ├── page1_welcome.py     # ウェルカムページ
│   ├── page2_screenshot.py  # スクリーンショットホットキー設定ページ
│   ├── page3_clipboard.py   # クリップボードホットキー設定ページ
│   ├── page4_smart_select.py # スマート選択説明ページ
│   ├── page5_translation.py # 翻訳機能説明ページ
│   └── page6_finish.py      # 完了ページ
│
└── selection_info/          # 選択情報UI
    ├── controller.py        # 選択情報コントローラー
    ├── panel.py             # 選択情報パネル（サイズ、座標）
    ├── hook_manager.py      # フックマネージャー
    ├── border_shadow.py     # 選択ボーダーシャドウエフェクト
    ├── lock_ratio.py        # アスペクト比ロック
    └── rounded_corners.py   # 角丸スクリーンショット
```

</details>

---

### tests/ — テストモジュール

<details>
<summary>ディレクトリ構造を表示</summary>

```text
tests/
├── conftest.py              # pytest設定＆共通フィクスチャ
├── pytest.ini               # pytest実行設定
├── run_tests.py             # テスト実行スクリプト
├── test_drawing_tools.py    # 描画ツールテスト（ペン/矩形/矢印/テキスト/番号/蛍光ペン）
├── test_mosaic_tool.py      # モザイクツールテスト
├── test_functional_handles.py # コントロールポイント編集テスト
├── test_capture_service.py  # キャプチャサービステスト
├── test_clipboard_api.py    # クリップボード公開 API テスト
├── test_clipboard_manage_dialog.py # クリップボード管理ウィンドウテスト
├── test_frame_recorder.py   # GIFフレーム録画テスト
├── test_playback_engine.py  # GIF再生エンジンテスト
├── test_ocr_text_layer.py   # OCRテキストレイヤーテスト
├── test_pin_window_zoom.py  # ピンウィンドウズームテスト
├── test_smart_translation.py # スマート翻訳テスト
├── test_translation_architecture.py # 翻訳プロバイダーアーキテクチャテスト
├── test_stitch_dedup.py     # 長いスクリーンショット結合重複除去テスト
├── test_settings_dialog_state.py # 設定ダイアログ状態テスト
├── test_welcome_translation.py # ウェルカムウィザード翻訳ページテスト
└── …（その他 70 以上のユニット/統合テストファイル）
```

</details>
