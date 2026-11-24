#include "ui/settings_ui/upload_target_dialog.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/theme.h"
#include "ui/fluent_lite/titlebar.h"
#include "upload/upload_service.h"
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPointer>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "UploadTargetDialog"); }

// The endpoint a provider's S3-compatible service answers on; %1 is the region where the host carries it.
struct Preset {
    const char *name;
    const char *endpoint;
    const char *region;
    bool virtual_host;
    bool region_in_endpoint;
};

const Preset PRESETS[] = {
    {"AWS S3", "https://s3.%1.amazonaws.com", "us-east-1", true, true},
    {"Cloudflare R2", "https://<ACCOUNT_ID>.r2.cloudflarestorage.com", "auto", false, false},
    {"Alibaba Cloud OSS", "https://s3.oss-%1.aliyuncs.com", "cn-hangzhou", true, true},
    {"Tencent Cloud COS", "https://cos.%1.myqcloud.com", "ap-guangzhou", true, true},
    {"MinIO or other", "http://localhost:9000", "us-east-1", false, false},
};
// The provider an endpoint belongs to, by the domain its host ends in; the last preset for any other.
const char *const PRESET_DOMAINS[] = {".amazonaws.com", ".r2.cloudflarestorage.com", ".aliyuncs.com", ".myqcloud.com"};
constexpr int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

QString preset_endpoint(const Preset &preset, const QString &region) { return QString::fromLatin1(preset.endpoint).replace("%1", region); }

int preset_of_endpoint(const QString &endpoint) {
    const QString host = QUrl(endpoint).host().toLower();
    for (int i = 0; i < static_cast<int>(std::size(PRESET_DOMAINS)); ++i)
        if (host.endsWith(QLatin1String(PRESET_DOMAINS[i]))) return i;
    return PRESET_COUNT - 1;
}

// One line that ends in an ellipsis in the middle and shows everything as its tooltip.
class ElidedLabel : public CaptionLabel {
public:
    explicit ElidedLabel(QWidget *parent) : CaptionLabel(parent) {
        setWordWrap(false);
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    void set_full_text(const QString &text) {
        full_ = text;
        setToolTip(text);
        elide();
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        CaptionLabel::resizeEvent(event);
        elide();
    }
private:
    void elide() { setText(fontMetrics().elidedText(full_, Qt::ElideMiddle, contentsRect().width())); }
    QString full_;
};
}

