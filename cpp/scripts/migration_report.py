"""Build an honest symbol-to-C++ evidence report, leaving all other work pending."""
from __future__ import annotations
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
inventory = json.loads((ROOT / 'migration/symbol_inventory.json').read_text(encoding='utf-8'))
mappings = []

def mapped(source, target, names, evidence, integration=False):
    if not (ROOT / target).exists(): raise RuntimeError(target)
    for name in names:
        mappings.append({'source': source, 'symbol': name, 'target': target,
                         'status': 'translated-with-test-evidence', 'evidence': evidence,
                         'application_integration': 'partial dependency integration' if integration else 'pending'})

mapped('main/canvas/selection_model.py', 'src/canvas/selection_model.cpp', [
    'SelectionModel.' + name for name in ['__init__', 'rect', 'set_rect', 'start_dragging', 'stop_dragging',
    'is_dragging', 'is_active', 'activate', 'deactivate', 'confirm', 'is_confirmed',
    'initialize_confirmed_rect', 'is_empty', 'normalize']], 'selectionStateAndSignalTrace')
mapped('main/core/global_hotkey.py', 'src/core/global_hotkey.cpp',
       ['_build_tables', 'parse_hotkey', 'format_hotkey', 'modifier_mask', 'modifier_names', 'route', 'route_of'],
       'globalHotkeyRoutesAndParsing; globalHotkeyFormatting', True)
mapped('main/core/last_capture_region.py', 'src/core/last_capture_region.cpp',
       ['set_last_region', 'get_last_region'], 'lastRegionCopiesAndLifetime')
mapped('main/ui/toolbar_layout.py', 'src/ui/toolbar_layout.cpp',
       ['LayoutProfile', 'LayoutProfile.default_mode', 'default_layout', 'normalize_layout', 'load_layout', 'save_layout'],
       'toolbarNormalization; pinToolbarNormalization; configuration persistence')
mapped('main/core/export.py', 'src/core/export.cpp',
       ['ExportService.' + name for name in ['__init__', 'get_result_pixmap', 'export', 'export_base_image_only']], 'exportPixelsAndVisibility')
mapped('main/core/ui_scale.py', 'src/core/ui_scale.cpp', [
    prefix + name for prefix in ['UIScaleManager.', 'DialogScaleManager.'] for name in
    ['__init__', 'instance', 'init', 'scale_changed', 'percent', 'factor', 'px', 'pxf', 'set_percent']], 'scaleRoundingAndDpi; firstRunAndIndependentScales')
mapped('main/core/ui_scale.py', 'src/core/ui_scale.cpp',
       ['UIScaleManager.normalize_percent', 'UIScaleManager.reset_to_default', 'recommended_scale_percent',
        'apply_first_run_scale_defaults', 'get_ui_scale', 'scaled', 'scaled_f', 'scale_factor', 'get_dialog_scale',
        'dialog_scaled', 'dialog_scaled_f', 'widget_scaled'], 'scaleRoundingAndDpi; firstRunAndIndependentScales')
mapped('main/settings/tool_settings.py', 'src/settings/tool_settings.cpp',
       ['ToolSettings.' + name for name in ['__init__', 'get', 'set', 'update', 'reset_to_defaults', 'to_dict', 'from_dict']],
       'configurationKeysPersistenceAndSignals; defaultsAndToolCoercion', True)
mapped('main/settings/tool_settings.py', 'src/settings/tool_settings.cpp',
       ['ToolSettingsManager.' + name for name in ['__init__', '_initialize_tools', '_load_tool_settings', '_save_tool_settings',
        'get_tool_settings', 'get_setting', 'set_setting', 'update_settings', 'save_all', 'reset_tool', 'reset_all',
        'reset_app_settings', 'app_setting_path', 'get_app_setting', 'set_app_setting', 'reset_all_settings', 'get_color', 'set_color',
        'get_stroke_width', 'set_stroke_width', 'get_opacity', 'set_opacity', 'get_font_size', 'set_font_size',
        'export_settings', 'import_settings', 'get_hotkey', 'set_hotkey', 'get_clipboard_hotkey',
        'get_inapp_shortcut', 'set_inapp_shortcut', 'is_first_run', 'mark_as_run',
        'get_screenshot_save_path', 'set_screenshot_save_path']], 'defaultsAndToolCoercion; configurationKeysPersistenceAndSignals', True)
