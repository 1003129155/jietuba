#include "text_recognition/original_image_view.h"
#include "core/i18n.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/text_context_menu.h"
#include "ui/fluent_lite/theme.h"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "TextRecognitionWindow"); }

// Lighter than the selection of the same colour, so the image's own selection stands out.
const QColor HIGHLIGHT_COLOR(100, 150, 255, 60);
constexpr double WHEEL_STEP = 1.15;
constexpr double BUTTON_STEP = 1.25;
// Room around a fitted image, before window scaling.
constexpr int FIT_MARGIN = 12;
}

// Text first, then the blank around it: a press off the text drags the image, a double click
// there switches between fitting and 1:1.
class OriginalImageView::View : public QGraphicsView {
public:
    View(OriginalImageView *owner, QGraphicsScene *scene) : QGraphicsView(scene, owner), owner_(owner) {
        setFrameShape(QFrame::NoFrame);
        setTransformationAnchor(QGraphicsView::NoAnchor);
        setResizeAnchor(QGraphicsView::NoAnchor);
        setRenderHint(QPainter::SmoothPixmapTransform);
        setAlignment(Qt::AlignCenter);
        setFocusPolicy(Qt::ClickFocus);
    }
protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (owner_->selection_->mouse_press(event)) return;
        if (event->button() == Qt::LeftButton) {
            panning_ = true;
            pan_from_ = event->position();
            viewport()->setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        QGraphicsView::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (panning_) {
            const QPointF delta = event->position() - pan_from_;
            pan_from_ = event->position();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - qRound(delta.x()));
            verticalScrollBar()->setValue(verticalScrollBar()->value() - qRound(delta.y()));
            event->accept();
            return;
        }
        if (owner_->selection_->mouse_move(event)) return;
        QGraphicsView::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (panning_ && event->button() == Qt::LeftButton) {
            panning_ = false;
            viewport()->unsetCursor();
            event->accept();
            return;
        }
        if (owner_->selection_->mouse_release(event)) return;
        QGraphicsView::mouseReleaseEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (owner_->selection_->mouse_double_click(event)) return;
        if (event->button() == Qt::LeftButton) {
            owner_->set_fit(owner_->fit_ == Fit::Window ? Fit::Actual : Fit::Window);
            event->accept();
            return;
        }
        QGraphicsView::mouseDoubleClickEvent(event);
    }
    void wheelEvent(QWheelEvent *event) override {
        const double steps = event->angleDelta().y() / 120.0;
        if (std::abs(steps) > 1e-6) owner_->zoom_by(std::pow(WHEEL_STEP, steps), event->position());
        event->accept();
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->matches(QKeySequence::Copy) && owner_->selection_->copy_selected_text()) {
            event->accept();
            return;
        }
        if (owner_->selection_->key_press(event)) {
            owner_->notify_selection();
            return;
        }
        QGraphicsView::keyPressEvent(event);
    }
    bool viewportEvent(QEvent *event) override {
        const bool handled = QGraphicsView::viewportEvent(event);
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
            owner_->notify_selection();
            break;
        default:
            break;
        }
        return handled;
    }
    void contextMenuEvent(QContextMenuEvent *event) override {
        if (!owner_->selection_->has_selection()) return;
        QMenu menu(this);
        auto *copy = menu.addAction(tr_text("Copy"), [this] { owner_->selection_->copy_selected_text(); });
        copy->setShortcut(QKeySequence::Copy);
        style_popup_menu(&menu, ui_tokens(this));
        menu.exec(event->globalPos());
    }
    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event);
        if (owner_->fit_ == Fit::Window) owner_->apply_fit();
    }
    void drawForeground(QPainter *painter, const QRectF &rect) override {
        QGraphicsView::drawForeground(painter, rect);
        if (!owner_->highlight_.isEmpty()) {
            painter->save();
            painter->setPen(Qt::NoPen);
            painter->setBrush(HIGHLIGHT_COLOR);
            painter->setRenderHint(QPainter::Antialiasing, true);
            for (const auto &area : owner_->highlight_)
                if (area.boundingRect().intersects(rect)) painter->drawPolygon(area);
            painter->restore();
        }
        owner_->selection_->paint(painter);
    }
private:
    OriginalImageView *owner_;
    bool panning_ = false;
    QPointF pan_from_;
};