UploadTargetDialog::UploadTargetDialog(const upload::Target &initial, const std::vector<upload::Target> &others, bool is_new, QWidget *parent)
    : FluentDialog(parent), initial_(initial), others_(others) {
    setObjectName("upload_target_dialog");
    setWindowTitle(is_new ? tr_text("Add Upload Target") : tr_text("Edit Upload Target"));
    setResizeEnabled(false);

    scroll = new QScrollArea(FluentDialog::body());
    scroll->setObjectName("upload_target_scroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; } QScrollArea > QWidget > QWidget { background: transparent; }" +
                          scrollbar_qss(this));
    auto *content = new QWidget(scroll);
    auto *outer = new QVBoxLayout(content);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *body = new DialogPanel(content);
    outer->addWidget(body);
    // Captions beside their fields keep the dialog short enough to show the link preview without scrolling.
    auto *form = new QGridLayout(body);
    form->setContentsMargins(dialog_scaled(16), dialog_scaled(14), dialog_scaled(16), dialog_scaled(10));
    form->setHorizontalSpacing(dialog_scaled(12));
    form->setVerticalSpacing(dialog_scaled(8));
    form->setColumnStretch(1, 1);
    scroll->setWidget(content);
    FluentDialog::bodyLayout()->addWidget(scroll, 1);

    const auto danger = QString("color: %1; font-size: %2px; background: transparent;").arg(ui_tokens(this).danger).arg(dialog_scaled(12));
    // The text fields by the target field each one edits.
    std::vector<std::pair<QLineEdit *, QString>> text_fields;
    const auto add_field = [&](const QString &key, const char *label, QWidget *control) {
        const int row = form->rowCount();
        auto *caption = new CaptionLabel(tr_text(label), body);
        // One line at its own width: wrapped, the caption column would shrink to nothing.
        caption->setWordWrap(false);
        caption->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        form->addWidget(caption, row, 0, Qt::AlignLeft | Qt::AlignVCenter);
        form->addWidget(control, row, 1);
        if (!key.isEmpty()) {
            auto *error = new QLabel(body);
            error->setObjectName("upload_error_" + key);
            error->setWordWrap(true);
            error->setTextFormat(Qt::PlainText);
            set_own_style(error, danger);
            error->hide();
            errors.insert(key, error);
            form->addWidget(error, row + 1, 1);
        }
        if (auto *edit = qobject_cast<QLineEdit *>(control)) {
            edit->setAccessibleName(tr_text(label));
            if (!key.isEmpty()) text_fields.emplace_back(edit, key);
        }
    };
    const auto make_edit = [&](const char *name, const QString &placeholder = {}) {
        auto *edit = new LineEdit(body);
        edit->setObjectName(name);
        edit->setPlaceholderText(placeholder);
        return edit;
    };

    name_edit = make_edit("upload_name");
    add_field("name", "Name", name_edit);
    preset_combo = new ComboBox(body);
    preset_combo->setObjectName("upload_preset");
    for (int i = 0; i < PRESET_COUNT; ++i) preset_combo->addItem(tr_text(PRESETS[i].name), i);
    add_field({}, "Service", preset_combo);
    endpoint_edit = make_edit("upload_endpoint", "https://s3.us-east-1.amazonaws.com");
    add_field("endpoint", "Endpoint", endpoint_edit);
    region_edit = make_edit("upload_region", "us-east-1");
    add_field("region", "Region", region_edit);
    bucket_edit = make_edit("upload_bucket");
    add_field("bucket", "Bucket", bucket_edit);
    access_key_edit = make_edit("upload_access_key");
    add_field("access_key_id", "Access Key ID", access_key_edit);
    secret_edit = make_edit("upload_secret");
    secret_edit->setEchoMode(QLineEdit::Password);
    add_field("secret_access_key", "Secret Access Key", secret_edit);
    prefix_edit = make_edit("upload_prefix", "screenshots/{yyyy}/{MM}");
    add_field("path_prefix", "Path Prefix", prefix_edit);
    addressing_combo = new ComboBox(body);
    addressing_combo->setObjectName("upload_addressing");
    addressing_combo->addItem(tr_text("Path-style"), "path");
    addressing_combo->addItem(tr_text("Virtual-hosted"), "virtual");
    add_field({}, "Addressing", addressing_combo);
    public_edit = make_edit("upload_public", "https://cdn.example.com");
    add_field("public_url", "Public Address (optional)", public_edit);

    auto *preview = new ElidedLabel(body);
    preview->setObjectName("upload_preview");
    preview_label = preview;
    add_field({}, "Link Preview", preview_label);

    // A fixed three lines: a result never moves the buttons.
    test_label = new QLabel(body);
    test_label->setObjectName("upload_test_result");
    test_label->setWordWrap(true);
    test_label->setTextFormat(Qt::PlainText);
    test_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    test_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    form->addWidget(test_label, form->rowCount(), 1);
    outer->addStretch(1);

    test_button = new PushButton(tr_text("Test Upload"), this);
    test_button->setObjectName("upload_test");
    test_button->setAutoDefault(false);
    save_button = new PrimaryPushButton(tr_text("Save"), this);
    save_button->setObjectName("upload_save");
    save_button->setDefault(true);
    cancel_button = new PushButton(tr_text("Cancel"), this);
    cancel_button->setObjectName("upload_cancel");
    cancel_button->setAutoDefault(false);
    setFooterButtons({test_button}, {save_button, cancel_button});

    // The fields take the target; a new one starts from the first provider.
    filling_ = true;
    if (is_new) {
        preset_combo->setCurrentIndex(0);
        apply_preset(0);
    } else {
        name_edit->setText(initial.name);
        endpoint_edit->setText(initial.endpoint);
        region_edit->setText(initial.region);
        bucket_edit->setText(initial.bucket);
        access_key_edit->setText(initial.access_key_id);
        secret_edit->setText(initial.secret_access_key);
        prefix_edit->setText(initial.path_prefix);
        public_edit->setText(initial.public_url);
        addressing_combo->setCurrentIndex(initial.addressing == upload::Addressing::Virtual ? 1 : 0);
        preset_combo->setCurrentIndex(preset_of_endpoint(initial.endpoint));
        templated_region_ = initial.region;
    }
    filling_ = false;

    connect(preset_combo, &QComboBox::activated, this, [this](int index) { apply_preset(index); });
    for (const auto &[edit, key] : text_fields) {
        connect(edit, &QLineEdit::textChanged, this, [this, key] {
            // A field being fixed stops showing its old complaint.
            if (auto *label = errors.value(key)) label->hide();
            refresh_preview();
        });
    }
    connect(region_edit, &QLineEdit::textEdited, this, [this](const QString &region) {
        // The endpoint of a provider that carries the region follows it until the endpoint is edited by hand.
        const auto &preset = PRESETS[std::max(0, preset_combo->currentData().toInt())];
        if (!preset.region_in_endpoint || endpoint_edit->text().trimmed() != preset_endpoint(preset, templated_region_)) return;
        templated_region_ = region.trimmed();
        endpoint_edit->setText(preset_endpoint(preset, templated_region_));
    });
    connect(addressing_combo, &QComboBox::currentIndexChanged, this, [this] { refresh_preview(); });
    connect(save_button, &QPushButton::clicked, this, [this] {
        if (validate_fields()) accept();
    });
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
    connect(test_button, &QPushButton::clicked, this, &UploadTargetDialog::run_test);

    finish_controls();
    test_label->setFixedHeight(test_label->fontMetrics().height() * 3 + dialog_scaled(4));
    refresh_preview();

    const auto *shown_on = screen() ? screen() : QGuiApplication::primaryScreen();
    const auto available = shown_on->availableGeometry().size();
    const int width = std::min(dialog_scaled(600), available.width() - dialog_scaled(40));
    setFixedSize(width, std::min(height_showing(scroll, width), available.height() - dialog_scaled(60)));
}

