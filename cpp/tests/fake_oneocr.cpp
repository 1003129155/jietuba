// A stand-in oneocr.dll for the Snipping Tool binding tests. It exports the engine's
// functions with trivial bodies and reports one fixed line. Variants (one DLL each):
//   LEGACY: lacks OcrProcessOptionsSetMaxRecognitionLineCount, as some Snipping Tool versions do.
//   BROKEN: lacks RunOcrPipeline, a function the binding requires.
// FakeWordMode picks what the line's words are: none (the default), "fake" and "line", or the
// words with one failing call (see WordMode).
#include <cstdint>

#define EXPORT extern "C" __declspec(dllexport)

namespace {
struct BoundingBox {
    float x1, y1, x2, y2, x3, y3, x4, y4;
};
const BoundingBox LINE_BOX{10, 10, 110, 10, 110, 30, 10, 30};
const BoundingBox WORD_BOXES[2] = {{10, 10, 50, 10, 50, 30, 10, 30}, {60, 10, 110, 10, 110, 30, 60, 30}};
int64_t max_line_count = -1;
// 0: no words; 1: the two words; 2: the second word cannot be fetched; 3: the second word has no
// text; 4: the second word has no box.
int64_t word_mode = 0;
}

// What the last OcrProcessOptionsSetMaxRecognitionLineCount call set; -1 when never called.
EXPORT int64_t FakeMaxLineCount() { return max_line_count; }
EXPORT void FakeWordMode(int64_t mode) { word_mode = mode; }

EXPORT int64_t CreateOcrInitOptions(int64_t *options) {
    *options = 1;
    return 0;
}
EXPORT int64_t OcrInitOptionsSetUseModelDelayLoad(int64_t, char) { return 0; }
EXPORT int64_t CreateOcrPipeline(const char *, const char *, int64_t, int64_t *pipeline) {
    *pipeline = 2;
    return 0;
}
EXPORT int64_t CreateOcrProcessOptions(int64_t *options) {
    *options = 3;
    return 0;
}
#ifndef FAKE_ONEOCR_LEGACY
EXPORT int64_t OcrProcessOptionsSetMaxRecognitionLineCount(int64_t, int64_t count) {
    max_line_count = count;
    return 0;
}
#endif
#ifndef FAKE_ONEOCR_BROKEN
EXPORT int64_t RunOcrPipeline(int64_t, const void *, int64_t, int64_t *result) {
    *result = 4;
    return 0;
}
#endif
EXPORT int64_t GetOcrLineCount(int64_t, int64_t *count) {
    *count = 1;
    return 0;
}
EXPORT int64_t GetOcrLine(int64_t, int64_t, int64_t *line) {
    *line = 5;
    return 0;
}
EXPORT int64_t GetOcrLineContent(int64_t, const char **content) {
    *content = "fake line";
    return 0;
}
EXPORT int64_t GetOcrLineBoundingBox(int64_t, const BoundingBox **box) {
    *box = &LINE_BOX;
    return 0;
}
EXPORT int64_t GetOcrLineWordCount(int64_t, int64_t *count) {
    *count = word_mode == 0 ? 0 : 2;
    return 0;
}
EXPORT int64_t GetOcrWord(int64_t, int64_t index, int64_t *word) {
    if (word_mode == 2 && index == 1) return 1;
    *word = 6 + index;
    return 0;
}
EXPORT int64_t GetOcrWordContent(int64_t word, const char **content) {
    if (word_mode == 3 && word == 7) {
        *content = nullptr;
        return 0;
    }
    *content = word == 6 ? "fake" : "line";
    return 0;
}
EXPORT int64_t GetOcrWordBoundingBox(int64_t word, const BoundingBox **box) {
    if (word_mode == 4 && word == 7) return 1;
    *box = &WORD_BOXES[word - 6];
    return 0;
}
EXPORT int64_t GetOcrWordConfidence(int64_t, float *confidence) {
    *confidence = 1.0f;
    return 0;
}
EXPORT void ReleaseOcrResult(int64_t) {}
EXPORT void ReleaseOcrInitOptions(int64_t) {}
EXPORT void ReleaseOcrPipeline(int64_t) {}
EXPORT void ReleaseOcrProcessOptions(int64_t) {}
