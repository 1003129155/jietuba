#pragma once
// C ABI of native/ (the reference Rust libraries); see native/src for the contracts.
#include <cstddef>
#include <cstdint>

extern "C" {
// A panic returns the export's failure value; its report goes here (null: stderr).
void jb_set_panic_handler(void (*handler)(const char *report));

struct JbInputHub;

enum JbInputEventKind : int32_t {
    JB_EVENT_GESTURE = 0,
    JB_EVENT_MOVED = 1,
    JB_EVENT_SIDE = 2,
    JB_EVENT_HOTKEY = 3,
    JB_EVENT_WHEEL = 4,
    JB_EVENT_KEY = 5,
    JB_EVENT_FOREGROUND = 6,
    JB_EVENT_FAILURE = 7,
    JB_EVENT_CAPTURED = 8,
};

struct JbInputEvent {
    int32_t kind;
    uint64_t id;
    int32_t x;
    int32_t y;
    int32_t value;
    bool flag;
    intptr_t hwnd;
    char text[512];
};

JbInputHub *jb_input_hub_new(char *error, size_t error_len);
void jb_input_hub_free(JbInputHub *hub);
void jb_input_hub_close(const JbInputHub *hub);
bool jb_input_hub_next_event(const JbInputHub *hub, int64_t timeout_ms, JbInputEvent *out);
bool jb_input_hub_configure_gestures(const JbInputHub *hub, const char *const *modifiers, const char *const *buttons, size_t count, bool enabled);
void jb_input_hub_set_blocked(const JbInputHub *hub, bool blocked);
void jb_input_hub_cancel_gesture(const JbInputHub *hub);
bool jb_input_hub_take_position(const JbInputHub *hub, uint64_t gesture_id, int32_t *x, int32_t *y);
bool jb_input_hub_dragging(const JbInputHub *hub);
bool jb_input_hub_accepts(const JbInputHub *hub, uint64_t gesture_id);
bool jb_input_hub_gesture_binding(const JbInputHub *hub, uint64_t gesture_id, char *modifiers, size_t modifiers_len, char *button, size_t button_len);
bool jb_input_hub_configure_side_buttons(const JbInputHub *hub, bool enabled, const char *const *suppressed, size_t count, bool capture_all);
int32_t jb_input_hub_bind_hotkey(const JbInputHub *hub, const char *name, const char *modifiers, uint32_t vk);
void jb_input_hub_unbind_hotkey(const JbInputHub *hub, const char *name);
void jb_input_hub_configure_key_capture(const JbInputHub *hub, bool enabled);
void jb_input_hub_watch_wheel(const JbInputHub *hub, const char *watcher, bool has_rect, int32_t left, int32_t top, int32_t right, int32_t bottom);
void jb_input_hub_unwatch_wheel(const JbInputHub *hub, const char *watcher);
void jb_input_hub_watch_keys(const JbInputHub *hub, const char *watcher, const uint32_t *keys, size_t count);
void jb_input_hub_unwatch_keys(const JbInputHub *hub, const char *watcher);
void jb_input_hub_watch_foreground(const JbInputHub *hub, const char *watcher);
void jb_input_hub_unwatch_foreground(const JbInputHub *hub, const char *watcher);
bool jb_input_hub_hooks_installed(const JbInputHub *hub);
void jb_input_hub_set_test_marker(const JbInputHub *hub, size_t marker);

// PP-OCR. ONNX Runtime is loaded at run time from the PP-OCR pack (jb_ppocr_init_runtime).
struct JbPpocr;
struct JbOcrLine {
    double points[8];
    char *text;
    double score;
};
bool jb_ppocr_init_runtime(const char *dll_path, char *error, size_t error_len);
// dict_path may be null: the dictionary then comes from the rec model's metadata.
JbPpocr *jb_ppocr_open(const char *det_path, const char *rec_path, const char *dict_path, char *error, size_t error_len);
void jb_ppocr_free(JbPpocr *engine);
void jb_ppocr_close(const JbPpocr *engine);
bool jb_ppocr_recognize(const JbPpocr *engine, const uint8_t *data, size_t len, size_t w, size_t h, size_t stride, JbOcrLine **lines, size_t *count,
                        char *error, size_t error_len);
void jb_ppocr_lines_free(JbOcrLine *lines, size_t count);
// PP-OCR models in a models/ folder (ppocr_rust::models). Lists and models are JSON strings
// freed with jb_ppocr_text_free; a null return means none, or failure with error set.
void jb_ppocr_text_free(char *text);
char *jb_ppocr_models_catalog();
char *jb_ppocr_models_discover(const char *root);
char *jb_ppocr_models_resolve(const char *root, const char *id);
char *jb_ppocr_models_auto_choice(const char *root);
char *jb_ppocr_models_import(const char *root, const char *name, const char *det_path, const char *rec_path, const char *dict_path, char *error,
                             size_t error_len);
char *jb_ppocr_models_finish_download(const char *root, const char *catalog_id, char *error, size_t error_len);
bool jb_ppocr_models_finish_runtime_download(const char *root, char *error, size_t error_len);
bool jb_ppocr_models_remove(const char *root, const char *id, char *error, size_t error_len);
void jb_ppocr_models_clean_staging(const char *root);

// longstitch's incremental StitchSession.
struct JbStitchSession;
struct JbPushResult {
    size_t head_cut;
    size_t head_rows;
    size_t keep;
    size_t skip;
    size_t height;
    size_t top;
};
JbStitchSession *jb_stitch_session_new(uint32_t width, uint32_t height, uint32_t ignore_right_pixels, uint32_t ignore_top_pixels, float min_overlap_ratio,
                                       char *error, size_t error_len);
void jb_stitch_session_free(JbStitchSession *session);
uint32_t jb_stitch_session_width(const JbStitchSession *session);
size_t jb_stitch_session_height(const JbStitchSession *session);
// 0 down, 1 up, -1 none.
int32_t jb_stitch_session_last_move(const JbStitchSession *session);
// heading: 0 down, 1 up, -1 unknown. Returns 1 pushed, 0 no overlap, -1 error.
int32_t jb_stitch_session_push(JbStitchSession *session, const uint8_t *bgra, size_t len, int32_t heading, float ignore_img1_top_ratio,
                               float ignore_img1_bottom_ratio, bool debug, JbPushResult *result, char *error, size_t error_len);
size_t jb_stitch_session_crop_top(JbStitchSession *session);
size_t jb_stitch_session_crop_bottom(JbStitchSession *session);
size_t jb_stitch_session_byte_len(const JbStitchSession *session);
bool jb_stitch_session_export(const JbStitchSession *session, uint8_t *out, size_t len);
void jb_stitch_session_close(JbStitchSession *session);

// hdrcapture: DXGI desktop duplication with tone mapping to sRGB BGRA8. A capture is
// used only on the thread that created it.
struct JbHdrCapture;
struct JbHdrMonitor {
    size_t index;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    bool is_virtual_desktop;
    bool hdr_enabled;
    bool hdr_supported;
};
struct JbHdrFrameInfo {
    size_t index;
    bool has_peak;
    float tone_map_peak;
};
struct JbHdrFrame {
    uint8_t *bgra;
    size_t len;
    uint32_t width;
    uint32_t height;
    JbHdrFrameInfo *infos;
    size_t info_count;
};
// Error classes returned by the grab functions (0 is success).
enum JbHdrError : int32_t {
    JB_HDR_OK = 0,
    JB_HDR_CAPTURE_ERROR = 1,
    JB_HDR_INITIAL_FRAME_TIMEOUT = 2,
    JB_HDR_ACCESS_LOST = 3,
    JB_HDR_DIMENSIONS_CHANGED = 4,
    JB_HDR_INVALID_MONITOR_INDEX = 5,
};
JbHdrCapture *jb_hdr_capture_new(uint32_t timeout_ms, char *error, size_t error_len, int32_t *error_kind);
void jb_hdr_capture_free(JbHdrCapture *capture);
uint32_t jb_hdr_capture_timeout_ms(const JbHdrCapture *capture);
size_t jb_hdr_capture_monitors(const JbHdrCapture *capture, JbHdrMonitor *out, size_t capacity);
// timeout_ms below zero uses the capture's default.
int32_t jb_hdr_capture_grab(JbHdrCapture *capture, size_t monitor_index, int64_t timeout_ms, bool adaptive, JbHdrFrame *out, char *error, size_t error_len);
int32_t jb_hdr_capture_grab_region(JbHdrCapture *capture, int32_t x, int32_t y, uint32_t width, uint32_t height, int64_t timeout_ms, bool adaptive,
                                   JbHdrFrame *out, char *error, size_t error_len);
void jb_hdr_frame_free(JbHdrFrame *frame);
bool jb_hdr_displays(JbHdrMonitor *out, size_t capacity, size_t *count, char *error, size_t error_len);

// gifrecorder: JPEG frame store, background playback decoder and capture session.
struct JbFrameStore;
struct JbFrameDecoder;
struct JbRecordSession;
struct JbSprite {
    const uint8_t *data;
    size_t len;
    uint32_t w;
    uint32_t h;
};
struct JbCursorSprites {
    JbSprite cursor;
    JbSprite burst_left[3];
    JbSprite burst_right[3];
    JbSprite scroll_up;
    JbSprite scroll_down;
};
struct JbCursorInfo {
    bool present;
    int32_t x;
    int32_t y;
    uint8_t press;
    int8_t scroll;
    uint8_t burst_frame;
    uint8_t burst_side;
};
typedef bool (*JbProgressCallback)(size_t current, size_t total, void *user);
void jb_gif_bytes_free(uint8_t *ptr, size_t len);
JbFrameStore *jb_frame_store_new(uint32_t width, uint32_t height, uint32_t fps, int32_t jpeg_quality, size_t max_frames, size_t max_memory_bytes);
void jb_frame_store_free(JbFrameStore *store);
uint32_t jb_frame_store_width(const JbFrameStore *store);
uint32_t jb_frame_store_height(const JbFrameStore *store);
uint32_t jb_frame_store_fps(const JbFrameStore *store);
size_t jb_frame_store_frame_count(const JbFrameStore *store);
uint64_t jb_frame_store_memory_usage_bytes(const JbFrameStore *store);
uint64_t jb_frame_store_dropped_frames(const JbFrameStore *store);
uint32_t jb_frame_store_total_duration_ms(const JbFrameStore *store);
size_t jb_frame_store_frame_timestamps(const JbFrameStore *store, uint32_t *out, size_t capacity);
bool jb_frame_store_get_elapsed_ms(const JbFrameStore *store, size_t index, uint32_t *out);
bool jb_frame_store_is_recording(const JbFrameStore *store);
bool jb_frame_store_is_paused(const JbFrameStore *store);
bool jb_frame_store_set_state(const JbFrameStore *store, uint8_t state);
uint8_t jb_frame_store_state(const JbFrameStore *store);
int32_t jb_frame_store_push_bgra(const JbFrameStore *store, const uint8_t *bgra, size_t len, uint32_t elapsed_ms, char *error, size_t error_len);
int32_t jb_frame_store_push_rgb(const JbFrameStore *store, const uint8_t *rgb, size_t len, uint32_t elapsed_ms, char *error, size_t error_len);
bool jb_frame_store_get_frame_rgb(const JbFrameStore *store, size_t index, uint32_t display_w, uint32_t display_h, uint8_t **out, size_t *out_len, char *error,
                                  size_t error_len);
JbFrameDecoder *jb_frame_store_start_decoder(const JbFrameStore *store, uint32_t display_w, uint32_t display_h, size_t prefetch, size_t start_frame);
bool jb_frame_store_export_gif(const JbFrameStore *store, const char *path, uint32_t width, uint32_t height, uint16_t repeat, size_t frame_start, size_t frame_end,
                               JbProgressCallback progress, void *progress_user, const JbCursorSprites *sprites, const JbCursorInfo *infos, size_t info_count,
                               float speed, char *error, size_t error_len);
void jb_frame_store_cancel_export(const JbFrameStore *store);
void jb_frame_store_clear(const JbFrameStore *store);
void jb_frame_decoder_free(JbFrameDecoder *decoder);
// 1 with a frame, 0 when all frames are taken (or none ready for try_), -1 once stopped.
int32_t jb_frame_decoder_next_frame(JbFrameDecoder *decoder, uint8_t **out, size_t *out_len, uint32_t *elapsed_ms);
int32_t jb_frame_decoder_try_next_frame(JbFrameDecoder *decoder, uint8_t **out, size_t *out_len, uint32_t *elapsed_ms);
size_t jb_frame_decoder_buffered_count(const JbFrameDecoder *decoder);
bool jb_frame_decoder_is_finished(const JbFrameDecoder *decoder);
size_t jb_frame_decoder_total_frames(const JbFrameDecoder *decoder);
size_t jb_frame_decoder_fetched_count(const JbFrameDecoder *decoder);
size_t jb_frame_decoder_skip(JbFrameDecoder *decoder, size_t n);
void jb_frame_decoder_stop(JbFrameDecoder *decoder);
JbRecordSession *jb_record_session_new(const JbFrameStore *store, int32_t left, int32_t top, int32_t width, int32_t height, uint32_t fps, bool prefer_dxgi,
                                       char *error, size_t error_len);
JbRecordSession *jb_record_session_prepare(bool prefer_dxgi);
bool jb_record_session_begin(JbRecordSession *session, const JbFrameStore *store, int32_t left, int32_t top, int32_t width, int32_t height, uint32_t fps,
                             char *error, size_t error_len);
bool jb_record_session_pause(const JbRecordSession *session);
bool jb_record_session_resume(const JbRecordSession *session);
void jb_record_session_stop(JbRecordSession *session);
// 1 dxgi, 2 gdi, 0 before any frame.
int32_t jb_record_session_backend(const JbRecordSession *session);
uint8_t jb_record_session_state(const JbRecordSession *session);
bool jb_record_session_is_stopped(const JbRecordSession *session);
void jb_record_session_free(JbRecordSession *session);
// video_recorder: MP4 recording with Media Foundation; run once, controls from any thread.
struct JbVideoRecorder;
typedef bool (*JbVideoEventCallback)(const char *name, const char *json, void *user);
JbVideoRecorder *jb_video_recorder_new(const char *output, int32_t left, int32_t top, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
                                       bool system_audio, bool hardware, bool cursor, bool prefer_dxgi);
void jb_video_recorder_free(JbVideoRecorder *recorder);
void jb_video_recorder_pause(const JbVideoRecorder *recorder);
void jb_video_recorder_resume(const JbVideoRecorder *recorder);
void jb_video_recorder_stop(const JbVideoRecorder *recorder);
void jb_video_recorder_cancel(const JbVideoRecorder *recorder);
bool jb_video_recorder_run(const JbVideoRecorder *recorder, JbVideoEventCallback callback, void *user, char *code, size_t code_len, char *message,
                           size_t message_len);
// pyclipboard: history database, watcher thread and writing items back. Items and groups
// are JSON; returned text, bytes and id lists are freed by their _free function.
struct JbClipboard;
typedef void (*JbClipboardItemCallback)(const char *item_json, void *user);
JbClipboard *jb_clipboard_open(const char *db_path, char *error, size_t error_len);
void jb_clipboard_free(JbClipboard *clipboard);
void jb_clipboard_text_free(char *text);
void jb_clipboard_bytes_free(uint8_t *data, size_t len);
void jb_clipboard_ids_free(int64_t *ids, size_t len);
uint64_t jb_clipboard_max_stitched_pixels();
bool jb_clipboard_filter_content_type(int64_t group_type, char *out, size_t out_len);
bool jb_clipboard_group_accepts(int64_t group_type, const char *content_type);
bool jb_clipboard_set_text(const char *text, char *error, size_t error_len);
char *jb_clipboard_db_path(const JbClipboard *clipboard);
char *jb_clipboard_images_dir(const JbClipboard *clipboard);
void jb_clipboard_set_history_limit(const JbClipboard *clipboard, int64_t limit);
int64_t jb_clipboard_history_limit(const JbClipboard *clipboard);
bool jb_clipboard_start_monitor(const JbClipboard *clipboard, JbClipboardItemCallback callback, void *user, char *error, size_t error_len);
void jb_clipboard_stop_monitor(const JbClipboard *clipboard);
bool jb_clipboard_is_monitoring(const JbClipboard *clipboard);
bool jb_clipboard_pause_for_update(const JbClipboard *clipboard);
void jb_clipboard_resume_after_update(const JbClipboard *clipboard);
size_t jb_clipboard_pending_writes(const JbClipboard *clipboard);
bool jb_clipboard_get_history(const JbClipboard *clipboard, int64_t offset, int64_t limit, const char *search, const char *content_type, const int64_t *start_time,
                              const int64_t *end_time, char **out_json, char *error, size_t error_len);
bool jb_clipboard_get_count(const JbClipboard *clipboard, int64_t *out, char *error, size_t error_len);
bool jb_clipboard_get_item(const JbClipboard *clipboard, int64_t id, char **out_json, char *error, size_t error_len);
bool jb_clipboard_delete_item(const JbClipboard *clipboard, int64_t id, char *error, size_t error_len);
bool jb_clipboard_clear_history(const JbClipboard *clipboard, bool keep_grouped, char *error, size_t error_len);
bool jb_clipboard_clear_unpinned_ungrouped(const JbClipboard *clipboard, bool vacuum, char *error, size_t error_len);
bool jb_clipboard_toggle_pin(const JbClipboard *clipboard, int64_t id, bool *out_pinned, char *error, size_t error_len);
bool jb_clipboard_add_item(const JbClipboard *clipboard, const char *content, const char *content_type, const char *title, int64_t *out_id, char *error,
                           size_t error_len);
bool jb_clipboard_update_item(const JbClipboard *clipboard, int64_t id, const char *content, const char *title, char *error, size_t error_len);
bool jb_clipboard_move_item_between(const JbClipboard *clipboard, int64_t id, const int64_t *before_id, const int64_t *after_id, char *error, size_t error_len);
bool jb_clipboard_create_group(const JbClipboard *clipboard, const char *name, const char *color, const char *icon, int64_t group_type, int64_t *out_id,
                               char *error, size_t error_len);
bool jb_clipboard_get_groups(const JbClipboard *clipboard, char **out_json, char *error, size_t error_len);
bool jb_clipboard_delete_group(const JbClipboard *clipboard, int64_t id, char *error, size_t error_len);
bool jb_clipboard_rename_group(const JbClipboard *clipboard, int64_t id, const char *name, char *error, size_t error_len);
bool jb_clipboard_update_group(const JbClipboard *clipboard, int64_t id, const char *name, const char *color, const char *icon, int64_t group_type, char *error,
                               size_t error_len);
bool jb_clipboard_move_to_group(const JbClipboard *clipboard, int64_t item_id, const int64_t *group_id, char *error, size_t error_len);
bool jb_clipboard_move_group_between(const JbClipboard *clipboard, int64_t id, const int64_t *before_id, const int64_t *after_id, char *error, size_t error_len);
bool jb_clipboard_get_by_group(const JbClipboard *clipboard, const int64_t *group_id, int64_t offset, int64_t limit, char **out_json, char *error,
                               size_t error_len);
bool jb_clipboard_paste_item(const JbClipboard *clipboard, int64_t id, bool with_html, bool move_to_top, bool *out_ok, char *error, size_t error_len);
bool jb_clipboard_paste_items(const JbClipboard *clipboard, const int64_t *ids, size_t count, bool with_html, bool move_to_top, const char *separator,
                              bool plain_text, const char *layout, bool keep_in_history, int64_t **out_ids, size_t *out_count, char *error, size_t error_len);
bool jb_clipboard_get_image_data(const JbClipboard *clipboard, const char *image_id, uint8_t **out, size_t *out_len, char *error, size_t error_len);
}
