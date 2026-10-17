# j-ppocr

PP-OCR (PaddleOCR) ONNX inference for Python, implemented in Rust with [`ort`](https://github.com/pykeio/ort) (ONNX Runtime).

- Detection (DBNet) + Recognition (CRNN/CTC), no OpenCV / numpy dependency on the Python side.
- Runs inference on native threads (no Python GIL contention).
- Models: PP-OCR v4/v5/v6 ONNX (det + rec). The recognition charset comes from the rec model's embedded `character` metadata or a separate dictionary file, and must match the model's output classes.
- Model management for a `models/` folder: the built-in download catalog, discovery of installed models, importing and validating user-supplied models, placing downloaded models, and removal.

The distribution is `j-ppocr`; the module imports as `ppocr_rust`.

Windows x86_64 or ARM64, CPython 3.11+ (abi3). Part of
[jietuba](https://github.com/1003129155/jietuba).

License: Apache-2.0

