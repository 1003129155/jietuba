# j-ppocr

PP-OCR (PaddleOCR) ONNX inference for Python, implemented in Rust with [`ort`](https://github.com/pykeio/ort) (ONNX Runtime).

- Detection (DBNet) + Recognition (CRNN/CTC), no OpenCV / numpy dependency on the Python side.
- Runs inference on native threads (no Python GIL contention).
- Models: PP-OCRv6 small ONNX (det + rec), recognition charset is read from the rec model's embedded `character` metadata.

The distribution is `j-ppocr`; the module imports as `ppocr_rust`.

Windows x86_64 or ARM64, CPython 3.11+ (abi3). Part of
[jietuba](https://github.com/1003129155/jietuba).

License: Apache-2.0