upload::Target UploadTargetDialog::target() const {
    upload::Target result;
    result.id = initial_.id;
    result.name = name_edit->text().trimmed();
    result.endpoint = endpoint_edit->text().trimmed();
    result.region = region_edit->text().trimmed();
    result.bucket = bucket_edit->text().trimmed();
    result.access_key_id = access_key_edit->text().trimmed();
    result.secret_access_key = secret_edit->text().trimmed();
    result.path_prefix = prefix_edit->text().trimmed();
    result.public_url = public_edit->text().trimmed();
    result.addressing = addressing_combo->currentData().toString() == "virtual" ? upload::Addressing::Virtual : upload::Addressing::Path;
    return result;
}

void UploadTargetDialog::apply_preset(int index) {
    const auto &preset = PRESETS[std::clamp(index, 0, PRESET_COUNT - 1)];
    templated_region_ = QString::fromLatin1(preset.region);
    region_edit->setText(templated_region_);
    endpoint_edit->setText(preset_endpoint(preset, templated_region_));
    addressing_combo->setCurrentIndex(preset.virtual_host ? 1 : 0);
}

bool UploadTargetDialog::validate_fields() {
    for (auto *error : errors) {
        error->hide();
        error->clear();
    }
    const auto found = upload::validate(target(), others_);
    for (const auto &entry : found) {
        auto *label = errors.value(entry.field);
        // One complaint per field, the first.
        if (!label || !label->text().isEmpty()) continue;
        label->setText(tr_text(entry.message));
        label->show();
    }
    if (!found.empty()) {
        // The first field with a problem comes into view.
        if (auto *label = errors.value(found.front().field)) scroll->ensureWidgetVisible(label);
    }
    return found.empty();
}

void UploadTargetDialog::refresh_preview() {
    const auto current = target();
    bool ready = true;
    for (const auto &entry : upload::validate(current, {}))
        if (entry.field == "endpoint" || entry.field == "bucket" || entry.field == "public_url") ready = false;
    if (ready) preview_full_ = upload::public_link(current, upload::make_object_key(current.path_prefix, "Screenshot", "1a2b3c4d", ".png", QDateTime::currentDateTime()));
    else preview_full_ = tr_text("Fill in the endpoint and the bucket to see the link.");
    static_cast<ElidedLabel *>(preview_label)->set_full_text(preview_full_);
}

void UploadTargetDialog::set_test_text(const QString &text, bool failed) {
    set_own_style(test_label, QString("color: %1; font-size: %2px; background: transparent;").arg(failed ? ui_tokens(this).danger : ui_tokens(this).text_muted).arg(dialog_scaled(12)));
    test_label->setText(text);
}

void UploadTargetDialog::run_test() {
    if (!validate_fields()) return;
    test_button->setEnabled(false);
    set_test_text(tr_text("Uploading..."), false);
    const QPointer<UploadTargetDialog> self(this);
    upload::upload_test_image(target(), this, [self](const upload::Report &report) {
        if (!self) return;
        self->test_button->setEnabled(true);
        self->set_test_text(report.outcome.ok() ? tr_text("Test upload succeeded: %1").arg(report.link) : report.message, !report.outcome.ok());
    });
}
}
