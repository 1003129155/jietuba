#include "tools/text.h"
#include "canvas/items/text_item.h"
#include "canvas/scene.h"
#include "core/constants.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include <QTextCursor>

namespace jietuba {
bool TextTool::on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) {
    if (button != Qt::LeftButton) return true;
    auto *manager = get_tool_settings_manager();
    const auto font_family = normalize_text_font_family(manager->get_setting("text", "font_family", "").toString());
    const int font_size = manager->get_setting("text", "font_size", 16).toInt();
    const bool font_bold = manager->get_setting("text", "font_bold", false).toBool();
    const bool font_italic = manager->get_setting("text", "font_italic", false).toBool();
    const bool font_underline = manager->get_setting("text", "font_underline", false).toBool();
    const bool background_enabled = manager->get_setting("text", "background_enabled", false).toBool();
    const auto background_color = manager->get_setting("text", "background_color", "#FFFFFF").toString();
    const int background_opacity = manager->get_setting("text", "background_opacity", 255).toInt();
    const bool always_on_top = manager->get_text_always_on_top_enabled();
    QFont font(font_family, font_size);
    font.setBold(font_bold);
    font.setItalic(font_italic);
    font.setUnderline(font_underline);
    auto *item = new TextItem("", pos, font, ctx.color, always_on_top, true);
    item->set_outline(manager->get_setting("text", "outline_enabled").toBool(),
                      QColor(manager->get_setting("text", "outline_color").toString()),
                      manager->get_setting("text", "outline_width"));
    item->set_shadow(manager->get_setting("text", "shadow_enabled").toBool(), QColor(manager->get_setting("text", "shadow_color").toString()));
    // The colour is set even with the background off, so enabling it later reuses the saved colour.
    QColor bg_color(background_color);
    bg_color.setAlpha(background_opacity);
    item->set_background(background_enabled, bg_color, background_opacity);
    ctx.scene->addItem(item);
    // No AddItemCommand here: the item is empty and the user may click away
    // without typing. TextItem.focusOutEvent commits it once it has content.
    item->setFocus();
    auto cursor = item->textCursor();
    cursor.movePosition(QTextCursor::End);
    item->setTextCursor(cursor);
    emit ctx.scene->item_auto_select_requested(item);
    log_debug(T("创建文字: {pos}", {{"pos", geometry_text(pos)}}), "TextTool");
    return true;
}

void TextTool::on_deactivate(ToolContext &ctx) {
    if (auto *focus = ctx.scene->focusItem()) focus->clearFocus();
}
}
