#pragma once
#include "tools/base.h"
#include <QPainterPath>
#include <array>

namespace jietuba {
class CanvasView;
class MosaicItem;
enum class MosaicKind;

// Freehand or box mosaic, blur or smart erase over the background.
class MosaicTool : public Tool {
public:
    // Block size is one of four levels, not a range: every reader snaps through
    // clamp_block_size, so stale settings such as 6 cannot disagree with the panel.
    // Doubling steps because perceived coarseness is logarithmic.
    static constexpr std::array<int, 4> BLOCK_SIZE_LEVELS = {4, 10, 16, 32};
    // Must itself be a level: it is what clamp_block_size returns for bad input.
    static constexpr int DEFAULT_BLOCK_SIZE = 10;
    static constexpr int MIN_BLOCK_SIZE = 4;
    static constexpr int MAX_BLOCK_SIZE = 32;
    static inline const QString MODE_FREEHAND = "freehand";
    static inline const QString MODE_RECT = "rect";
    static inline const QString STYLE_PIXELATE = "pixelate";
    static inline const QString STYLE_BLUR = "blur";
    static inline const QString STYLE_SMART_ERASE = "smart_erase";
    // Smaller boxes are treated as accidental clicks.
    static constexpr double MIN_SIZE = 10;

    QString id() const override { return "mosaic"; }
    // Snaps to the nearest level; an exact tie takes the coarser one.
    static int clamp_block_size(const QVariant &block_size);
    static int get_block_size(const ToolContext &ctx);
    static QString get_draw_mode(const ToolContext &ctx);
    static QString get_style(const ToolContext &ctx);
    // Unknown styles are mosaic.
    static MosaicKind kind_of(const QString &style);
    static QString style_of(MosaicKind kind);
    // Applies a panel change to the selected mosaic without an undo command:
    // these are settings, and each block-size command would hold a full-screen image.
    static bool apply_style_change(const QString &style, CanvasView *view);
    // The reduced image comes from the item's own scene, so pins and captures cannot mix them up.
    static bool apply_block_size_change(const QVariant &block_size, CanvasView *view);

    void on_activate(ToolContext &ctx) override;
    bool on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) override;
    void on_move(const QPointF &pos, ToolContext &ctx) override;
    void on_release(const QPointF &pos, ToolContext &ctx) override;
    void on_deactivate(ToolContext &ctx) override;

    bool drawing = false;
    MosaicItem *current_item = nullptr;
    std::optional<QPainterPath> path;
    std::optional<QPointF> start_pos;
    QString draw_mode = MODE_FREEHAND;
    std::optional<QPointF> last_point;
private:
    static QVariant read_setting(const ToolContext &ctx, const QString &key);
    static MosaicItem *selected_mosaic(CanvasView *view);
    void reset();
    void remove_live_item(ToolContext &ctx);
};
}
