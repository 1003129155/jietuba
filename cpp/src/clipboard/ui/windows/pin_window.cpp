#include "clipboard/ui/windows/pin_window.h"
#include "clipboard/controllers/clipboard_controller.h"
#include "clipboard/core/manager.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "pin/pin_animation.h"
#include "pin/pin_file_card.h"
#include "pin/pin_manager.h"
#include "pin/pin_text_content.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/toast.h"
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMimeData>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

namespace jietuba::clipboard {
namespace {
// The newest image is not simply the first row: history sorts pinned items first and
// manual reordering ignores insertion time, so one page is scanned for the latest created_at.
constexpr int LATEST_IMAGE_SCAN_LIMIT = 50;
// More picture files than this at once ask first: each becomes a window of its own.
constexpr int MANY_PICTURES = 15;
// Pins from one hotkey press step down and right from the pointer, so none hides another.
constexpr int CASCADE_STEP = 24;

QString pin_tr(const char *text) { return i18n_tr(text, "PinWindow"); }

bool pin_item(qint64 item_id, const std::function<std::optional<ClipboardItem>(qint64)> &get_item, QWidget *clipboard_window, std::optional<QPoint> center) {
    const auto clipboard_item = get_item(item_id);
    if (clipboard_item && clipboard_item->content_type == "text")
        return create_pin_from_text(clipboard_item->content, clipboard_item->html_content.value_or(QString()), clipboard_window, center);
    if (clipboard_item && clipboard_item->content_type == "file") return create_pins_from_clipboard_items({*clipboard_item}, clipboard_window, center);
    if (!clipboard_item || clipboard_item->content_type != "image") {
        log_error(T("无法创建钉图：item_id={item_id} 不是图片", {{"item_id", item_id}}), "Clipboard");
        return false;
    }
    if (!clipboard_item->image_id || clipboard_item->image_id->isEmpty()) {
        log_error(T("无法创建钉图：图片 ID 为空"), "Clipboard");
        return false;
    }
    // The full image data, not the thumbnail.
    auto image_data = ClipboardManager::instance().get_image_data(*clipboard_item->image_id);
    if (!image_data || image_data->isEmpty()) {
        log_error(T("无法创建钉图：图片数据加载失败 (image_id={image_id})", {{"image_id", *clipboard_item->image_id}}), "Clipboard");
        return false;
    }
    const QImage image = QImage::fromData(*image_data);
    if (image.isNull()) {
        log_error(T("无法创建钉图：图片解码失败"), "Clipboard");
        return false;
    }
    // The decoded QImage needs no compressed data; the PNG bytes go early.
    image_data.reset();
    if (!create_pin_from_image(image, clipboard_window, center)) return false;
    log_debug(T("从剪贴板创建钉图窗口成功 (item_id={item_id})", {{"item_id", item_id}}), "Clipboard");
    return true;
}

// The screen holding center, else the clipboard window's, else the primary.
QScreen *target_screen(QWidget *clipboard_window, std::optional<QPoint> center) {
    if (center) {
        if (auto *screen = QGuiApplication::screenAt(*center)) return screen;
    } else if (clipboard_window) {
        return clipboard_window->screen();
    }
    return QGuiApplication::primaryScreen();
}

QPoint calculate_pin_position(const QSize &size, QWidget *clipboard_window, std::optional<QPoint> center) {
    const auto *screen = target_screen(clipboard_window, center);
    if (!screen) return QPoint(100, 100);

    const QRect screen_geometry = screen->geometry();
    const QPoint target = center ? *center : QPoint(screen_geometry.x() + screen_geometry.width() / 2, screen_geometry.y() + screen_geometry.height() / 2);
    int pin_x = target.x() - size.width() / 2;
    int pin_y = target.y() - size.height() / 2;
    // Kept inside the screen.
    pin_x = std::max(screen_geometry.x(), std::min(pin_x, screen_geometry.right() - size.width()));
    pin_y = std::max(screen_geometry.y(), std::min(pin_y, screen_geometry.bottom() - size.height()));
    return {pin_x, pin_y};
}

// PNG data, which a capture copied with its file path also carries: the picture itself, not the file.
QImage clipboard_png(const QMimeData *mime) {
    for (const char *format : {"image/png", "application/x-qt-windows-mime;value=\"PNG\""}) {
        if (!mime->hasFormat(format)) continue;
        const auto image = QImage::fromData(mime->data(format), "PNG");
        if (!image.isNull()) return image;
    }
    return {};
}

// The copied local files, in order. Not checked on disk: thousands of files would hold the UI thread.
QStringList clipboard_files(const QMimeData *mime) {
    QStringList files;
    if (!mime->hasUrls()) return files;
    for (const auto &url : mime->urls())
        if (url.isLocalFile()) files.append(url.toLocalFile());
    return files;
}

// By name alone, so even ten thousand files are sorted at once; one that does not decode goes with
// the cards. The image formats this build reads, written out: asking the plugins costs a first-use delay.
bool picture_name(const QString &file) {
    static const QSet<QString> suffixes = {"bmp", "dib", "gif", "ico", "cur", "jfif", "jpe", "jpeg", "jpg", "pbm", "pgm", "png", "ppm",
                                           "svg", "svgz", "tif", "tiff", "wbmp", "webp", "xbm", "xpm"};
    return suffixes.contains(QFileInfo(file).suffix().toLower());
}

// What one batch pins: optionally a card of files, then each picture or text; in this order on screen.
struct Opened {
    std::optional<AnimationSource> animation;
    QImage image;
    // A card with its icons fetched, drawn as image.
    std::optional<PinFileCard> card;
    std::unique_ptr<PinTextContent> text;
};

// One pin of a batch after its card.
struct Piece {
    enum Kind { PictureFile, StoredImage, Text } kind;
    // The file, the id of the image stored in history, or the text.
    QString value;
    QString html = {};
};

Opened still_picture(QImage image) {
    Opened opened;
    opened.image = std::move(image);
    return opened;
}

Opened decode_picture(const QString &file) {
    if (auto animation = load_animation(file)) {
        Opened opened;
        opened.animation = std::move(animation);
        return opened;
    }
    QImageReader reader(file);
    // Photos stand as the camera held them.
    reader.setAutoTransform(true);
    return still_picture(reader.read());
}

Opened decode_stored_image(const QString &image_id) {
    const auto data = ClipboardManager::instance().get_image_data(image_id);
    return still_picture(data ? QImage::fromData(*data) : QImage());
}

Opened draw_card(const QStringList &files, const QSize &limit) {
    // The shell's icons want COM on the thread asking for them.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PinFileCard card(files, limit);
    card.load_icons();
    QImage image = card.render(card.size());
    if (SUCCEEDED(com)) CoUninitialize();
    Opened opened;
    opened.image = std::move(image);
    opened.card = std::move(card);
    return opened;
}

void place_image_pin(const QImage &image, QWidget *clipboard_window, std::optional<QPoint> center) {
    auto *config_manager = get_tool_settings_manager();
    const QPoint position = calculate_pin_position(image.size(), clipboard_window, center);
    get_pin_manager()->create_pin(image, position, config_manager);
    // Capture pins trim the working set when the capture window finishes; the clipboard path has no capture window, so it trims here.
    request_trim_working_set(1500);
}

// Pictures decode, texts are laid out and cards are drawn on worker threads, so many or large ones
// never hold the UI thread. Each pin opens on the UI thread in its own turn, in order, once it and
// those before it are ready. Picture files that do not decode end on one card, unless they no
// longer exist.
class PinBatch : public QObject {
public:
    PinBatch(const QStringList &card_files, const QList<Piece> &pieces, bool cards, const QSize &card_limit, const QPoint &origin, bool pinned)
        : QObject(qApp), pieces_(pieces), opened_(pieces.size() + (card_files.isEmpty() ? 0 : 1)), cards_(cards), card_limit_(card_limit), origin_(origin),
          pinned_(pinned) {
        // Below the UI thread and on half the cores, so neither the pins nor other programs wait for this.
        pool_.setThreadPriority(QThread::LowPriority);
        pool_.setMaxThreadCount(std::max(1, QThread::idealThreadCount() / 2));
        int index = 0;
        if (!card_files.isEmpty()) start(index++, [card_files, card_limit] { return draw_card(card_files, card_limit); });
        const bool formatted = get_tool_settings_manager()->get_pin_text_formatting();
        for (const auto &piece : pieces) {
            const int at = index++;
            if (piece.kind == Piece::Text) {
                // Unformatted, the pin keeps the formatting for its menu to switch to.
                PinTextContent::build_async(piece.value, piece.html, formatted, this, [this, at](std::unique_ptr<PinTextContent> content) {
                    Opened opened;
                    opened.text = std::move(content);
                    arrive(at, std::move(opened));
                });
            } else if (piece.kind == Piece::StoredImage) {
                start(at, [image_id = piece.value] { return decode_stored_image(image_id); });
            } else {
                start(at, [file = piece.value] { return decode_picture(file); });
            }
        }
    }
    ~PinBatch() override {
        // Results still on their way are dropped with this object.
        pool_.clear();
        pool_.waitForDone();
    }
private:
    void start(int index, std::function<Opened()> work) {
        pool_.start([this, index, work] {
            auto opened = std::make_shared<Opened>(work());
            QMetaObject::invokeMethod(this, [this, index, opened] { arrive(index, std::move(*opened)); }, Qt::QueuedConnection);
        });
    }
    void arrive(int index, Opened opened) {
        opened_[index] = std::move(opened);
        schedule_next();
    }
    // Results that arrive together still open one pin per turn of the event loop, so input and painting
    // get their turn between pins.
    void schedule_next() {
        if (scheduled_) return;
        scheduled_ = true;
        QTimer::singleShot(0, this, [this] {
            scheduled_ = false;
            open_next();
        });
    }
    // Whether a picture this size is pinned. The first small one asks, and its answer covers the rest
    // of the batch; pins wait while the question is open.
    bool wanted(const QSize &size) {
        if (!PinManager::is_small_pin(size)) return true;
        if (!small_wanted_) {
            asking_ = true;
            small_wanted_ = PinManager::confirm_small_pin(size, nullptr);
            asking_ = false;
        }
        if (!*small_wanted_) declined_ = true;
        return *small_wanted_;
    }
    void open_next() {
        if (asking_ || next_ >= static_cast<int>(opened_.size()) || !opened_[next_]) return;
        auto opened = std::move(*opened_[next_]);
        opened_[next_].reset();
        // The pieces follow the card, when there is one.
        const int piece = next_++ - (static_cast<int>(opened_.size()) - pieces_.size());
        const QPoint at = origin_ + QPoint(CASCADE_STEP, CASCADE_STEP) * placed_;
        auto *settings = get_tool_settings_manager();
        if (point_out_repeat(opened)) {
            ++pointed_;
        } else if (opened.card) {
            get_pin_manager()->create_file_pin(*opened.card, calculate_pin_position(opened.card->size(), nullptr, at), settings, opened.image);
            ++placed_;
        } else if (opened.animation) {
            if (!wanted(opened.animation->first_frame.deviceIndependentSize().toSize())) return finish_turn();
            const QPoint position = calculate_pin_position(opened.animation->first_frame.size(), nullptr, at);
            get_pin_manager()->create_animation_pin(std::move(*opened.animation), position, settings);
            ++placed_;
        } else if (opened.text) {
            const QPoint position = calculate_pin_position(opened.text->size(), nullptr, at);
            get_pin_manager()->create_text_pin(std::move(opened.text), position, settings);
            ++placed_;
        } else if (!opened.image.isNull()) {
            if (!wanted(opened.image.deviceIndependentSize().toSize())) return finish_turn();
            place_image_pin(opened.image, nullptr, at);
            ++placed_;
        } else if (cards_ && piece >= 0 && pieces_[piece].kind == Piece::PictureFile && QFileInfo::exists(pieces_[piece].value)) {
            undecoded_.append(pieces_[piece].value);
        }
        finish_turn();
    }
    // A piece an open pin already shows: that pin shakes instead.
    static bool point_out_repeat(const Opened &opened) {
        auto *manager = get_pin_manager();
        if (opened.card) return manager->point_out(manager->file_pin_of(opened.card->files()));
        if (opened.animation) return manager->point_out(manager->animation_pin_of(opened.animation->file_path));
        if (opened.text) return manager->point_out(manager->text_pin_of(opened.text->text()));
        return manager->point_out(manager->image_pin_of(opened.image));
    }
    // The next pin's turn, or the end of the batch.
    void finish_turn() {
        if (next_ < static_cast<int>(opened_.size())) {
            schedule_next();
            return;
        }
        if (!undecoded_.isEmpty()) new PinBatch(undecoded_, {}, false, card_limit_, origin_ + QPoint(CASCADE_STEP, CASCADE_STEP) * placed_, true);
        if (placed_ == 0 && pointed_ == 0 && undecoded_.isEmpty() && !pinned_ && !declined_) show_toast(i18n_tr("Nothing on the clipboard to pin", "MainApp"));
        deleteLater();
    }
    QThreadPool pool_;
    QList<Piece> pieces_;
    std::vector<std::optional<Opened>> opened_;
    bool cards_;
    QSize card_limit_;
    QPoint origin_;
    // Something else from the same press was pinned already.
    bool pinned_;
    int next_ = 0, placed_ = 0, pointed_ = 0;
    bool scheduled_ = false;
    // The answer about small pictures, once asked; declined_ when one was left out for it.
    std::optional<bool> small_wanted_;
    bool asking_ = false, declined_ = false;
    QStringList undecoded_;
};

// Half the screen at most; past that the last card counts the rest.
QSize card_limit(const QScreen *screen) { return screen ? screen->availableGeometry().size() / 2 : QSize(960, 540); }

// card_files on one card, then a pin per piece, from origin down and right. Asks first when there
// are many pictures. True when anything is being pinned or the user declined.
bool start_batch(const QStringList &card_files, const QList<Piece> &pieces, bool cards, const QPoint &origin, QWidget *parent) {
    if (card_files.isEmpty() && pieces.isEmpty()) return false;
    const auto pictures = std::count_if(pieces.begin(), pieces.end(), [](const Piece &piece) { return piece.kind != Piece::Text; });
    if (pictures > MANY_PICTURES &&
        !show_confirm_dialog(parent, pin_tr("Pin Many Images"), pin_tr("Pin %1 images at once? So many windows may slow the computer down.").arg(pictures)))
        return true;
    const auto *screen = target_screen(nullptr, origin);
    new PinBatch(card_files, pieces, cards, card_limit(screen), origin, false);
    return true;
}

// Each picture file on its own pin, an animation playing, and the other files together as cards;
// pictures turned off go with the cards. True when anything is being pinned or the user declined.
bool pin_files(const QStringList &files, bool pictures, bool cards, const QPoint &origin) {
    QStringList others;
    QList<Piece> images;
    for (const auto &file : files) {
        if (pictures && picture_name(file)) images.append({Piece::PictureFile, file});
        else if (cards) others.append(file);
    }
    if (!start_batch(others, images, cards, origin, nullptr)) return false;
    log_debug(T("已钉住剪贴板中的 {count} 个文件", {{"count", static_cast<int>(files.size())}}), "Clipboard");
    return true;
}

// The history image with the latest created_at.
std::optional<ClipboardItem> find_latest_history_image() {
    // With monitoring off the history is stale, and one hotkey does not open the database.
    if (!get_tool_settings_manager()->get_clipboard_enabled()) return std::nullopt;
    auto &manager = ClipboardManager::instance();
    if (!manager.is_available()) return std::nullopt;
    QList<ClipboardItem> candidates;
    for (const auto &item : manager.get_history(0, LATEST_IMAGE_SCAN_LIMIT, std::nullopt, FilterRule::of_kind("image")))
        if (item.image_id && !item.image_id->isEmpty()) candidates.append(item);
    if (candidates.isEmpty()) return std::nullopt;
    const ClipboardItem *latest = nullptr;
    for (const auto &item : candidates)
        if (item.created_at.isValid() && (!latest || item.created_at > latest->created_at)) latest = &item;
    // created_at is normally present; without it, list order.
    return latest ? *latest : candidates.front();
}
}

bool create_pin_from_clipboard_item(qint64 item_id, ClipboardController *controller, QWidget *clipboard_window, std::optional<QPoint> center) {
    return pin_item(item_id, [controller](qint64 id) { return controller->get_item(id); }, clipboard_window, center);
}

bool create_pin_from_clipboard_item(qint64 item_id, ClipboardManager *manager, QWidget *clipboard_window, std::optional<QPoint> center) {
    return pin_item(item_id, [manager](qint64 id) { return manager->get_item(id); }, clipboard_window, center);
}

bool create_pin_from_image(const QImage &image, QWidget *clipboard_window, std::optional<QPoint> center) {
    if (image.isNull()) return false;
    auto *manager = get_pin_manager();
    if (manager->point_out(manager->image_pin_of(image))) return true;
    // Declining counts as handled, so no "nothing to pin" follows.
    if (PinManager::confirm_small_pin(image.deviceIndependentSize().toSize(), clipboard_window)) place_image_pin(image, clipboard_window, center);
    return true;
}

bool create_pins_from_clipboard_items(const QList<ClipboardItem> &items, QWidget *clipboard_window, std::optional<QPoint> center) {
    QStringList others;
    QList<Piece> pieces;
    for (const auto &item : items) {
        if (item.content_type == "image" && item.image_id && !item.image_id->isEmpty()) {
            pieces.append({Piece::StoredImage, *item.image_id});
        } else if (item.content_type == "text" && !item.content.trimmed().isEmpty()) {
            pieces.append({Piece::Text, item.content, item.html_content.value_or(QString())});
        } else if (item.content_type == "file") {
            for (const auto &file : item_paths(item)) {
                if (picture_name(file)) pieces.append({Piece::PictureFile, file});
                else others.append(file);
            }
        }
    }
    const auto *screen = target_screen(clipboard_window, center);
    const QPoint origin = center ? *center : screen ? screen->geometry().center() : QPoint(100, 100);
    return start_batch(others, pieces, true, origin, clipboard_window);
}

bool create_pin_from_text(const QString &text, const QString &html, QWidget *clipboard_window, std::optional<QPoint> center) {
    if (text.trimmed().isEmpty()) return false;
    auto *manager = get_pin_manager();
    if (manager->point_out(manager->text_pin_of(text))) return true;
    // Laid out and drawn on a worker thread, so a long text never holds the UI; the pin opens once it
    // is ready. Unformatted, the pin keeps the formatting for its menu to switch to.
    const QPointer<QWidget> window(clipboard_window);
    PinTextContent::build_async(text, html, get_tool_settings_manager()->get_pin_text_formatting(), qApp, [window, center](std::unique_ptr<PinTextContent> content) {
        const QPoint position = calculate_pin_position(content->size(), window, center);
        get_pin_manager()->create_text_pin(std::move(content), position, get_tool_settings_manager());
    });
    return true;
}

bool pin_latest_clipboard_image(std::optional<QPoint> center) {
    auto *settings = get_tool_settings_manager();
    const bool images = settings->get_pin_clipboard_image(), texts = settings->get_pin_clipboard_text(), files = settings->get_pin_clipboard_file();
    QClipboard *clipboard = QGuiApplication::clipboard();
    // A cleared clipboard has no data at all; the newest image in history stands in for it.
    const QMimeData *mime = clipboard ? clipboard->mimeData() : nullptr;
    if (images && mime)
        if (const auto png = clipboard_png(mime); !png.isNull()) {
            create_pin_from_image(png, nullptr, center);
            log_debug(T("已钉住系统剪贴板中的图片"), "Clipboard");
            return true;
        }
    // A bitmap wins over the text beside it, as in copied spreadsheet cells, and over the file beside
    // it, as in a capture copied with the path it is still being saved to.
    if (images && mime)
        if (const auto image = clipboard->image(); !image.isNull()) {
            create_pin_from_image(image, nullptr, center);
            log_debug(T("已钉住系统剪贴板中的图片"), "Clipboard");
            return true;
        }
    const auto copied = mime ? clipboard_files(mime) : QStringList();
    if (!copied.isEmpty() && (images || files)) {
        const auto *screen = target_screen(nullptr, center);
        const QPoint origin = center ? *center : screen ? screen->geometry().center() : QPoint(100, 100);
        if (pin_files(copied, images, files, origin)) return true;
    }
    // hasText() also holds for a bare file list (its paths as text); only a real text format counts.
    if (texts && mime && mime->hasFormat("text/plain") && create_pin_from_text(mime->text(), mime->hasHtml() ? mime->html() : QString(), nullptr, center)) {
        log_debug(T("已钉住系统剪贴板中的文字"), "Clipboard");
        return true;
    }
    // Copied files never bring back an older picture from history.
    if (!images || !copied.isEmpty()) return false;
    const auto found = find_latest_history_image();
    if (!found) {
        log_debug(T("剪贴板和历史中都没有图片，钉图已跳过"), "Clipboard");
        return false;
    }
    return create_pin_from_clipboard_item(found->id, &ClipboardManager::instance(), nullptr, center);
}
}
