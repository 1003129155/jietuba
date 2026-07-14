# -*- coding: utf-8 -*-
"""Real-world timing check for the Rust clipboard watcher's lifecycle.

This is deliberately opt-in: it starts the real Windows clipboard watcher and
must therefore not run as part of the normal unit-test suite.

Run from ``main/tests``::

    $env:RUN_CLIPBOARD_MONITOR_BENCHMARK = "1"
    ..\\..\\venv311\\Scripts\\python.exe -m pytest test_clipboard_monitoring_performance.py -s

Optional environment variables:

* ``CLIPBOARD_MONITOR_BENCHMARK_SAMPLES``: number of start/stop cycles
  (default: 20).
* ``CLIPBOARD_MONITOR_MAX_STOP_MS``: fail when the slowest stop exceeds this
  budget.  It is intentionally opt-in because actual clipboard latency varies
  with applications such as Office and image-heavy clipboard content.
"""

from __future__ import annotations

import os
import platform
import statistics
import time

import pytest


pytestmark = pytest.mark.clipboard_monitor_benchmark


def _percentile(samples: list[float], percentile: float) -> float:
    """Return an interpolated percentile without adding a benchmark package."""
    ordered = sorted(samples)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * percentile / 100
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def _format_stats(label: str, samples: list[float]) -> str:
    return (
        f"{label}: min={min(samples):.1f} ms, "
        f"p50={_percentile(samples, 50):.1f} ms, "
        f"p95={_percentile(samples, 95):.1f} ms, "
        f"max={max(samples):.1f} ms, "
        f"mean={statistics.mean(samples):.1f} ms"
    )


@pytest.mark.skipif(platform.system() != "Windows", reason="Windows clipboard watcher required")
def test_measure_real_clipboard_monitor_start_stop(tmp_path):
    """Measure call latency for a fully-started watcher over repeated cycles."""
    if os.environ.get("RUN_CLIPBOARD_MONITOR_BENCHMARK") != "1":
        pytest.skip("set RUN_CLIPBOARD_MONITOR_BENCHMARK=1 to run the real watcher benchmark")

    pyclipboard = pytest.importorskip("pyclipboard")
    sample_count = max(1, int(os.environ.get("CLIPBOARD_MONITOR_BENCHMARK_SAMPLES", "20")))
    # start_monitor returns after spawning a thread.  Let that thread create its
    # Windows message queue and shutdown channel before timing stop_monitor.
    settle_seconds = 0.15
    manager = pyclipboard.PyClipboardManager(str(tmp_path / "clipboard-benchmark.db"))
    start_samples_ms: list[float] = []
    stop_samples_ms: list[float] = []

    try:
        for _ in range(sample_count):
            started_at = time.perf_counter()
            manager.start_monitor(callback=lambda _item: None)
            start_samples_ms.append((time.perf_counter() - started_at) * 1000)
            assert manager.is_monitoring()

            time.sleep(settle_seconds)

            stopped_at = time.perf_counter()
            manager.stop_monitor()
            stop_samples_ms.append((time.perf_counter() - stopped_at) * 1000)
            assert not manager.is_monitoring()
    finally:
        if manager.is_monitoring():
            manager.stop_monitor()

    print("\\nClipboard monitor lifecycle benchmark")
    print(_format_stats("start_monitor", start_samples_ms))
    print(_format_stats("stop_monitor ", stop_samples_ms))

    max_stop_ms = os.environ.get("CLIPBOARD_MONITOR_MAX_STOP_MS")
    if max_stop_ms:
        assert max(stop_samples_ms) <= float(max_stop_ms), (
            f"slowest stop_monitor call was {max(stop_samples_ms):.1f} ms; "
            f"budget is {max_stop_ms} ms"
        )
