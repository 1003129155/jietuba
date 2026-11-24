#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <array>
#include <optional>
#include <stdexcept>

namespace jietuba {
// Raised when GitHub or the helper does not return a usable release.
struct UpdateCheckError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ReleaseInfo {
    QString tag_name;
    QString title;
    QString notes;
    QString url;
    std::optional<bool> available;
    QString asset_name;
    QString arch;
    QJsonObject artifact;
    // When the release became public (ISO 8601); empty when the helper does not say.
    QString published_at;
    bool operator==(const ReleaseInfo &other) const {
        return tag_name == other.tag_name && title == other.title && notes == other.notes && url == other.url && available == other.available &&
               asset_name == other.asset_name && arch == other.arch && artifact == other.artifact && published_at == other.published_at;
    }
    // The same download: what an earlier download of the other one can install.
    bool same_package(const ReleaseInfo &other) const { return tag_name == other.tag_name && asset_name == other.asset_name && arch == other.arch; }
};

// A numeric version from tags such as "v2.1" or "release-2.1"; throws UpdateCheckError.
std::array<int, 4> comparable_version(const QString &value);
bool is_newer_version(const QString &remote_version, const QString &current_version);
// "2.2.5" from tags such as "release-2.2.5"; the tag itself when it holds no version.
QString version_text(const QString &tag);
ReleaseInfo parse_release_payload(const QByteArray &payload);
// The release matching this executable from the helper's "check" output; throws UpdateCheckError.
ReleaseInfo release_from_check_output(const QByteArray &output, int exit_code);

// Asks the helper for the latest release without blocking the GUI thread.
class GitHubReleaseChecker : public QObject {
    Q_OBJECT
public:
    explicit GitHubReleaseChecker(QObject *parent = nullptr, int timeout_ms = 10000);
    ~GitHubReleaseChecker() override;
    bool is_checking() const { return process_ != nullptr; }
    bool check();
signals:
    void release_found(const ReleaseInfo &release);
    void failed(const QString &reason);
private:
    void finish(int exit_code, QProcess::ExitStatus status);
    void stop(const QString &error);
    int timeout_ms_;
    QPointer<QProcess> process_;
    QTimer timer_;
    QString helper_;
};
}

Q_DECLARE_METATYPE(jietuba::ReleaseInfo)
