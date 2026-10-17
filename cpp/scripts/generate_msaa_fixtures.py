"""Generate point-cache expectations by executing the Python implementation."""
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "main"))
from PySide6.QtWidgets import QApplication
from capture.msaa_element_finder import ElementSnapshot, MSAAElementFinder
from ui.initial_selection import InitialSelectionGate


class Service:
    def submit(self, request):
        self.request = request
        request.session.latest = request.sequence


app = QApplication([])
cases = [
    {"name": "leaf", "point": [20, 20], "rects": [[10, 10, 40, 40]], "min": [1, 1]},
    {"name": "actual_parent", "point": [12, 12], "rects": [[10, 10, 15, 20], [0, 0, 100, 100]], "min": [10, 10]},
    {"name": "right_exclusive", "point": [40, 20], "rects": [[10, 10, 40, 40]], "min": [1, 1]},
    {"name": "negative_coordinates", "point": [-40, -20], "rects": [[-50, -30, -10, 0]], "min": [1, 1]},
    {"name": "empty", "point": [20, 20], "rects": [], "min": [1, 1]},
    {"name": "chain", "point": [20, 20], "rects": [[10, 10, 40, 40], [0, 0, 100, 100]], "min": [1, 1]},
]
for case in cases:
    service = Service()
    finder = MSAAElementFinder(service=service)
    finder.elements_at(100, *case["point"])
    request = service.request
    request.session.replies.put((request, tuple(ElementSnapshot(tuple(r)) for r in case["rects"]), ""))
    finder.prepare_at(100, *case["point"])
    case["expected"] = [e.rect for e in finder.elements_at(100, *case["point"], min_size=case["min"])]
    finder.close()
    finder.deleteLater()

target = ROOT / "cpp/tests/fixtures/msaa.json"
target.write_text(json.dumps({"initial_wait_ms": InitialSelectionGate.WAIT_MS, "cases": cases}, indent=2) + "\n", encoding="utf8")
print(target)
