#pragma once
#include <QObject>
#include <QVariant>
#include <QWidget>
#include <optional>

namespace jietuba {
class ToolSettingsManager;
qint64 python_round(double value);
// Python a // b for ints: rounds toward negative infinity.
inline int python_floordiv(int a, int b) { const int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
class UIScaleManager : public QObject {
    Q_OBJECT
public:
    static constexpr int PERCENT_OPTIONS[] = {80, 90, 100, 110, 125, 150, 175, 200};
    explicit UIScaleManager(QString setting = "ui_scale_percent", QObject *parent = nullptr);
    static UIScaleManager &instance();
    void init(ToolSettingsManager *config);
    int percent() const { return percent_; }
    double factor() const { return percent_ / 100.0; }
    int px(double base) const;
    double pxf(double base) const { return base * factor(); }
    bool set_percent(const QVariant &value);
    static int normalize_percent(const QVariant &value);
    void reset_to_default() { set_percent(100); }
signals:
    void scale_changed();
private:
    QString setting_;
    ToolSettingsManager *config_ = nullptr;
    int percent_ = 100;
};
class DialogScaleManager : public UIScaleManager {
public:
    DialogScaleManager() : UIScaleManager("dialog_scale_percent") {}
    static DialogScaleManager &instance();
};
int recommended_scale_percent(QVariant system_dpi = {});
std::optional<int> apply_first_run_scale_defaults(ToolSettingsManager &config, QVariant system_dpi = {});
UIScaleManager &get_ui_scale();
DialogScaleManager &get_dialog_scale();
int scaled(double base);
double scaled_f(double base);
double scale_factor();
int dialog_scaled(double base);
double dialog_scaled_f(double base);
int widget_scaled(const QWidget *widget, double base);
void scale_dialog_font(QWidget &widget);
void configure_dialog_control(QWidget &widget);
void configure_dialog_controls(QWidget &root);
}