OriginalImageView::OriginalImageView(const QImage &image, QWidget *parent) : QWidget(parent), image_(image) {
    auto *scene = new QGraphicsScene(this);
    pixmap_item_ = scene->addPixmap(QPixmap::fromImage(image));
    pixmap_item_->setTransformationMode(Qt::SmoothTransformation);
    scene->setSceneRect(pixmap_item_->boundingRect());
    view_ = new View(this, scene);
    selection_ = std::make_unique<PinTextSelection>(view_);

    auto button = [this](const char *icon, const char *tip) {
        auto *b = new TransparentToolButton(ResourceManager::get_icon_path(icon), this);
        b->setToolTip(tr_text(tip));
        return b;
    };
    size_label = new CaptionLabel(QString("%1 × %2 px").arg(image.width()).arg(image.height()), this);
    // A caption label wraps and gives up its width by default; these stay on one line.
    size_label->setWordWrap(false);
    size_label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    zoom_out_button = button("zoom_out.svg", "Zoom out");
    scale_label = new CaptionLabel(this);
    scale_label->setWordWrap(false);
    scale_label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    scale_label->setAlignment(Qt::AlignCenter);
    zoom_in_button = button("zoom_in.svg", "Zoom in");
    fit_button = button("actual_size.svg", "Actual size");
    copy_button = button("copy.svg", "Copy image");
    connect(zoom_out_button, &QAbstractButton::clicked, this, [this] { zoom_by(1 / BUTTON_STEP, view_->viewport()->rect().center()); });
    connect(zoom_in_button, &QAbstractButton::clicked, this, [this] { zoom_by(BUTTON_STEP, view_->viewport()->rect().center()); });
    connect(fit_button, &QAbstractButton::clicked, this, [this] { set_fit(fit_ == Fit::Window ? Fit::Actual : Fit::Window); });
    connect(copy_button, &QAbstractButton::clicked, this, [this] { QApplication::clipboard()->setImage(image_); });

    auto *toolbar = new QFrame(this);
    toolbar->setObjectName("originalImageToolbar");
    auto *tools = new QHBoxLayout(toolbar);
    tools->setContentsMargins(dialog_scaled(12), dialog_scaled(4), dialog_scaled(8), dialog_scaled(4));
    tools->setSpacing(dialog_scaled(2));
    tools->addWidget(size_label);
    tools->addStretch();
    tools->addWidget(zoom_out_button);
    tools->addWidget(scale_label);
    tools->addWidget(zoom_in_button);
    tools->addSpacing(dialog_scaled(6));
    tools->addWidget(fit_button);
    tools->addWidget(copy_button);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(view_, 1);
    layout->addWidget(toolbar);
    setMinimumWidth(dialog_scaled(200));

    apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_theme(); });
    apply_fit();
}

OriginalImageView::~OriginalImageView() = default;

QGraphicsView *OriginalImageView::view() const { return view_; }

void OriginalImageView::apply_theme() {
    const auto &t = ui_tokens(this);
    view_->setStyleSheet(QString("QGraphicsView { background: %1; border: none; }").arg(t.surface_subtle) + scrollbar_qss(this));
    findChild<QFrame *>("originalImageToolbar")->setStyleSheet(QString("#originalImageToolbar { background: %1; border-top: 1px solid %2; }").arg(t.input_background, t.separator));
}

void OriginalImageView::set_text(OcrTextLayout layout) {
    selection_->set_layout(std::move(layout));
    notify_selection();
}

void OriginalImageView::clear_text_selection() {
    selection_->clear_selection();
    notify_selection();
}

void OriginalImageView::notify_selection() {
    QString text = selection_->selected_text();
    if (text == selected_text_) return;
    selected_text_ = std::move(text);
    emit text_selection_changed();
}

void OriginalImageView::set_highlight(QVector<QPolygonF> areas) {
    highlight_ = std::move(areas);
    view_->viewport()->update();
    if (highlight_.isEmpty()) return;
    QRectF bounds;
    for (const auto &area : highlight_) bounds = bounds.isNull() ? area.boundingRect() : bounds.united(area.boundingRect());
    const QRectF visible = view_->mapToScene(view_->viewport()->rect()).boundingRect();
    if (!visible.intersects(bounds)) view_->centerOn(bounds.center());
}

double OriginalImageView::device_ratio() const { return std::max(1.0, devicePixelRatioF()); }

double OriginalImageView::scale() const { return view_->transform().m11() * device_ratio(); }

void OriginalImageView::set_fit(Fit fit) {
    fit_ = fit;
    if (fit == Fit::Actual) set_view_scale(1.0, view_->viewport()->rect().center());
    else apply_fit();
    update_toolbar();
}

void OriginalImageView::apply_fit() {
    if (image_.isNull()) return;
    const double margin = 2.0 * dialog_scaled(FIT_MARGIN);
    const QSizeF room(std::max(1.0, view_->viewport()->width() - margin), std::max(1.0, view_->viewport()->height() - margin));
    // A small image is not blown up past 1:1.
    const double fitted = std::min({room.width() / image_.width() * device_ratio(), room.height() / image_.height() * device_ratio(), 1.0});
    view_->setTransform(QTransform::fromScale(fitted / device_ratio(), fitted / device_ratio()));
    view_->centerOn(pixmap_item_);
    update_toolbar();
}

void OriginalImageView::zoom_by(double factor, const QPointF &anchor) {
    if (image_.isNull() || !std::isfinite(factor) || factor <= 0) return;
    fit_ = Fit::Custom;
    set_view_scale(std::clamp(scale() * factor, MIN_SCALE, MAX_SCALE), anchor);
    update_toolbar();
}

void OriginalImageView::set_view_scale(double scale, const QPointF &anchor) {
    const QPointF fixed = view_->mapToScene(anchor.toPoint());
    view_->setTransform(QTransform::fromScale(scale / device_ratio(), scale / device_ratio()));
    const QPointF moved = view_->mapFromScene(fixed) - anchor;
    view_->horizontalScrollBar()->setValue(view_->horizontalScrollBar()->value() + qRound(moved.x()));
    view_->verticalScrollBar()->setValue(view_->verticalScrollBar()->value() + qRound(moved.y()));
}

void OriginalImageView::update_toolbar() {
    const double current = scale();
    scale_label->setText(QString("%1%").arg(qRound(current * 100)));
    // Room for "2000%" in the label's current font, so the buttons beside it stay put while zooming.
    scale_label->setMinimumWidth(scale_label->fontMetrics().horizontalAdvance("2000%") + dialog_scaled(4));
    zoom_out_button->setEnabled(current > MIN_SCALE + 1e-9);
    zoom_in_button->setEnabled(current < MAX_SCALE - 1e-9);
    const bool fitted = fit_ == Fit::Window;
    fit_button->setIcon(ResourceManager::get_icon_path(fitted ? "actual_size.svg" : "fit_window.svg"));
    fit_button->setToolTip(tr_text(fitted ? "Actual size" : "Fit to window"));
}
}
