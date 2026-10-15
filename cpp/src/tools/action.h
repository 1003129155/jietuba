#pragma once
#include "core/export.h"
#include <QPointer>
#include <QRectF>
#include <QWidget>
#include <memory>

class QGraphicsItem;

namespace jietuba {
class CanvasScene;
class SaveService;
class ToolSettingsManager;

// The window ActionTools works for; cleanup_and_close when it has one, else close().
class ActionHost {
public:
    virtual ~ActionHost() = default;
    virtual void cleanup_and_close() = 0;
};

// The toolbar's action buttons: confirm, copy, save, pin, translate, recognize, scan.
class ActionTools {
public:
    ActionTools(CanvasScene *scene, ToolSettingsManager *config_manager = nullptr, QWidget *parent_window = nullptr);
    // Copies to the clipboard, auto-saves per settings and closes.
    void handle_confirm();
    // Same as confirm.
    void handle_copy();
    // Asks where to save.
    void handle_save(bool close_after = true);
    // A pin window gets the clean base image plus the annotations as vector items,
    // so they stay editable; the composite still goes to the clipboard.
    void handle_pin(bool close_after = true);
    // One capture action chosen by a mouse binding; false when it does not apply.
    bool handle_capture_action(const QString &action);
    // Recognizes the selection, closes the capture, then opens the translation window.
    void handle_screenshot_translate();
    // Recognition is slow (hundreds of ms locally), so the result window opens first and waits.
    void handle_text_recognize();
    // Lists QR codes and barcodes in the selection after closing the capture.
    void handle_scan_code();
    CanvasScene *scene;
    ToolSettingsManager *config_manager;
    QPointer<QWidget> parent_window;
    ExportService export_service;
    std::shared_ptr<SaveService> save_service;
private:
    // Export -> clipboard -> auto save -> close.
    void copy_save_and_close(bool close_after = true);
    static bool has_annotations(const QList<QGraphicsItem *> &drawing_items);
    // Quick save skips deliver_screenshot, so "also save the original" is handled here.
    void save_original_beside(const QRectF &selection_rect, const QString &saved_path);
    // The selection without annotations, which would cover text and codes; warns and
    // returns a null image without a confirmed selection or on export failure.
    QImage selection_base_image();
    // Scene coordinates are absolute virtual desktop coordinates, stored as is.
    void remember_last_region(const QRectF &rect);
    // Leaves editing so selection frames and handles are not exported.
    void temporarily_exit_editing();
    void cleanup_and_close();
    static QString format_from_save_path(const QString &file_path, const QString &selected_filter);
};
}
