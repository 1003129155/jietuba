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
    "test_double_click_inside_edited_text_does_not_confirm",
    "test_mosaic_first_click_is_rolled_back_before_confirm",
    "test_existing_annotation_double_click_confirms_without_removing_item",
    "test_existing_mosaic_double_click_confirms_without_removing_item",
    "test_eraser_first_click_is_rolled_back_before_confirm",
    "test_default_canvas_is_opted_out",
    "test_pin_canvas_view_is_opted_out",
    "test_gif_drawing_view_is_opted_out",
    "test_active_drawing_with_redo_branch_rolls_back_and_confirms",
    "test_number_increment_handle_repeated_clicks_do_not_confirm",
    "test_drag_created_selection_allows_double_click_confirmation",
    "test_modified_double_click_does_not_confirm",
    "test_second_click_outside_drag_tolerance_does_not_confirm",
    "test_selection_border_double_click_restores_rect_and_confirms",
    "test_subthreshold_crop_mutation_is_restored_before_confirm",
    "test_real_edit_command_first_click_is_undone_before_confirm",
    "test_unknown_multiple_command_delta_fails_closed",
    "test_wheel_and_ime_input_invalidate_candidate",
    "test_drag_invalidates_double_click_candidate",
    "test_edit_handle_click_cancels_double_click_confirm",
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


@pytest.mark.parametrize("double_click_enabled", [True, False])
@pytest.mark.parametrize("cross_tool_enabled", [True, False])
def test_screenshot_view_factory_reads_capture_behavior_settings(
    monkeypatch,
    double_click_enabled,
    cross_tool_enabled,
):
    """初次与复用会话共享的视图工厂必须把设置传给 CanvasView。"""
    from types import SimpleNamespace
    from ui import screenshot_window as screenshot_window_module

    captured = {}

    def fake_canvas_view(
        scene,
        parent,
        *,
        confirm_on_double_click=False,
        cross_tool_select=False,
    ):
        captured.update(
            scene=scene,
            parent=parent,
            confirm_on_double_click=confirm_on_double_click,
            cross_tool_select=cross_tool_select,
        )
        return object()

    monkeypatch.setattr(screenshot_window_module, "CanvasView", fake_canvas_view)
    scene = object()
    window = SimpleNamespace(
        config_manager=SimpleNamespace(
            get_double_click_copy_close_enabled=lambda: double_click_enabled,
            get_cross_tool_selection_enabled=lambda: cross_tool_enabled,
        ),
    )

    result = screenshot_window_module.ScreenshotWindow._create_canvas_view(window, scene)

    assert result is not None
    assert captured == {
        "scene": scene,
        "parent": window,
        "confirm_on_double_click": double_click_enabled,
        "cross_tool_select": cross_tool_enabled,
    }
