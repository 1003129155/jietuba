#!/bin/bash
# Recognises and builds the tables of benchmark sets: bench_run.sh TAG DIR...
# Both engines; OCR only where DIR/lines_<engine>.jsonl is missing. Writes DIR/pred_TAG_<engine> with the models
# and DIR/pred_base_<engine> with the rules alone.
#   TOOL_DIR       build folder holding jietuba_table_tool.exe and jietuba_test_launcher.exe (built with the tests)
#   PPOCR_MODELS   folder with the PP-OCR models and onnxruntime.dll
set -e
: "${TOOL_DIR:?set TOOL_DIR}" "${PPOCR_MODELS:?set PPOCR_MODELS}"
# The launcher keeps a missing DLL from opening a system dialog.
TOOL() { "$TOOL_DIR/jietuba_test_launcher.exe" "$TOOL_DIR/jietuba_table_tool.exe" "$@"; }
TAG=$1; shift
for d in "$@"; do
  [ -f "$d/lines_one.jsonl" ] || TOOL ocr --engine oneocr --tag one "$d"
  [ -f "$d/lines_pp.jsonl" ] || TOOL ocr --engine ppocr --ppocr-models "$PPOCR_MODELS" --tag pp "$d"
  for e in one pp; do
    TOOL build --tag $e --pred "${TAG}_$e" --out "$d/trace.jsonl" "$d" > /dev/null
    TOOL build --rules --tag $e --pred "base_$e" --out "$d/trace.jsonl" "$d" > /dev/null
  done
  rm -f "$d/trace.jsonl"
  echo "$d done"
done
