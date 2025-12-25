# Jietuba スクリーンショットツール
スマート選択、画像編集、OCR認識、長いスクリーンショット結合などの機能を備えた強力な Windows スクリーンショットツール。


## 📸 機能

### コア機能

- **🎯 スマートスクリーンショット**
  - ホットキースクリーンショット（デフォルト Ctrl+1）
  - スマートウィンドウ/コントロール認識
  - マルチモニター対応
  - 全画面スクリーンショット

- **🎨 豊富な編集ツール**
  - ペン：自由描画　--shift押しながら直線
  - 直線/矢印：正確な注釈
  - 矩形/楕円：塗りつぶしと枠線付き
  - テキスト：フォント、色、サイズ調整対応
  - 蛍光ペン：マーキング　--shift押しながら直線
  - 消しゴム：正確な消去
  - 番号注釈：自動インクリメント--shift+スクロールで番号を変える

- **📌 ピン機能**
  - スクリーンショットをデスクトップに固定
  - ドラッグ、拡大縮小可能　--スクロール
  - 再編集対応

- **🔤 ピン留め画像にOCR文字認識（自動）**
  - RapidOCR エンジンベース（ローカル）
  - 中国語・英語・日本語対応
  - リアルタイム領域認識
  - 自動レイアウト処理
  - テキストレイヤー編集可能

- **📜 長いスクリーンショット**
  - スマートスクロールスクリーンショット
  - 自作の合成方法
  -　効率のためにRustで自動重複削除結合

**プログラムを実行**
python main_app.py

### 初回使用

1. プログラム起動後、システムトレイにアイコンが表示されます
2. トレイアイコンを右クリックすると：
   - スクリーンショット開始（または Ctrl+1 を押す）
   - 設定を開く
   - プログラムを終了
3. スクリーンショット後、各種編集ツールを使用可能
4. `Enter` で保存、`ESC` でキャンセル

### ディレクトリ構造

```text
jietu/
├── 新架構文件/                    # メインソースコードディレクトリ
│   ├── main_app.py                # アプリケーションエントリーポイント
│   ├── requirements_no_ocr.txt    # 基本版依存関係
│   ├── requirements_with_ocr.txt  # 完全版依存関係
│   ├── OCR_SETUP_GUIDE.md         # OCR 設定ガイド
│   │
│   ├── canvas/                    # キャンバスシステム（コア描画エンジン）
│   │   ├── model.py              # データモデル（描画項目、選択領域）
│   │   ├── scene.py              # シーン管理（QGraphicsScene）
│   │   ├── view.py               # ビューコントロール（ズーム、ドラッグ、スマート選択）
│   │   ├── toolbar_adapter.py    # ツールバーアダプター
│   │   ├── layer_editor.py       # レイヤーエディター
│   │   ├── export.py             # 画像エクスポート
│   │   ├── undo.py               # 元に戻す/やり直しシステム
│   │   ├── snap_system.py        # スナップアシストシステム
│   │   ├── cursor_decision.py    # カーソル状態管理
│   │   ├── smart_edit_controller.py  # スマート編集コントローラー
│   │   └── items/                # 描画項目（図形要素）
│   │       ├── base.py           # 基本図形項目
│   │       ├── pen.py            # ペン図形項目
│   │       ├── arrow.py          # 矢印図形項目
│   │       ├── rect.py           # 矩形図形項目
│   │       ├── ellipse.py        # 楕円図形項目
│   │       ├── text.py           # テキスト図形項目
│   │       ├── mosaic.py         # モザイク図形項目
│   │       └── ...
│   │
│   ├── capture/                   # スクリーンショットキャプチャモジュール
│   │   ├── capture_service.py    # スクリーンショットサービス（マルチモニター対応）
│   │   ├── window_finder.py      # スマートウィンドウ認識（Windows API）
│   │   └── SMART_SELECTION.md    # スマート選択説明ドキュメント
│   │
│   ├── tools/                     # 描画ツールセット
│   │   ├── base.py               # ツール基底クラス
│   │   ├── controller.py         # ツールコントローラー
│   │   ├── pen.py                # ペンツール
│   │   ├── arrow.py              # 矢印ツール
│   │   ├── rect.py               # 矩形ツール
│   │   ├── ellipse.py            # 楕円ツール
│   │   ├── text.py               # テキストツール
│   │   ├── highlighter.py        # 蛍光ペンツール
│   │   ├── eraser.py             # 消しゴムツール
│   │   ├── number.py             # 番号ツール
│   │   ├── cursor.py             # カーソルツール（選択・移動）
│   │   ├── action.py             # アクションツール（元に戻す/やり直し）
│   │   └── cursor_manager.py     # カーソルマネージャー
│   │
│   ├── pin/                       # ピンモジュール
│   │   ├── pin_window.py         # ピンウィンドウ
│   │   ├── pin_manager.py        # ピンマネージャー
│   │   ├── pin_canvas.py         # ピンキャンバス
│   │   ├── pin_canvas_view.py    # ピンビュー
│   │   ├── pin_canvas_renderer.py # ピンレンダラー
│   │   ├── pin_toolbar.py        # ピンツールバー
│   │   ├── ocr_text_layer.py     # OCRテキストレイヤー
│   │   └── pin_mock_scene.py     # モックシーン（ピン用）
│   │
│   ├── ocr/                       # OCR 認識モジュール
│   │   ├── ocr_manager.py        # OCR マネージャー
│   │   ├── OCR_INTEGRATION.md    # OCR 統合ドキュメント
│   │   └── ocr_model_checker.py  # （core/内）モデルチェッカー
│   │
│   ├── stitch/                    # 長いスクリーンショット結合モジュール
│   │   ├── scroll_window.py      # スクロールスクリーンショットウィンドウ
│   │   ├── jietuba_long_stitch.py       # 結合アルゴリズム（スマート選択）
│   │   ├── jietuba_long_stitch_rust.py  # Rust アクセラレーション版
│   │   └── jietuba_long_stitch_unified.py # 統一インターフェース
│   │
│   ├── ui/                        # ユーザーインターフェースモジュール
│   │   ├── screenshot_window.py  # スクリーンショットウィンドウ（メインウィンドウ）
│   │   ├── settings_window.py    # 設定ウィンドウ
│   │   ├── toolbar.py            # ツールバー
│   │   ├── color_board.py        # カラーピッカー
│   │   ├── size_slider.py        # サイズスライダー
│   │   └── ...
│   │
│   ├── core/                      # コア機能モジュール
│   │   ├── hotkey_system.py      # グローバルホットキーシステム
│   │   ├── resource_manager.py   # リソースマネージャー
│   │   ├── logger.py             # ログシステム
│   │   ├── save.py               # 保存機能
│   │   └── ocr_model_checker.py  # OCRモデルチェック
│   │
│   └── settings/                  # 設定管理モジュール
│       └── tool_settings.py      # ツール設定マネージャー
│
├── packaging/                     # パッケージングスクリプト
│   ├── build_no_ocr.py           # OCRなし版パッケージング
│   ├── build_with_ocr.py         # 完全版パッケージング
│   ├── build_no_ocr_onefile.py   # 単一ファイルOCRなし版
│   └── build_with_ocr_onefile.py # 単一ファイル完全版
│
├── FFmpeg/                        # FFmpeg バイナリファイル
├── svg/                           # SVG アイコンリソース
├── build/                         # ビルド出力ディレクトリ
└── 老代码/                        # 旧バージョンコード（非推奨）
<div align="center">

**このプロジェクトが役に立った場合は、⭐ Star をお願いします！**

Made with ❤️ by RiJyaaru

</div>
