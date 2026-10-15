#include "tools/watermark.h"
#include "canvas/scene.h"
#include "settings/tool_settings.h"

namespace jietuba {
WatermarkStyle WatermarkTool::load_style(ToolSettingsManager *settings_manager) {
    QVariantMap values;
    if (settings_manager) {
        const auto all = settings_manager->get_tool_settings("watermark")->to_dict();
        for (const auto &key : WatermarkStyle().to_values().keys())
            if (all.contains(key)) values.insert(key, all.value(key));
    }
    return WatermarkStyle::from_values(values);
}

QString WatermarkTool::current_template(ToolSettingsManager *settings_manager) {
    return settings_manager ? settings_manager->get_screenshot_watermark_text() : QString();
}

WatermarkItem *WatermarkTool::add(CanvasScene *scene, bool undoable) {
    if (auto *existing = WatermarkItem::find(scene)) return existing;
    auto *manager = scene->tool_settings_manager;
    auto *item = new WatermarkItem(current_template(manager), load_style(manager));
    if (undoable) {
        scene->undo_stack->push(new AddItemCommand(scene, ItemReference(item), "Add Watermark"));
    } else {
        scene->addItem(item);
    }
    return item;
}

WatermarkStyle WatermarkTool::change_style(CanvasScene *scene, const QVariantMap &changes) {
    auto *manager = scene->tool_settings_manager;
    const auto style = load_style(manager).with_changes(changes);
    manager->update_settings("watermark", style.to_values());
    if (auto *item = WatermarkItem::find(scene)) item->set_style(style);
    return style;
}

WatermarkItem *WatermarkTool::change_text(CanvasScene *scene, const QString &template_text) {
    scene->tool_settings_manager->set_screenshot_watermark_text(template_text);
    auto *item = WatermarkItem::find(scene);
    if (item) item->set_template(template_text, QDateTime::currentDateTime());
    return item;
}

QString WatermarkTool::auto_template(ToolSettingsManager *settings_manager) {
    if (!settings_manager || !settings_manager->get_screenshot_watermark_auto() || !settings_manager->get_screenshot_watermark_last_on()) return {};
    return current_template(settings_manager);
}

void WatermarkTool::remember(CanvasScene *scene) {
    auto *item = WatermarkItem::find(scene);
    scene->tool_settings_manager->set_screenshot_watermark_last_on(item && !item->text().isEmpty());
}

QImage WatermarkTool::apply_to_image(const QImage &image, ToolSettingsManager *settings_manager, const QDateTime &when) {
    const auto text = expand_text(auto_template(settings_manager), when);
    return text.isEmpty() ? image : apply_watermark(image, text, load_style(settings_manager));
}
}
