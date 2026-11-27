#!/bin/bash
# Trains the three table models in order and exports them into src/text_recognition/: pipeline.sh
#   DATA           folder holding the training sets below (outside the repository)
#   BUILD_DIR      CMake build folder (with the tests), rebuilt after each export
#   CONFIG         build configuration, default Release
#   PPOCR_MODELS   folder with the PP-OCR models and onnxruntime.dll
#   CMAKE          cmake, default cmake
# Training sets, each made once (python, with requirements.txt installed):
#   trc_web    collect_web.py --context --pages pages/train.json --per-page 4 DATA/trc_web
#   trc_old1   collect_web.py --context --pages pages/train_old1.json --per-page 4 DATA/trc_old1
#   trc_old2   collect_web.py --context --pages pages/train_old2.json --per-page 4 DATA/trc_old2
#   tr_syn_a   render_synthetic.py DATA/tr_syn_a 3001 400
#   tr_syn_b   render_synthetic.py DATA/tr_syn_b 13001 400
#   trc_syn_a  render_synthetic.py DATA/trc_syn_a 4001 400 --context
#   trc_syn_b  render_synthetic.py DATA/trc_syn_b 14001 400 --context
# Pages in pages/dev_test.json and pages/bench.json are kept for testing; never train on them.
# Each model is trained on traces of the tool built with the models before it: the caption candidates depend on the
# extent, and the row pairs on both.
set -e
: "${DATA:?set DATA}" "${BUILD_DIR:?set BUILD_DIR}" "${PPOCR_MODELS:?set PPOCR_MODELS}"
CONFIG=${CONFIG:-Release}
CMAKE=${CMAKE:-cmake}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../../src/text_recognition
TOOL_DIR=$BUILD_DIR/$CONFIG
WORK=$DATA/work
SETS="trc_web trc_old1 trc_old2 tr_syn_a tr_syn_b trc_syn_a trc_syn_b"
mkdir -p "$WORK"
# The launcher keeps a missing DLL from opening a system dialog.
TOOL() { "$TOOL_DIR/jietuba_test_launcher.exe" "$TOOL_DIR/jietuba_table_tool.exe" "$@"; }
BUILD() { "$CMAKE" --build "$BUILD_DIR" --config "$CONFIG" --target jietuba_table_tool > "$WORK/build.log"; }
TRAIN() { python "$HERE/train.py" "$@"; }
traces() {  # $1: name; one trace file per engine, keyed <set>/<image>
  for e in one pp; do TOOL build --tag $e --out "$WORK/$1_$e.jsonl" $(for d in $SETS; do echo "$DATA/$d"; done) > /dev/null; done
}
stage() {  # $1: kind, $2: .inc name
  traces "$1"
  local T="$WORK/$1_one.jsonl $WORK/$1_pp.jsonl"
  TRAIN fit --kind "$1" --root "$DATA" --traces $T --model "$WORK/$1_val.pkl" --val-frac 0.25
  TRAIN fit --kind "$1" --root "$DATA" --traces $T --model "$WORK/$1.pkl" --val-frac 0.25 --final | tail -1
  TRAIN export --model "$WORK/$1.pkl" --names "$WORK/names_$1.txt" --out "$SRC/$2"
  BUILD
}

BUILD
for d in $SETS; do
  [ -f "$DATA/$d/lines_one.jsonl" ] || TOOL ocr --engine oneocr --tag one "$DATA/$d"
  [ -f "$DATA/$d/lines_pp.jsonl" ] || TOOL ocr --engine ppocr --ppocr-models "$PPOCR_MODELS" --tag pp "$DATA/$d"
done
TOOL names extent > "$WORK/names_extent.txt"
TOOL names caption > "$WORK/names_caption.txt"
TOOL names > "$WORK/names_rows.txt"

stage extent table_extent_model.inc
stage caption table_caption_model.inc
stage rows table_row_model.inc

# The rebuilt tool must decide exactly as the pickled models do.
traces final
for kind in extent caption rows; do
  TRAIN check --kind $kind --model "$WORK/$kind.pkl" --traces "$WORK/final_one.jsonl" "$WORK/final_pp.jsonl"
done
