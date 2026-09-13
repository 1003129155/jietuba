"""main/barcode/ 目录下 log_* 调用的中→英翻译表。"""

TRANSLATIONS: dict[str, str] = {
    "扫码完成: 识别到 {count} 个码, 耗时 {elapsed_ms:.0f} ms": "Code scan finished: {count} code(s) found in {elapsed_ms:.0f} ms",
}
