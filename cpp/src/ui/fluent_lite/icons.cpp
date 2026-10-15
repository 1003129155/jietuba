#include "ui/fluent_lite/icons.h"
#include "core/resource_manager.h"

namespace jietuba {
QString fluent_icon_value(FluentIcon icon) {
    switch (icon) {
    case FluentIcon::CommandPrompt: return "CommandPrompt";
    case FluentIcon::Camera: return "Camera";
    case FluentIcon::Paste: return "Paste";
    case FluentIcon::Brush: return "Brush";
    case FluentIcon::Language: return "Language";
    case FluentIcon::History: return "History";
    case FluentIcon::Application: return "Application";
    case FluentIcon::Info: return "Info";
    case FluentIcon::Delete: return "Delete";
    case FluentIcon::Close: return "Close";
    case FluentIcon::Cancel: return "CancelClose";
    case FluentIcon::Check: return "CheckWhite";
    case FluentIcon::Sparkle: return "SparkleWhite";
    case FluentIcon::Save: return "Save";
    case FluentIcon::Filter: return "Filter";
    case FluentIcon::DateTime: return "DateTime";
    case FluentIcon::Send: return "Send";
    case FluentIcon::Folder: return "Folder";
    case FluentIcon::Edit: return "Edit";
    case FluentIcon::Download: return "Download";
    case FluentIcon::Palette: return "Palette";
    case FluentIcon::FontSize: return "FontSize";
    case FluentIcon::Transparent: return "Transparent";
    case FluentIcon::Certificate: return "Certificate";
    case FluentIcon::Alignment: return "Alignment";
    case FluentIcon::Document: return "Document";
    case FluentIcon::PowerButton: return "PowerButton";
    case FluentIcon::Pin: return "Pin";
    case FluentIcon::Setting: return "Setting";
    case FluentIcon::StopWatch: return "StopWatch";
    case FluentIcon::Font: return "Font";
    case FluentIcon::Layout: return "Layout";
    case FluentIcon::Search: return "Search";
    case FluentIcon::Sync: return "Sync";
    case FluentIcon::Hide: return "Hide";
    case FluentIcon::People: return "People";
    case FluentIcon::GitHub: return "GitHub";
    }
    return {};
}

QString fluent_icon_path(FluentIcon icon) { return ResourceManager::get_resource_path("svg/fluent_icons/" + fluent_icon_value(icon) + ".svg"); }

QIcon fluent_icon(FluentIcon icon) { return ResourceManager::get_icon(fluent_icon_path(icon)); }
}
