#pragma once
#include <QDialog>
#include <QJsonObject>
#include <deque>
#include <functional>
#include <optional>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

// The PP-OCR model manager: downloads catalog models, adds local ones and deletes them.
// File changes take effect at once (downloads and copies take time, so they do not wait for
// Apply); which model to use is still chosen on the settings page and saved with Apply.
namespace jietuba {
class ModelDownload;
class ModelRow;

// "Chinese, Japanese, English" for catalog language codes.
QString ppocr_language_names(const QStringList &codes);

class PpocrModelDialog : public QDialog {
    Q_OBJECT
public:
    explicit PpocrModelDialog(QWidget *parent = nullptr);
    ModelDownload *download() const { return download_; }
    // The rows' titles, details and action texts, and the downloading row's progress, for tests.
    struct RowInfo {
        QString title, detail, action;
        QPushButton *button;
        int progress = -1;  // per mille; -1 when the row is not downloading
        QString progress_text;
    };
    QVector<RowInfo> rows() const;
    QString status_text() const;
    void show_status(const QString &text);
    // Tests replace the clock the speed is measured with (seconds).
    void set_clock_for_test(std::function<double()> clock) { clock_ = std::move(clock); }
    void reject() override;
signals:
    void models_changed();
    // A model was deleted; models_changed follows.
    void model_removed(const QString &model_id);
    void import_done(const QJsonObject &model, const QString &error);
private:
    // Rebuilds the rows from the catalog and the installed models, keeping the scroll position.
    void fill();
    void fit_height();
    void start_download(const QString &catalog_id, const QString &name);
    void on_progress(qint64 received, qint64 total);
    void on_downloaded(const QJsonObject &model);
    void on_download_stopped();
    void add_local();
    void on_imported(const QJsonObject &model, const QString &error);
    void remove_model(const QJsonObject &model);
    void open_folder();
    ModelDownload *download_;
    QWidget *rows_;
    QScrollArea *scroll_;
    QVBoxLayout *rows_layout_;
    QLabel *status_;
    QPushButton *add_btn_;
    QString download_name_;
    QString downloading_id_;
    ModelRow *download_row_ = nullptr;
    struct Progress {
        qint64 received, total;
        std::optional<double> speed;  // bytes per second; nullopt until it can be measured
    };
    std::optional<Progress> last_progress_;
    std::deque<std::pair<double, qint64>> samples_;
    std::function<double()> clock_;
    bool close_after_download_ = false;
    bool importing_ = false;
};

// Picks the detection model, recognition model and optional dictionary, and names the model.
class PpocrImportForm : public QDialog {
    Q_OBJECT
public:
    explicit PpocrImportForm(QWidget *parent = nullptr);
    QString name() const;
    QString det() const;
    QString rec() const;
    QString dict() const;
private:
    void browse(QLineEdit *edit, const QString &filter);
    void update_ok();
    QLineEdit *name_;
    QLineEdit *det_;
    QLineEdit *rec_;
    QLineEdit *dict_;
    QPushButton *ok_;
};
}
