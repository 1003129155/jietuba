// Generated from frozen Python AST. Regenerate; do not hand edit.
// main/settings/tool_settings.py:907
QString get_hotkey_2() const { return qsettings.value("app/hotkey_2", app_defaults_.value("hotkey_2")).toString(); }
// main/settings/tool_settings.py:911
void set_hotkey_2(const QVariant &value) { qsettings.setValue("app/hotkey_2", value); }
// main/settings/tool_settings.py:919
void set_clipboard_hotkey(const QVariant &value) { qsettings.setValue("clipboard/hotkey", value); }
// main/settings/tool_settings.py:923
QString get_clipboard_hotkey_2() const { return qsettings.value("clipboard/hotkey_2", app_defaults_.value("clipboard_hotkey_2")).toString(); }
// main/settings/tool_settings.py:927
void set_clipboard_hotkey_2(const QVariant &value) { qsettings.setValue("clipboard/hotkey_2", value); }
// main/settings/tool_settings.py:931
QString get_pin_clipboard_hotkey() const { return qsettings.value("clipboard/pin_hotkey", app_defaults_.value("pin_clipboard_hotkey")).toString(); }
// main/settings/tool_settings.py:939
void set_pin_clipboard_hotkey(const QVariant &value) { qsettings.setValue("clipboard/pin_hotkey", value); }
// main/settings/tool_settings.py:943
QString get_pin_clipboard_hotkey_2() const { return qsettings.value("clipboard/pin_hotkey_2", app_defaults_.value("pin_clipboard_hotkey_2")).toString(); }
// main/settings/tool_settings.py:951
void set_pin_clipboard_hotkey_2(const QVariant &value) { qsettings.setValue("clipboard/pin_hotkey_2", value); }
// main/settings/tool_settings.py:955
QString get_translation_hotkey() const { return qsettings.value("app/translation_hotkey", app_defaults_.value("translation_hotkey")).toString(); }
// main/settings/tool_settings.py:963
void set_translation_hotkey(const QVariant &value) { qsettings.setValue("app/translation_hotkey", value); }
// main/settings/tool_settings.py:967
QString get_translation_hotkey_2() const { return qsettings.value("app/translation_hotkey_2", app_defaults_.value("translation_hotkey_2")).toString(); }
// main/settings/tool_settings.py:975
void set_translation_hotkey_2(const QVariant &value) { qsettings.setValue("app/translation_hotkey_2", value); }
// main/settings/tool_settings.py:1034
QString get_inapp_cursor_move_mode() const { return qsettings.value("inapp/inapp_cursor_move_mode", app_defaults_.value("inapp_cursor_move_mode")).toString(); }
// main/settings/tool_settings.py:1041
void set_inapp_cursor_move_mode(const QVariant &value) { qsettings.setValue("inapp/inapp_cursor_move_mode", value); }
// main/settings/tool_settings.py:1045
QString get_inapp_clipboard_pick_mode() const { return qsettings.value("inapp/inapp_clipboard_pick_mode", app_defaults_.value("inapp_clipboard_pick_mode")).toString(); }
// main/settings/tool_settings.py:1052
void set_inapp_clipboard_pick_mode(const QVariant &value) { qsettings.setValue("inapp/inapp_clipboard_pick_mode", value); }
// main/settings/tool_settings.py:1074
bool get_smart_selection() const { return qsettings.value("app/smart_selection", app_defaults_.value("smart_selection")).toBool(); }
// main/settings/tool_settings.py:1082
bool get_smart_selection_animation() const { return qsettings.value("app/smart_selection_animation", app_defaults_.value("smart_selection_animation")).toBool(); }
// main/settings/tool_settings.py:1090
void set_smart_selection_animation(const QVariant &value) { qsettings.setValue("app/smart_selection_animation", value); }
// main/settings/tool_settings.py:1094
bool get_double_click_copy_close_enabled() const { return qsettings.value("app/double_click_copy_close", app_defaults_.value("double_click_copy_close")).toBool(); }
// main/settings/tool_settings.py:1102
void set_double_click_copy_close_enabled(const QVariant &value) { qsettings.setValue("app/double_click_copy_close", value); }
// main/settings/tool_settings.py:1106
bool get_cross_tool_selection_enabled() const { return qsettings.value("app/cross_tool_selection", app_defaults_.value("cross_tool_selection")).toBool(); }
// main/settings/tool_settings.py:1114
void set_cross_tool_selection_enabled(const QVariant &value) { qsettings.setValue("app/cross_tool_selection", value); }
// main/settings/tool_settings.py:1118
bool get_text_always_on_top_enabled() const { return qsettings.value("app/text_always_on_top", app_defaults_.value("text_always_on_top")).toBool(); }
// main/settings/tool_settings.py:1126
void set_text_always_on_top_enabled(const QVariant &value) { qsettings.setValue("app/text_always_on_top", value); }
// main/settings/tool_settings.py:1130
bool get_ocr_copy_directly_enabled() const { return qsettings.value("app/ocr_copy_directly", app_defaults_.value("ocr_copy_directly")).toBool(); }
// main/settings/tool_settings.py:1138
void set_ocr_copy_directly_enabled(const QVariant &value) { qsettings.setValue("app/ocr_copy_directly", value); }
// main/settings/tool_settings.py:1141
bool get_barcode_copy_single_enabled() const { return qsettings.value("app/barcode_copy_single", app_defaults_.value("barcode_copy_single")).toBool(); }
// main/settings/tool_settings.py:1149
void set_barcode_copy_single_enabled(const QVariant &value) { qsettings.setValue("app/barcode_copy_single", value); }
// main/settings/tool_settings.py:1153
bool get_log_enabled() const { return qsettings.value("app/log_enabled", app_defaults_.value("log_enabled")).toBool(); }
// main/settings/tool_settings.py:1157
void set_log_enabled(const QVariant &value) { qsettings.setValue("app/log_enabled", value); }
// main/settings/tool_settings.py:1161
QString get_log_dir() const { return qsettings.value("app/log_dir", app_defaults_.value("log_dir")).toString(); }
// main/settings/tool_settings.py:1165
void set_log_dir(const QVariant &value) { qsettings.setValue("app/log_dir", value); }
// main/settings/tool_settings.py:1169
QString get_log_level() const { return qsettings.value("app/log_level", app_defaults_.value("log_level")).toString(); }
// main/settings/tool_settings.py:1173
void set_log_level(const QVariant &value) { qsettings.setValue("app/log_level", value); }
// main/settings/tool_settings.py:1177
qint64 get_log_retention_days() const { return qsettings.value("app/log_retention_days", app_defaults_.value("log_retention_days")).toLongLong(); }
// main/settings/tool_settings.py:1181
void set_log_retention_days(const QVariant &value) { qsettings.setValue("app/log_retention_days", value); }
// main/settings/tool_settings.py:1185
QString get_long_stitch_engine() const { return qsettings.value("app/long_stitch_engine", app_defaults_.value("long_stitch_engine")).toString(); }
// main/settings/tool_settings.py:1189
void set_long_stitch_engine(const QVariant &value) { qsettings.setValue("app/long_stitch_engine", value); }
// main/settings/tool_settings.py:1206
qint64 get_long_stitch_ignore_top_pixels() const { return qsettings.value("screenshot/long_stitch_ignore_top_pixels", app_defaults_.value("long_stitch_ignore_top_pixels")).toLongLong(); }
// main/settings/tool_settings.py:1214
void set_long_stitch_ignore_top_pixels(const QVariant &value) { qsettings.setValue("screenshot/long_stitch_ignore_top_pixels", value); }
// main/settings/tool_settings.py:1218
bool get_screenshot_save_enabled() const { return qsettings.value("app/screenshot_save_enabled", app_defaults_.value("screenshot_save_enabled")).toBool(); }
// main/settings/tool_settings.py:1222
void set_screenshot_save_enabled(const QVariant &value) { qsettings.setValue("app/screenshot_save_enabled", value); }
// main/settings/tool_settings.py:1226
bool get_screenshot_pin_save_enabled() const { return qsettings.value("app/screenshot_pin_save_enabled", app_defaults_.value("screenshot_pin_save_enabled")).toBool(); }
// main/settings/tool_settings.py:1234
void set_screenshot_pin_save_enabled(const QVariant &value) { qsettings.setValue("app/screenshot_pin_save_enabled", value); }
// main/settings/tool_settings.py:1238
bool get_screenshot_save_original_enabled() const { return qsettings.value("app/screenshot_save_original_enabled", app_defaults_.value("screenshot_save_original_enabled")).toBool(); }
// main/settings/tool_settings.py:1246
void set_screenshot_save_original_enabled(const QVariant &value) { qsettings.setValue("app/screenshot_save_original_enabled", value); }
// main/settings/tool_settings.py:1266
QString get_screenshot_format() const { return qsettings.value("app/screenshot_format", app_defaults_.value("screenshot_format")).toString(); }
// main/settings/tool_settings.py:1274
bool get_clipboard_file_reference_enabled() const { return qsettings.value("app/clipboard_file_reference_enabled", app_defaults_.value("clipboard_file_reference_enabled")).toBool(); }
// main/settings/tool_settings.py:1282
void set_clipboard_file_reference_enabled(const QVariant &value) { qsettings.setValue("app/clipboard_file_reference_enabled", value); }
// main/settings/tool_settings.py:1307
bool get_screenshot_watermark_auto() const { return qsettings.value("app/screenshot_watermark_auto", app_defaults_.value("screenshot_watermark_auto")).toBool(); }
// main/settings/tool_settings.py:1318
bool get_screenshot_watermark_last_on() const { return qsettings.value("app/screenshot_watermark_last_on", app_defaults_.value("screenshot_watermark_last_on")).toBool(); }
// main/settings/tool_settings.py:1329
qint64 get_screenshot_quality() const { return qsettings.value("app/screenshot_quality", app_defaults_.value("screenshot_quality")).toLongLong(); }
// main/settings/tool_settings.py:1337
bool get_show_main_window() const { return qsettings.value("app/show_main_window", app_defaults_.value("show_main_window")).toBool(); }
// main/settings/tool_settings.py:1341
void set_show_main_window(const QVariant &value) { qsettings.setValue("app/show_main_window", value); }
// main/settings/tool_settings.py:1345
bool get_ocr_enabled() const { return qsettings.value("app/ocr_enabled", app_defaults_.value("ocr_enabled")).toBool(); }
// main/settings/tool_settings.py:1349
void set_ocr_enabled(const QVariant &value) { qsettings.setValue("app/ocr_enabled", value); }
// main/settings/tool_settings.py:1392
bool get_translation_on_top() const { return qsettings.value("app/translation_on_top", app_defaults_.value("translation_on_top")).toBool(); }
// main/settings/tool_settings.py:1403
bool get_ocr_result_on_top() const { return qsettings.value("app/ocr_result_on_top", app_defaults_.value("ocr_result_on_top")).toBool(); }
// main/settings/tool_settings.py:1426
bool get_ocr_remove_spaces() const { return qsettings.value("app/ocr_remove_spaces", app_defaults_.value("ocr_remove_spaces")).toBool(); }
// main/settings/tool_settings.py:1449
bool get_ocr_copy_on_close() const { return qsettings.value("app/ocr_copy_on_close", app_defaults_.value("ocr_copy_on_close")).toBool(); }
// main/settings/tool_settings.py:1460
bool get_ocr_grayscale_enabled() const { return qsettings.value("app/ocr_grayscale", app_defaults_.value("ocr_grayscale")).toBool(); }
// main/settings/tool_settings.py:1466
bool get_pin_auto_toolbar() const { return qsettings.value("pin/auto_toolbar", app_defaults_.value("pin_auto_toolbar")).toBool(); }
// main/settings/tool_settings.py:1470
void set_pin_auto_toolbar(const QVariant &value) { qsettings.setValue("pin/auto_toolbar", value); }
// main/settings/tool_settings.py:1474
double get_pin_default_opacity() const { return qsettings.value("pin/default_opacity", app_defaults_.value("pin_default_opacity")).toDouble(); }
// main/settings/tool_settings.py:1482
void set_ocr_grayscale_enabled(const QVariant &value) { qsettings.setValue("app/ocr_grayscale", value); }
// main/settings/tool_settings.py:1486
bool get_ocr_upscale_enabled() const { return qsettings.value("app/ocr_upscale", app_defaults_.value("ocr_upscale")).toBool(); }
// main/settings/tool_settings.py:1490
void set_ocr_upscale_enabled(const QVariant &value) { qsettings.setValue("app/ocr_upscale", value); }
// main/settings/tool_settings.py:1494
double get_ocr_upscale_factor() const { return qsettings.value("app/ocr_upscale_factor", app_defaults_.value("ocr_upscale_factor")).toDouble(); }
// main/settings/tool_settings.py:1498
void set_ocr_upscale_factor(const QVariant &value) { qsettings.setValue("app/ocr_upscale_factor", value); }
// main/settings/tool_settings.py:1504
QString get_translation_provider() const { return qsettings.value("translation/active_provider", app_defaults_.value("translation_provider")).toString(); }
// main/settings/tool_settings.py:1578
QString get_deepl_api_key() const { return qsettings.value("app/deepl_api_key", app_defaults_.value("deepl_api_key")).toString(); }
// main/settings/tool_settings.py:1582
void set_deepl_api_key(const QVariant &value) { qsettings.setValue("app/deepl_api_key", value); }
// main/settings/tool_settings.py:1586
bool get_deepl_use_pro() const { return qsettings.value("app/deepl_use_pro", app_defaults_.value("deepl_use_pro")).toBool(); }
// main/settings/tool_settings.py:1590
void set_deepl_use_pro(const QVariant &value) { qsettings.setValue("app/deepl_use_pro", value); }
// main/settings/tool_settings.py:1594
QString get_amazon_translate_region() const { return qsettings.value("translation/providers/amazon/region", app_defaults_.value("amazon_translate_region")).toString(); }
// main/settings/tool_settings.py:1607
QString get_amazon_translate_access_key_id() const { return qsettings.value("translation/providers/amazon/access_key_id", app_defaults_.value("amazon_translate_access_key_id")).toString(); }
// main/settings/tool_settings.py:1620
QString get_amazon_translate_secret_access_key() const { return qsettings.value("translation/providers/amazon/secret_access_key", app_defaults_.value("amazon_translate_secret_access_key")).toString(); }
// main/settings/tool_settings.py:1635
QString get_amazon_translate_session_token() const { return qsettings.value("translation/providers/amazon/session_token", app_defaults_.value("amazon_translate_session_token")).toString(); }
// main/settings/tool_settings.py:1648
QString get_google_translate_api_key() const { return qsettings.value("translation/providers/google/api_key", app_defaults_.value("google_translate_api_key")).toString(); }
// main/settings/tool_settings.py:1661
QString get_azure_translate_api_key() const { return qsettings.value("translation/providers/azure/api_key", app_defaults_.value("azure_translate_api_key")).toString(); }
// main/settings/tool_settings.py:1674
QString get_azure_translate_region() const { return qsettings.value("translation/providers/azure/region", app_defaults_.value("azure_translate_region")).toString(); }
// main/settings/tool_settings.py:1687
QString get_azure_translate_endpoint() const { return qsettings.value("translation/providers/azure/endpoint", app_defaults_.value("azure_translate_endpoint")).toString(); }
// main/settings/tool_settings.py:1700
QString get_baidu_translate_appid() const { return qsettings.value("translation/providers/baidu/appid", app_defaults_.value("baidu_translate_appid")).toString(); }
// main/settings/tool_settings.py:1713
QString get_baidu_translate_secret_key() const { return qsettings.value("translation/providers/baidu/secret_key", app_defaults_.value("baidu_translate_secret_key")).toString(); }
// main/settings/tool_settings.py:1726
QString get_volcengine_translate_access_key() const { return qsettings.value("translation/providers/volcengine/access_key_id", app_defaults_.value("volcengine_translate_access_key")).toString(); }
// main/settings/tool_settings.py:1739
QString get_volcengine_translate_secret_key() const { return qsettings.value("translation/providers/volcengine/secret_access_key", app_defaults_.value("volcengine_translate_secret_key")).toString(); }
// main/settings/tool_settings.py:1752
QString get_tencent_translate_secret_id() const { return qsettings.value("translation/providers/tencent/secret_id", app_defaults_.value("tencent_translate_secret_id")).toString(); }
// main/settings/tool_settings.py:1765
QString get_tencent_translate_secret_key() const { return qsettings.value("translation/providers/tencent/secret_key", app_defaults_.value("tencent_translate_secret_key")).toString(); }
// main/settings/tool_settings.py:1781
bool get_volcengine_translate_quota_enabled() const { return qsettings.value("translation/providers/volcengine/quota_enabled", app_defaults_.value("volcengine_translate_quota_enabled")).toBool(); }
// main/settings/tool_settings.py:1807
bool get_tencent_translate_quota_enabled() const { return qsettings.value("translation/providers/tencent/quota_enabled", app_defaults_.value("tencent_translate_quota_enabled")).toBool(); }
// main/settings/tool_settings.py:1874
QString get_deepseek_api_key() const { return qsettings.value("translation/providers/deepseek/api_key", app_defaults_.value("deepseek_api_key")).toString(); }
// main/settings/tool_settings.py:1887
QString get_deepseek_model() const { return qsettings.value("translation/providers/deepseek/model", app_defaults_.value("deepseek_model")).toString(); }
// main/settings/tool_settings.py:1900
QString get_deepseek_base_url() const { return qsettings.value("translation/providers/deepseek/base_url", app_defaults_.value("deepseek_base_url")).toString(); }
// main/settings/tool_settings.py:1957
void set_translation_target_lang(const QVariant &value) { qsettings.setValue("app/translation_target_lang", value); }
// main/settings/tool_settings.py:1961
bool get_translation_split_sentences() const { return qsettings.value("app/translation_split_sentences", app_defaults_.value("translation_split_sentences")).toBool(); }
// main/settings/tool_settings.py:1965
void set_translation_split_sentences(const QVariant &value) { qsettings.setValue("app/translation_split_sentences", value); }
// main/settings/tool_settings.py:1969
bool get_translation_preserve_formatting() const { return qsettings.value("app/translation_preserve_formatting", app_defaults_.value("translation_preserve_formatting")).toBool(); }
// main/settings/tool_settings.py:1973
void set_translation_preserve_formatting(const QVariant &value) { qsettings.setValue("app/translation_preserve_formatting", value); }
// main/settings/tool_settings.py:2031
bool get_clipboard_enabled() const { return qsettings.value("clipboard/enabled", app_defaults_.value("clipboard_enabled")).toBool(); }
// main/settings/tool_settings.py:2035
void set_clipboard_enabled(const QVariant &value) { qsettings.setValue("clipboard/enabled", value); }
// main/settings/tool_settings.py:2039
bool get_clipboard_auto_paste() const { return qsettings.value("clipboard/auto_paste", app_defaults_.value("clipboard_auto_paste")).toBool(); }
// main/settings/tool_settings.py:2043
void set_clipboard_auto_paste(const QVariant &value) { qsettings.setValue("clipboard/auto_paste", value); }
// main/settings/tool_settings.py:2047
bool get_clipboard_close_after_paste() const { return qsettings.value("clipboard/close_after_paste", app_defaults_.value("clipboard_close_after_paste")).toBool(); }
// main/settings/tool_settings.py:2055
void set_clipboard_close_after_paste(const QVariant &value) { qsettings.setValue("clipboard/close_after_paste", value); }
// main/settings/tool_settings.py:2059
qint64 get_clipboard_history_limit() const { return qsettings.value("clipboard/history_limit", app_defaults_.value("clipboard_history_limit")).toLongLong(); }
// main/settings/tool_settings.py:2067
QString get_clipboard_db_path() const { return qsettings.value("clipboard/db_path", app_defaults_.value("clipboard_db_path")).toString(); }
// main/settings/tool_settings.py:2075
bool get_clipboard_auto_cleanup() const { return qsettings.value("clipboard/auto_cleanup", app_defaults_.value("clipboard_auto_cleanup")).toBool(); }
// main/settings/tool_settings.py:2079
void set_clipboard_auto_cleanup(const QVariant &value) { qsettings.setValue("clipboard/auto_cleanup", value); }
// main/settings/tool_settings.py:2101
qint64 get_clipboard_window_opacity() const { return qsettings.value("clipboard/window_opacity", app_defaults_.value("clipboard_window_opacity")).toLongLong(); }
// main/settings/tool_settings.py:2105
void set_clipboard_window_opacity(const QVariant &value) { qsettings.setValue("clipboard/window_opacity", value); }
// main/settings/tool_settings.py:2109
qint64 get_clipboard_font_size() const { return qsettings.value("clipboard/font_size", app_defaults_.value("clipboard_font_size")).toLongLong(); }
// main/settings/tool_settings.py:2113
void set_clipboard_font_size(const QVariant &value) { qsettings.setValue("clipboard/font_size", value); }
// main/settings/tool_settings.py:2117
QVariant get_clipboard_font_size_options() const { return app_defaults_.value("clipboard_font_size_options"); }
// main/settings/tool_settings.py:2136
bool get_clipboard_show_metadata() const { return qsettings.value("clipboard/show_metadata", app_defaults_.value("clipboard_show_metadata")).toBool(); }
// main/settings/tool_settings.py:2142
void set_clipboard_show_metadata(const QVariant &value) { qsettings.setValue("clipboard/show_metadata", value); }
// main/settings/tool_settings.py:2146
bool get_clipboard_preserve_search() const { return qsettings.value("clipboard/preserve_search", app_defaults_.value("clipboard_preserve_search")).toBool(); }
// main/settings/tool_settings.py:2152
void set_clipboard_preserve_search(const QVariant &value) { qsettings.setValue("clipboard/preserve_search", value); }
// main/settings/tool_settings.py:2156
QVariant get_clipboard_window_opacity_options() const { return app_defaults_.value("clipboard_window_opacity_options"); }
// main/settings/tool_settings.py:2160
qint64 get_clipboard_line_height_padding() const { return qsettings.value("clipboard/line_height_padding", app_defaults_.value("clipboard_line_height_padding")).toLongLong(); }
// main/settings/tool_settings.py:2170
bool get_clipboard_move_to_top_on_paste() const { return qsettings.value("clipboard/move_to_top_on_paste", true).toBool(); }
// main/settings/tool_settings.py:2174
void set_clipboard_move_to_top_on_paste(const QVariant &value) { qsettings.setValue("clipboard/move_to_top_on_paste", value); }
// main/settings/tool_settings.py:2207
bool get_clipboard_multi_paste_keep_merged() const { return qsettings.value("clipboard/multi_paste_keep_merged", app_defaults_.value("clipboard_multi_paste_keep_merged")).toBool(); }
// main/settings/tool_settings.py:2218
QString get_clipboard_theme() const { return qsettings.value("clipboard/theme", app_defaults_.value("clipboard_theme")).toString(); }
// main/settings/tool_settings.py:2222
void set_clipboard_theme(const QVariant &value) { qsettings.setValue("clipboard/theme", value); }
// main/settings/tool_settings.py:2239
qint64 get_gif_fps() const { return qsettings.value("gif/fps", app_defaults_.value("gif_fps")).toLongLong(); }
// main/settings/tool_settings.py:2243
void set_gif_fps(const QVariant &value) { qsettings.setValue("gif/fps", value); }
// main/settings/tool_settings.py:2247
QVariant get_gif_fps_options() const { return app_defaults_.value("gif_fps_options"); }
QVariantMap generated_accessor_values() const {
    QVariantMap result;
    result["get_hotkey_2"] = get_hotkey_2();
    result["get_clipboard_hotkey_2"] = get_clipboard_hotkey_2();
    result["get_pin_clipboard_hotkey"] = get_pin_clipboard_hotkey();
    result["get_pin_clipboard_hotkey_2"] = get_pin_clipboard_hotkey_2();
    result["get_translation_hotkey"] = get_translation_hotkey();
    result["get_translation_hotkey_2"] = get_translation_hotkey_2();
    result["get_inapp_cursor_move_mode"] = get_inapp_cursor_move_mode();
    result["get_inapp_clipboard_pick_mode"] = get_inapp_clipboard_pick_mode();
    result["get_smart_selection"] = get_smart_selection();
    result["get_smart_selection_animation"] = get_smart_selection_animation();
    result["get_double_click_copy_close_enabled"] = get_double_click_copy_close_enabled();
    result["get_cross_tool_selection_enabled"] = get_cross_tool_selection_enabled();
    result["get_text_always_on_top_enabled"] = get_text_always_on_top_enabled();
    result["get_ocr_copy_directly_enabled"] = get_ocr_copy_directly_enabled();
    result["get_barcode_copy_single_enabled"] = get_barcode_copy_single_enabled();
    result["get_log_enabled"] = get_log_enabled();
    result["get_log_dir"] = get_log_dir();
    result["get_log_level"] = get_log_level();
    result["get_log_retention_days"] = get_log_retention_days();
    result["get_long_stitch_engine"] = get_long_stitch_engine();
    result["get_long_stitch_ignore_top_pixels"] = get_long_stitch_ignore_top_pixels();
    result["get_screenshot_save_enabled"] = get_screenshot_save_enabled();
    result["get_screenshot_pin_save_enabled"] = get_screenshot_pin_save_enabled();
    result["get_screenshot_save_original_enabled"] = get_screenshot_save_original_enabled();
    result["get_screenshot_format"] = get_screenshot_format();
    result["get_clipboard_file_reference_enabled"] = get_clipboard_file_reference_enabled();
    result["get_screenshot_watermark_auto"] = get_screenshot_watermark_auto();
    result["get_screenshot_watermark_last_on"] = get_screenshot_watermark_last_on();
    result["get_screenshot_quality"] = get_screenshot_quality();
    result["get_show_main_window"] = get_show_main_window();
    result["get_ocr_enabled"] = get_ocr_enabled();
    result["get_translation_on_top"] = get_translation_on_top();
    result["get_ocr_result_on_top"] = get_ocr_result_on_top();
    result["get_ocr_remove_spaces"] = get_ocr_remove_spaces();
    result["get_ocr_copy_on_close"] = get_ocr_copy_on_close();
    result["get_ocr_grayscale_enabled"] = get_ocr_grayscale_enabled();
    result["get_pin_auto_toolbar"] = get_pin_auto_toolbar();
    result["get_pin_default_opacity"] = get_pin_default_opacity();
    result["get_ocr_upscale_enabled"] = get_ocr_upscale_enabled();
    result["get_ocr_upscale_factor"] = get_ocr_upscale_factor();
    result["get_translation_provider"] = get_translation_provider();
    result["get_deepl_api_key"] = get_deepl_api_key();
    result["get_deepl_use_pro"] = get_deepl_use_pro();
    result["get_amazon_translate_region"] = get_amazon_translate_region();
    result["get_amazon_translate_access_key_id"] = get_amazon_translate_access_key_id();
    result["get_amazon_translate_secret_access_key"] = get_amazon_translate_secret_access_key();
    result["get_amazon_translate_session_token"] = get_amazon_translate_session_token();
    result["get_google_translate_api_key"] = get_google_translate_api_key();
    result["get_azure_translate_api_key"] = get_azure_translate_api_key();
    result["get_azure_translate_region"] = get_azure_translate_region();
    result["get_azure_translate_endpoint"] = get_azure_translate_endpoint();
    result["get_baidu_translate_appid"] = get_baidu_translate_appid();
    result["get_baidu_translate_secret_key"] = get_baidu_translate_secret_key();
    result["get_volcengine_translate_access_key"] = get_volcengine_translate_access_key();
    result["get_volcengine_translate_secret_key"] = get_volcengine_translate_secret_key();
    result["get_tencent_translate_secret_id"] = get_tencent_translate_secret_id();
    result["get_tencent_translate_secret_key"] = get_tencent_translate_secret_key();
    result["get_volcengine_translate_quota_enabled"] = get_volcengine_translate_quota_enabled();
    result["get_tencent_translate_quota_enabled"] = get_tencent_translate_quota_enabled();
    result["get_deepseek_api_key"] = get_deepseek_api_key();
    result["get_deepseek_model"] = get_deepseek_model();
    result["get_deepseek_base_url"] = get_deepseek_base_url();
    result["get_translation_split_sentences"] = get_translation_split_sentences();
    result["get_translation_preserve_formatting"] = get_translation_preserve_formatting();
    result["get_clipboard_enabled"] = get_clipboard_enabled();
    result["get_clipboard_auto_paste"] = get_clipboard_auto_paste();
    result["get_clipboard_close_after_paste"] = get_clipboard_close_after_paste();
    result["get_clipboard_history_limit"] = get_clipboard_history_limit();
    result["get_clipboard_db_path"] = get_clipboard_db_path();
    result["get_clipboard_auto_cleanup"] = get_clipboard_auto_cleanup();
    result["get_clipboard_window_opacity"] = get_clipboard_window_opacity();
    result["get_clipboard_font_size"] = get_clipboard_font_size();
    result["get_clipboard_font_size_options"] = get_clipboard_font_size_options();
    result["get_clipboard_show_metadata"] = get_clipboard_show_metadata();
    result["get_clipboard_preserve_search"] = get_clipboard_preserve_search();
    result["get_clipboard_window_opacity_options"] = get_clipboard_window_opacity_options();
    result["get_clipboard_line_height_padding"] = get_clipboard_line_height_padding();
    result["get_clipboard_move_to_top_on_paste"] = get_clipboard_move_to_top_on_paste();
    result["get_clipboard_multi_paste_keep_merged"] = get_clipboard_multi_paste_keep_merged();
    result["get_clipboard_theme"] = get_clipboard_theme();
    result["get_gif_fps"] = get_gif_fps();
    result["get_gif_fps_options"] = get_gif_fps_options();
    return result;
}
std::optional<QVariant> generated_accessor_value(const QString &name) const {
    if (name == "get_hotkey_2") return QVariant(get_hotkey_2());
    if (name == "get_clipboard_hotkey_2") return QVariant(get_clipboard_hotkey_2());
    if (name == "get_pin_clipboard_hotkey") return QVariant(get_pin_clipboard_hotkey());
    if (name == "get_pin_clipboard_hotkey_2") return QVariant(get_pin_clipboard_hotkey_2());
    if (name == "get_translation_hotkey") return QVariant(get_translation_hotkey());
    if (name == "get_translation_hotkey_2") return QVariant(get_translation_hotkey_2());
    if (name == "get_inapp_cursor_move_mode") return QVariant(get_inapp_cursor_move_mode());
    if (name == "get_inapp_clipboard_pick_mode") return QVariant(get_inapp_clipboard_pick_mode());
    if (name == "get_smart_selection") return QVariant(get_smart_selection());
    if (name == "get_smart_selection_animation") return QVariant(get_smart_selection_animation());
    if (name == "get_double_click_copy_close_enabled") return QVariant(get_double_click_copy_close_enabled());
    if (name == "get_cross_tool_selection_enabled") return QVariant(get_cross_tool_selection_enabled());
    if (name == "get_text_always_on_top_enabled") return QVariant(get_text_always_on_top_enabled());
    if (name == "get_ocr_copy_directly_enabled") return QVariant(get_ocr_copy_directly_enabled());
    if (name == "get_barcode_copy_single_enabled") return QVariant(get_barcode_copy_single_enabled());
    if (name == "get_log_enabled") return QVariant(get_log_enabled());
    if (name == "get_log_dir") return QVariant(get_log_dir());
    if (name == "get_log_level") return QVariant(get_log_level());
    if (name == "get_log_retention_days") return QVariant(get_log_retention_days());
    if (name == "get_long_stitch_engine") return QVariant(get_long_stitch_engine());
    if (name == "get_long_stitch_ignore_top_pixels") return QVariant(get_long_stitch_ignore_top_pixels());
    if (name == "get_screenshot_save_enabled") return QVariant(get_screenshot_save_enabled());
    if (name == "get_screenshot_pin_save_enabled") return QVariant(get_screenshot_pin_save_enabled());
    if (name == "get_screenshot_save_original_enabled") return QVariant(get_screenshot_save_original_enabled());
    if (name == "get_screenshot_format") return QVariant(get_screenshot_format());
    if (name == "get_clipboard_file_reference_enabled") return QVariant(get_clipboard_file_reference_enabled());
    if (name == "get_screenshot_watermark_auto") return QVariant(get_screenshot_watermark_auto());
    if (name == "get_screenshot_watermark_last_on") return QVariant(get_screenshot_watermark_last_on());
    if (name == "get_screenshot_quality") return QVariant(get_screenshot_quality());
    if (name == "get_show_main_window") return QVariant(get_show_main_window());
    if (name == "get_ocr_enabled") return QVariant(get_ocr_enabled());
    if (name == "get_translation_on_top") return QVariant(get_translation_on_top());
    if (name == "get_ocr_result_on_top") return QVariant(get_ocr_result_on_top());
    if (name == "get_ocr_remove_spaces") return QVariant(get_ocr_remove_spaces());
    if (name == "get_ocr_copy_on_close") return QVariant(get_ocr_copy_on_close());
    if (name == "get_ocr_grayscale_enabled") return QVariant(get_ocr_grayscale_enabled());
    if (name == "get_pin_auto_toolbar") return QVariant(get_pin_auto_toolbar());
    if (name == "get_pin_default_opacity") return QVariant(get_pin_default_opacity());
    if (name == "get_ocr_upscale_enabled") return QVariant(get_ocr_upscale_enabled());
    if (name == "get_ocr_upscale_factor") return QVariant(get_ocr_upscale_factor());
    if (name == "get_translation_provider") return QVariant(get_translation_provider());
    if (name == "get_deepl_api_key") return QVariant(get_deepl_api_key());
    if (name == "get_deepl_use_pro") return QVariant(get_deepl_use_pro());
    if (name == "get_amazon_translate_region") return QVariant(get_amazon_translate_region());
    if (name == "get_amazon_translate_access_key_id") return QVariant(get_amazon_translate_access_key_id());
    if (name == "get_amazon_translate_secret_access_key") return QVariant(get_amazon_translate_secret_access_key());
    if (name == "get_amazon_translate_session_token") return QVariant(get_amazon_translate_session_token());
    if (name == "get_google_translate_api_key") return QVariant(get_google_translate_api_key());
    if (name == "get_azure_translate_api_key") return QVariant(get_azure_translate_api_key());
    if (name == "get_azure_translate_region") return QVariant(get_azure_translate_region());
    if (name == "get_azure_translate_endpoint") return QVariant(get_azure_translate_endpoint());
    if (name == "get_baidu_translate_appid") return QVariant(get_baidu_translate_appid());
    if (name == "get_baidu_translate_secret_key") return QVariant(get_baidu_translate_secret_key());
    if (name == "get_volcengine_translate_access_key") return QVariant(get_volcengine_translate_access_key());
    if (name == "get_volcengine_translate_secret_key") return QVariant(get_volcengine_translate_secret_key());
    if (name == "get_tencent_translate_secret_id") return QVariant(get_tencent_translate_secret_id());
    if (name == "get_tencent_translate_secret_key") return QVariant(get_tencent_translate_secret_key());
    if (name == "get_volcengine_translate_quota_enabled") return QVariant(get_volcengine_translate_quota_enabled());
    if (name == "get_tencent_translate_quota_enabled") return QVariant(get_tencent_translate_quota_enabled());
    if (name == "get_deepseek_api_key") return QVariant(get_deepseek_api_key());
    if (name == "get_deepseek_model") return QVariant(get_deepseek_model());
    if (name == "get_deepseek_base_url") return QVariant(get_deepseek_base_url());
    if (name == "get_translation_split_sentences") return QVariant(get_translation_split_sentences());
    if (name == "get_translation_preserve_formatting") return QVariant(get_translation_preserve_formatting());
    if (name == "get_clipboard_enabled") return QVariant(get_clipboard_enabled());
    if (name == "get_clipboard_auto_paste") return QVariant(get_clipboard_auto_paste());
    if (name == "get_clipboard_close_after_paste") return QVariant(get_clipboard_close_after_paste());
    if (name == "get_clipboard_history_limit") return QVariant(get_clipboard_history_limit());
    if (name == "get_clipboard_db_path") return QVariant(get_clipboard_db_path());
    if (name == "get_clipboard_auto_cleanup") return QVariant(get_clipboard_auto_cleanup());
    if (name == "get_clipboard_window_opacity") return QVariant(get_clipboard_window_opacity());
    if (name == "get_clipboard_font_size") return QVariant(get_clipboard_font_size());
    if (name == "get_clipboard_font_size_options") return QVariant(get_clipboard_font_size_options());
    if (name == "get_clipboard_show_metadata") return QVariant(get_clipboard_show_metadata());
    if (name == "get_clipboard_preserve_search") return QVariant(get_clipboard_preserve_search());
    if (name == "get_clipboard_window_opacity_options") return QVariant(get_clipboard_window_opacity_options());
    if (name == "get_clipboard_line_height_padding") return QVariant(get_clipboard_line_height_padding());
    if (name == "get_clipboard_move_to_top_on_paste") return QVariant(get_clipboard_move_to_top_on_paste());
    if (name == "get_clipboard_multi_paste_keep_merged") return QVariant(get_clipboard_multi_paste_keep_merged());
    if (name == "get_clipboard_theme") return QVariant(get_clipboard_theme());
    if (name == "get_gif_fps") return QVariant(get_gif_fps());
    if (name == "get_gif_fps_options") return QVariant(get_gif_fps_options());
    return std::nullopt;
}
bool apply_generated_setter(const QString &name, const QVariant &value) {
    if (name == "set_hotkey_2") { set_hotkey_2(value); return true; }
    if (name == "set_clipboard_hotkey") { set_clipboard_hotkey(value); return true; }
    if (name == "set_clipboard_hotkey_2") { set_clipboard_hotkey_2(value); return true; }
    if (name == "set_pin_clipboard_hotkey") { set_pin_clipboard_hotkey(value); return true; }
    if (name == "set_pin_clipboard_hotkey_2") { set_pin_clipboard_hotkey_2(value); return true; }
    if (name == "set_translation_hotkey") { set_translation_hotkey(value); return true; }
    if (name == "set_translation_hotkey_2") { set_translation_hotkey_2(value); return true; }
    if (name == "set_inapp_cursor_move_mode") { set_inapp_cursor_move_mode(value); return true; }
    if (name == "set_inapp_clipboard_pick_mode") { set_inapp_clipboard_pick_mode(value); return true; }
    if (name == "set_smart_selection_animation") { set_smart_selection_animation(value); return true; }
    if (name == "set_double_click_copy_close_enabled") { set_double_click_copy_close_enabled(value); return true; }
    if (name == "set_cross_tool_selection_enabled") { set_cross_tool_selection_enabled(value); return true; }
    if (name == "set_text_always_on_top_enabled") { set_text_always_on_top_enabled(value); return true; }
    if (name == "set_ocr_copy_directly_enabled") { set_ocr_copy_directly_enabled(value); return true; }
    if (name == "set_barcode_copy_single_enabled") { set_barcode_copy_single_enabled(value); return true; }
    if (name == "set_log_enabled") { set_log_enabled(value); return true; }
    if (name == "set_log_dir") { set_log_dir(value); return true; }
    if (name == "set_log_level") { set_log_level(value); return true; }
    if (name == "set_log_retention_days") { set_log_retention_days(value); return true; }
    if (name == "set_long_stitch_engine") { set_long_stitch_engine(value); return true; }
    if (name == "set_long_stitch_ignore_top_pixels") { set_long_stitch_ignore_top_pixels(value); return true; }
    if (name == "set_screenshot_save_enabled") { set_screenshot_save_enabled(value); return true; }
    if (name == "set_screenshot_pin_save_enabled") { set_screenshot_pin_save_enabled(value); return true; }
    if (name == "set_screenshot_save_original_enabled") { set_screenshot_save_original_enabled(value); return true; }
    if (name == "set_clipboard_file_reference_enabled") { set_clipboard_file_reference_enabled(value); return true; }
    if (name == "set_show_main_window") { set_show_main_window(value); return true; }
    if (name == "set_ocr_enabled") { set_ocr_enabled(value); return true; }
    if (name == "set_pin_auto_toolbar") { set_pin_auto_toolbar(value); return true; }
    if (name == "set_ocr_grayscale_enabled") { set_ocr_grayscale_enabled(value); return true; }
    if (name == "set_ocr_upscale_enabled") { set_ocr_upscale_enabled(value); return true; }
    if (name == "set_ocr_upscale_factor") { set_ocr_upscale_factor(value); return true; }
    if (name == "set_deepl_api_key") { set_deepl_api_key(value); return true; }
    if (name == "set_deepl_use_pro") { set_deepl_use_pro(value); return true; }
    if (name == "set_translation_target_lang") { set_translation_target_lang(value); return true; }
    if (name == "set_translation_split_sentences") { set_translation_split_sentences(value); return true; }
    if (name == "set_translation_preserve_formatting") { set_translation_preserve_formatting(value); return true; }
    if (name == "set_clipboard_enabled") { set_clipboard_enabled(value); return true; }
    if (name == "set_clipboard_auto_paste") { set_clipboard_auto_paste(value); return true; }
    if (name == "set_clipboard_close_after_paste") { set_clipboard_close_after_paste(value); return true; }
    if (name == "set_clipboard_auto_cleanup") { set_clipboard_auto_cleanup(value); return true; }
    if (name == "set_clipboard_window_opacity") { set_clipboard_window_opacity(value); return true; }
    if (name == "set_clipboard_font_size") { set_clipboard_font_size(value); return true; }
    if (name == "set_clipboard_show_metadata") { set_clipboard_show_metadata(value); return true; }
    if (name == "set_clipboard_preserve_search") { set_clipboard_preserve_search(value); return true; }
    if (name == "set_clipboard_move_to_top_on_paste") { set_clipboard_move_to_top_on_paste(value); return true; }
    if (name == "set_clipboard_theme") { set_clipboard_theme(value); return true; }
    if (name == "set_gif_fps") { set_gif_fps(value); return true; }
    return false;
}
