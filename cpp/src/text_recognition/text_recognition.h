#pragma once
// Text recognition: recognizes the text in a screenshot selection (OCR) and only
// shows it, without translating.
// recognize_async recognizes in the background and reports (lines, reason key);
// shutdown_recognition waits for running threads before exit; show_text_recognition
// opens the result window (the toolbar button); copy_text_recognition copies
// directly without a window.
#include "text_recognition/recognizer.h"
#include "text_recognition/result_window.h"
