"""Run high-churn CanvasView double-click cases in isolated offscreen children."""

import os
from pathlib import Path
import subprocess
import sys

import pytest


CASE_FILE = Path(__file__).with_name("double_click_cases.py")
CASE_FAMILIES = (
    "test_safe_blank_double_click_confirms_exactly_once",
    "test_pen_first_click_is_rolled_back_before_confirm",
    "test_other_drawing_tool_first_click_is_rolled_back_before_confirm",
    "test_number_first_click_is_rolled_back_before_confirm",
    "test_small_number_handle_does_not_steal_blank_double_click",
    "test_empty_text_first_click_is_rolled_back_before_confirm",
    "test_existing_text_edit_double_click_confirms_and_preserves_text",
    "test_nonempty_provisional_text_cancels_double_click_confirm",
    "test_mosaic_first_click_is_rolled_back_before_confirm",
    "test_existing_annotation_double_click_confirms_without_removing_item",
    "test_existing_mosaic_double_click_confirms_without_removing_item",
    "test_eraser_first_click_is_rolled_back_before_confirm",
    "test_default_canvas_is_opted_out",
    "test_pin_canvas_view_is_opted_out",
    "test_gif_drawing_view_is_opted_out",
    "test_active_drawing_with_redo_branch_rolls_back_and_confirms",
    "test_merged_number_control_first_click_is_restored_and_confirms",
    "test_real_number_increment_handle_merge_is_restored_and_confirms",
    "test_merged_number_restore_failure_rejects_confirmation",
    "test_drag_created_selection_allows_double_click_confirmation",
    "test_modified_double_click_does_not_confirm",
    "test_second_click_outside_drag_tolerance_does_not_confirm",
    "test_selection_border_double_click_restores_rect_and_confirms",
    "test_subthreshold_crop_mutation_is_restored_before_confirm",
    "test_real_edit_command_first_click_is_undone_before_confirm",
    "test_unknown_multiple_command_delta_fails_closed",
    "test_wheel_and_ime_input_invalidate_candidate",
    "test_drag_invalidates_double_click_candidate",
    "test_edit_handle_double_click_candidate_keeps_confirm_priority",
)


@pytest.mark.parametrize("case_family", CASE_FAMILIES)
def test_isolated_double_click_case(case_family):
    assert os.environ.get("QT_QPA_PLATFORM") == "offscreen"
    child_env = os.environ.copy()
    child_env["JIETUBA_ISOLATED_QT_CASE"] = "1"
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "pytest",
            "-q",
            str(CASE_FILE),
            "-k",
            case_family,
        ],
        cwd=str(Path(__file__).parents[2]),
        env=child_env,
        capture_output=True,
        text=True,
        timeout=45,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
