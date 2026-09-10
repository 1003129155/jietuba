# ppocr_rust

PP-OCR (PaddleOCR) ONNX inference for Python, implemented in Rust with [`ort`](https://github.com/pykeio/ort) (ONNX Runtime).

- Detection (DBNet) + Recognition (CRNN/CTC), no OpenCV / numpy dependency on the Python side.
- Runs inference on native threads (no Python GIL contention).
- Models: PP-OCRv6 small ONNX (det + rec), recognition charset is read from the rec model's embedded `character` metadata.

License: Apache-2.0