for row in json.loads((ROOT / 'migration/settings_accessor_map.json').read_text(encoding='utf-8')):
    mapped('main/settings/tool_settings.py', 'src/settings/settings_accessors.inc', [row['symbol']], 'exactGeneratedAccessors', True)
mapped('main/settings/color_formats.py', 'src/settings/color_formats.cpp',
       ['ColorFormat.render', 'default_formats', 'normalize', 'enabled_formats', 'load', '_migrate_from_legacy', 'save'],
       'colorFormatsAndLegacyPersistence')
mapped('main/core/save.py', 'src/core/save.cpp', ['SaveService.' + name for name in
       ['__init__', 'get_default_directory', 'save_qimage', 'save_qimage_async', 'reserve_save_path', 'format_for_size',
        'save_beside', 'save_qimage_to_path', '_save_qimage_async', '_save_qimage_to_path', '_save_qimage_to_pdf_path',
        '_flatten_for_pdf', '_compose_path', '_build_filename', '_normalize_format', '_reserve_unique_path',
        '_claim_first_free', '_cleanup_failed_placeholder']], 'saveFormatLimitsAndPdfPixels; saveReservationAsyncAndFailureCleanup')

mapped('main/canvas/items/drawing_items.py', 'src/canvas/items/drawing_items.cpp', [
    'DrawingItemMixin.' + name for name in ['_init_drawing_mixin', '_set_hovered', '_update_hover_cursor',
    'hoverEnterEvent', 'hoverMoveEvent', 'hoverLeaveEvent', 'shows_selection_frame', 'is_edit_target',
    'selection_frame_style', 'selection_frame_pen', '_edit_controller', '_can_show_hover']],
    'drawingGeometryPixelsAndHover; first-view DrawingEditContext dependency adapter')
for cls, extra in [('StrokeItem', ['shows_selection_frame', 'setPath']),
                   ('RectItem', ['setRect', 'get_corner_radius', 'set_corner_radius', 'boundingRect']),
                   ('EllipseItem', ['setRect', 'boundingRect'])]:
    mapped('main/canvas/items/drawing_items.py', 'src/canvas/items/drawing_items.cpp', [
        cls + '.' + name for name in ['__init__', 'setPen', 'shape', 'paint', 'set_stroke_width',
            'scale_stroke_width', 'set_visual_opacity', 'get_stroke_width', 'get_visual_opacity'] + extra],
        'drawingGeometryPixelsAndHover; editGeometryAndValueCopies (typed native inputs)')
mapped('main/canvas/items/drawing_items.py', 'src/canvas/items/drawing_items.cpp', [
    'NumberItem.' + name for name in ['__init__', 'visualRect', 'boundingRect', 'shape', 'sceneVisualRect',
    'normalize_style', 'set_style', 'is_hollow', 'has_ring', '_ring_width', '_number_font', '_solid_text_color',
    'paint', '_paint_circle', '_paint_number', '_paint_selection_frame', 'set_stroke_width', 'scale_stroke_width',
    'set_visual_opacity', 'get_stroke_width', 'get_visual_opacity']], 'drawingGeometryPixelsAndHover; numberUndoStateAndMergeTrace')
mapped('main/canvas/undo.py', 'src/canvas/undo.h', ['CommandUndoStack.__init__', 'CommandUndoStack.push_command'],
       'numberUndoStateAndMergeTrace; debug logging remains pending')
for cls in ['AddItemCommand', 'RemoveItemCommand', 'AddNumberCommand', 'RemoveNumberCommand',
            'RemoveNumberAndRenumberCommand', 'BatchRemoveCommand', 'NumberStyleCommand']:
    mapped('main/canvas/undo.py', 'src/canvas/undo.cpp', [cls + '.' + name for name in ['__init__', 'undo', 'redo']],
           'numberUndoStateAndMergeTrace; commandAndSceneOwnership')
