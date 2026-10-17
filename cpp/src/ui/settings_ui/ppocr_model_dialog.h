#pragma once
#include <QDialog>
#include <QJsonObject>
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
class ModelDownloads;
class ModelRow;

// "Chinese, Japanese, English" for catalog language codes.
QString ppocr_language_names(const QStringList &codes);

class PpocrModelDialog : public QDialog {
    Q_OBJECT
public:
    explicit PpocrModelDialog(QWidget *parent = nullptr, ModelDownloads *downloads = nullptr);
    ModelDownload *download() const;
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
    void set_clock_for_test(std::function<double()> clock);
    void reject() override;
    void done(int result) override;
signals:
    void models_changed();
    // A model was deleted; models_changed follows.
    void model_removed(const QString &model_id);
    void import_done(const QJsonObject &model, const QString &error);
private:
    void showEvent(QShowEvent *event) override;
    void subscribe();
    void sync_download();
    // Rebuilds the rows from the catalog and the installed models, keeping the scroll position.
    void fill();
    void fit_height();
    void start_download(const QString &catalog_id);
    void on_progress();
    void add_local();
    void on_imported(const QJsonObject &model, const QString &error);
    void remove_model(const QJsonObject &model);
    void open_folder();
    ModelDownloads *downloads_;
    QWidget *rows_;
    QScrollArea *scroll_;
    QVBoxLayout *rows_layout_;
    QLabel *status_;
    QPushButton *add_btn_;
    ModelRow *download_row_ = nullptr;
    bool subscribed_ = false;
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
