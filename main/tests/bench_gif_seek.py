"""Compare sequential skipping with direct seeking, using synthetic frames only.

Run with the locally rebuilt extension:
    python main/tests/bench_gif_seek.py --output assessment/gif_seek_results.json
"""

import argparse
import json
import os
from pathlib import Path
import statistics
import sys
import time

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gifrecorder
from PIL import Image, ImageDraw
from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QApplication

from gif.frame_recorder import FrameData
from gif.playback_engine import PlaybackEngine


class LegacyStore:
    """Force the compatibility path: start at zero, then skip the prefix."""

    def __init__(self, store):
        self.store = store

    def start_decoder(self, display_w, display_h, prefetch):
        return self.store.start_decoder(display_w, display_h, prefetch)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--samples", type=int, default=5)
    args = parser.parse_args()
    if "start_frame" not in (gifrecorder.FrameStore.start_decoder.__text_signature__ or ""):
        raise SystemExit("Build and install the local gifrecorder wheel first.")
    app = QApplication.instance() or QApplication([])
    store = gifrecorder.FrameStore(args.width, args.height, 16)
    image = Image.new("RGB", (args.width, args.height), (240, 240, 240))
    ImageDraw.Draw(image).rectangle((100, 90, 180, 180), fill=(240, 40, 30))
    raw = image.tobytes()
    frames = []
    for i in range(1001):
        store.push_rgb(raw, i * 62)
        frames.append(FrameData(elapsed_ms=i * 62, width=args.width, height=args.height))

    results = {"size": [args.width, args.height], "frames": len(frames),
               "samples": args.samples, "extension": gifrecorder.__file__, "results": []}
    for index in (100, 500, 1000):
        for mode in ("legacy_skip", "direct_start"):
            samples = []
            for _ in range(args.samples + 1):
                engine = PlaybackEngine()
                engine.load(frames, 16, LegacyStore(store) if mode == "legacy_skip" else store)
                engine.set_display_size(args.width, args.height)
                received = []
                heartbeat = []
                engine.frame_ready.connect(lambda image, idx, target=received: target.append(idx))
                try:
                    engine.play()
                    engine.stop_timer()
                    start = time.perf_counter()
                    QTimer.singleShot(
                        0, lambda target=heartbeat, t=start: target.append((time.perf_counter() - t) * 1000)
                    )
                    engine.seek(index)
                    call_ms = (time.perf_counter() - start) * 1000
                    app.processEvents()
                    deadline = time.perf_counter() + 5
                    while not received and time.perf_counter() < deadline:
                        engine._advance()
                        if not received:
                            time.sleep(0.001)
                    assert received == [index], received
                    samples.append({"call_ms": call_ms, "heartbeat_ms": heartbeat[0],
                                    "first_frame_ms": (time.perf_counter() - start) * 1000})
                finally:
                    engine.stop()
            samples = samples[1:]  # discard warmup
            row = {"mode": mode, "index": index,
                   **{key: round(statistics.median(s[key] for s in samples), 3)
                      for key in samples[0]}}
            results["results"].append(row)
            print(row)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(results, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