mapped('main/canvas/undo.py', 'src/canvas/undo.cpp', ['RemoveNumberAndRenumberCommand._build_new_numbers'],
       'numberUndoStateAndMergeTrace (stable sorting including duplicate numbers and draw order)')
mapped('main/canvas/undo.py', 'src/canvas/undo.cpp', ['NumberEditCommand.' + name for name in
       ['__init__', 'undo', 'redo', 'id', 'mergeWith']], 'numberUndoStateAndMergeTrace (number state and 0.7-second merge window)')
# EditItemCommand._apply_state is deliberately NOT mapped as translated:
# font/text/custom-item branches and Python complex-object aliasing remain pending.
mapped('main/canvas/items/arrow_item.py', 'src/canvas/items/arrow_item.cpp', ['ArrowItem.' + name for name in
       ['normalize_style', '__init__', 'setPath', 'boundingRect', 'shape', 'arrow_style', 'control_pos',
        'set_positions', 'set_control_point', 'reset_control_point', 'get_control_point', 'is_curved',
        '_unit', '_perp', '_frame', '_metrics', '_head_trim', '_head_path', '_solid_head_path',
        '_swept_head_path', '_open_head_path', '_bar_path', '_shaft_path', 'update_geometry', 'paint',
        'set_stroke_width', 'scale_stroke_width', 'set_visual_opacity', 'get_stroke_width', 'get_visual_opacity']],
       'drawingGeometryPixelsAndHover; editGeometryAndValueCopies (nine styles, control state, degenerate geometry, both Qt platforms)')
mapped('main/tools/number.py', 'src/canvas/undo.cpp', ['NumberTool.' + name for name in
       ['_count_numbers', 'get_max_number', 'get_next_number', 'adjust_next_number', 'set_next_number',
        'get_next_after_number_edit', 'assign_number_order']], 'numberUndoStateAndMergeTrace (native integer domain)')

lookup = {(row['source'], row['symbol']): row for row in mappings}
used = set()
for module in inventory:
    mapped_count = 0
    for symbol in module['symbols']:
        key = module['source'], symbol['name']
        match = lookup.get(key)
        symbol['migration_status'] = match['status'] if match else 'pending'
        if match:
            symbol['cpp_target'] = match['target']
            symbol['evidence'] = match['evidence']
            mapped_count += 1
            used.add(key)
    module['mapped_symbols'] = mapped_count
    # No entire module has been accepted as end-to-end lossless yet.
    module['status'] = 'partial-with-test-evidence' if mapped_count else 'unported'
missing = set(lookup) - used
if missing: raise RuntimeError(f'Unknown source symbols: {sorted(missing)}')
production = [m for m in inventory if m['role'] == 'production']
report = {'overall_lossless_migration_complete': False, 'source_modules': len(production),
          'production_symbols': sum(len(m['symbols']) for m in production),
          'mapped_symbols_with_test_evidence': sum(m['mapped_symbols'] for m in production),
          'unique_symbol_mappings': len(lookup),
          'pending_production_symbols': sum(len(m['symbols']) - m['mapped_symbols'] for m in production),
          'oracle_qt': '6.11.0', 'native_qt': '6.10.3',
          'mappings': mappings,
          'limitations': ['Finite oracle corpus is not a proof of all possible input states.',
                         'Prototype UI, full canvas, capture lifecycle, editable pins and clipboard are not parity-certified.',
                         'Rust/Pillow adapters, complete input hub, complete settings validation and integration are pending.',
                         'Drawing/edit controller and number refresh use explicit dependency adapters; full canvas integration is pending.',
                         'EditItemCommand replay is partial: font/text/custom-item branches and complex-object reference semantics are pending.',
                         'CommandUndoStack debug logging and complete native/Python dynamic input-domain compatibility are pending.',
                         'Qt version differences and physical multi-monitor/HDR coverage remain unresolved.']}
(ROOT / 'migration/symbol_inventory.json').write_text(json.dumps(inventory, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
(ROOT / 'migration/parity_report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps({k: v for k, v in report.items() if k not in ('mappings', 'limitations')}))
