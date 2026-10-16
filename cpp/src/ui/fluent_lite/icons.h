#pragma once
#include <QIcon>
#include <QString>

namespace jietuba {
// Local SVG icons under svg/fluent_icons.
// CamelCase names: windows.h defines DELETE and TRANSPARENT as macros.
enum class FluentIcon {
    CommandPrompt, Camera, Paste, Brush, Language, History, Application, Info, Delete, Close, Cancel, Check,
    Sparkle, Save, Filter, DateTime, Send, Folder, Edit, Download, Palette, FontSize, Transparent, Certificate,
    Alignment, Document, PowerButton, Pin, Setting, StopWatch, Font, Layout, Search, Sync, Hide, People, GitHub,
};

QString fluent_icon_value(FluentIcon icon);
QString fluent_icon_path(FluentIcon icon);
// Rasterised and cached through ResourceManager.
QIcon fluent_icon(FluentIcon icon);
}
