// Explorer's "pin to screen" entry: the command line, the hand-over between processes, the registry
// verbs of the portable build and the preference file of the packaged one.
#include "core/explorer_menu.h"
#include "core/i18n.h"
#include "core/package_identity.h"
#include "core/pin_arguments.h"
#include "core/pin_inbox.h"
#include "core/pin_request_queue.h"
#include "core/text_file.h"
#include "settings/tool_settings.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <atomic>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace jietuba;
using explorer_menu::Menu;
using explorer_menu::Mode;

namespace {
// ---- Registry helpers ----

QString key_path(const QString &root, const QString &relative) { return root + "\\" + relative; }

bool key_exists(const QString &path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.toStdWString().c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

// The value of a key; name empty for its default value. Null when the key or value is missing.
QString key_value(const QString &path, const QString &name = {}) {
    wchar_t buffer[2048] = {};
    DWORD size = sizeof(buffer);
    const std::wstring wide_name = name.toStdWString();
    if (RegGetValueW(HKEY_CURRENT_USER, path.toStdWString().c_str(), name.isEmpty() ? nullptr : wide_name.c_str(), RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(buffer);
}

quint64 last_write(const QString &path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.toStdWString().c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    FILETIME time = {};
    RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &time);
    RegCloseKey(key);
    return (static_cast<quint64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

using Batches = QList<QStringList>;
Batches batches_of(std::initializer_list<QStringList> lists) { return Batches(lists); }

QString random_name() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

const QStringList IMAGE_EXTENSIONS = {"png", "jpg", "jpeg", "jpe", "jfif", "bmp", "dib", "gif", "webp", "tif", "tiff", "ico", "svg"};
const QStringList TEXT_EXTENSIONS = {"txt", "md", "markdown", "log", "csv", "json", "xml", "html", "htm"};

const QString LITE_EXE = "C:\\Apps\\jietuba_lite.exe";
const QString FULL_EXE = "C:\\Apps\\jietuba_pp.exe";

QStringList paths(const QStringList &names) {
    QStringList result;
    for (const auto &name : names) result << "C:\\Pictures\\" + name;
    return result;
}

// Holds a mutex on a thread of its own until told to let go; threads, not objects, own mutexes.
class RemoteHolder {
public:
    explicit RemoteHolder(const QString &instance_name) {
        thread_ = std::thread([this, instance_name] {
            PrimaryInstance primary(instance_name);
            held_ = primary.try_claim(0);
            ready_ = true;
            while (!stop_) Sleep(5);
            primary.release();
        });
        while (!ready_) Sleep(1);
    }
    bool held() const { return held_; }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    ~RemoteHolder() { stop(); }
private:
    std::thread thread_;
    std::atomic_bool ready_{false}, stop_{false}, held_{false};
};
}

namespace {
struct TestRoot {
    QString root = "Software\\jietuba_tests\\" + random_name();
    int notified = 0;
    explorer_menu::Environment environment;
    TestRoot() {
        environment.classes_root = root + "\\Classes";
        environment.notify = [this] { ++notified; };
    }
    ~TestRoot() {
        RegDeleteTreeW(HKEY_CURRENT_USER, root.toStdWString().c_str());
        // The shared parent goes too once nothing else is under it.
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\jietuba_tests");
    }
    QString verb(const QString &ext, const QString &exe = LITE_EXE) const { return key_path(environment.classes_root, explorer_menu::verb_key(ext, exe)); }
};
}

class ExplorerMenuTests : public QObject {
    Q_OBJECT
private:
    QString language_;
    bool had_packaged_override_ = false;
private slots:
    void initTestCase() { language_ = I18nManager::get_current_language(); }
    void cleanupTestCase() { I18nManager::load_language(language_); }
    void cleanup() { set_packaged_for_test(std::nullopt); }

    // ---- The command line ----

    void aPinOptionTakesTheFileAfterIt() {
        const auto args = parse_pin_arguments({"--pin", "C:\\Pictures\\a b.png"});
        QVERIFY(args.requested);
        QCOMPARE(args.files, QStringList({"C:\\Pictures\\a b.png"}));
    }

    void pinOptionsRepeatAndKeepTheirOrderAndDuplicates() {
        const auto args = parse_pin_arguments({"--pin", "C:\\a.png", "--other", "--pin", "D:/b.jpg", "--pin", "\\\\server\\share\\c.txt", "--pin", "C:\\a.png"});
        QVERIFY(args.requested);
        QCOMPARE(args.files, QStringList({"C:\\a.png", "D:/b.jpg", "\\\\server\\share\\c.txt", "C:\\a.png"}));
    }

    void withoutAPinOptionNothingIsRequested() {
        QVERIFY(!parse_pin_arguments({}).requested);
        QVERIFY(!parse_pin_arguments({"--video-recorder-worker", "x"}).requested);
        QVERIFY(parse_pin_arguments({"C:\\a.png"}).files.isEmpty());
    }

    void oneInvalidPathDiscardsTheWholeBatch() {
        const QStringList bad = {"relative\\a.png", "a.png", "C:a.png", "\\\\", "\\\\\\a", "C:\\a\"b.png", QString("C:\\a") + QChar(7) + ".png", QString("C:\\a") + QChar(0x7F), "--pin", ""};
        for (const auto &path : bad) {
            const auto args = parse_pin_arguments({"--pin", "C:\\good.png", "--pin", path});
            QVERIFY2(!args.requested && args.files.isEmpty(), qPrintable(path));
        }
        // A --pin with nothing after it.
        QVERIFY(!parse_pin_arguments({"--pin", "C:\\good.png", "--pin"}).requested);
    }

    void pinPathsAreDriveOrUncPaths() {
        QVERIFY(is_pin_path("C:\\a"));
        QVERIFY(is_pin_path("z:/a/b.png"));
        QVERIFY(is_pin_path("\\\\server\\share\\a"));
        QVERIFY(is_pin_path("\\\\?\\C:\\a"));
        QVERIFY(is_pin_path(QString("C:\\") + QString(QChar(0x56FE)) + ".png"));
        QVERIFY(!is_pin_path(QString(QChar(0x00E9)) + ":\\a"));
        QVERIFY(!is_pin_path(QString("C:\\") + QString(40000, 'a')));
    }

    // ---- The message ----

    void aRequestSurvivesPackingWithItsFilesInOrder() {
        const PinRequest request{"id-1", {"C:\\a.png", "D:\\" + QString(QChar(0x56FE)) + QString(QChar(0x7247)) + ".jpg", "\\\\s\\h\\c.txt"}};
        const auto data = pack_pin_request(request);
        const auto parsed = parse_pin_request(data);
        QVERIFY(parsed);
        QCOMPARE(parsed->id, QString("id-1"));
        QCOMPARE(parsed->files, request.files);
        // Plain UTF-8 JSON.
        QVERIFY(data.startsWith("{"));
        QVERIFY(data.contains("\"files\""));
    }

    void malformedRequestsAreRefused() {
        QVERIFY(!parse_pin_request({}));
        QVERIFY(!parse_pin_request("not json"));
        QVERIFY(!parse_pin_request("[]"));
        QVERIFY(!parse_pin_request("{\"files\":[\"C:\\\\a.png\"]}"));
        QVERIFY(!parse_pin_request("{\"id\":\"\",\"files\":[\"C:\\\\a.png\"]}"));
        QVERIFY(!parse_pin_request("{\"id\":\"x\",\"files\":[]}"));
        QVERIFY(!parse_pin_request("{\"id\":\"x\",\"files\":\"C:\\\\a.png\"}"));
        QVERIFY(!parse_pin_request("{\"id\":\"x\",\"files\":[5]}"));
        QVERIFY(!parse_pin_request("{\"id\":\"x\",\"files\":[\"C:\\\\a.png\",\"relative.png\"]}"));
        QVERIFY(!parse_pin_request("{\"id\":7,\"files\":[\"C:\\\\a.png\"]}"));
        const QByteArray long_id = QByteArray("{\"id\":\"") + QByteArray(65, 'a') + QByteArray("\",\"files\":[\"C:\\\\a.png\"]}");
        QVERIFY(!parse_pin_request(long_id));
        QJsonArray many;
        for (int i = 0; i < 4097; ++i) many.append("C:\\a.png");
        QVERIFY(!parse_pin_request(QJsonDocument(QJsonObject{{"id", "x"}, {"files", many}}).toJson()));
    }

    void newRequestIdsDiffer() { QVERIFY(new_pin_request_id() != new_pin_request_id()); }

    void recentIdsRememberTheLastSixtyFour() {
        RecentRequestIds ids;
        QVERIFY(ids.remember("a"));
        QVERIFY(!ids.remember("a"));
        for (int i = 0; i < 63; ++i) QVERIFY(ids.remember(QString::number(i)));
        // 64 are held: a is still the oldest of them.
        QVERIFY(!ids.remember("a"));
        QVERIFY(ids.remember("overflow"));
        // a went out to make room.
        QVERIFY(ids.remember("a"));
        QVERIFY(!ids.remember("overflow"));
    }

    void uniquePathsKeepTheOrderAndTheFirstSpelling() {
        QCOMPARE(unique_pin_paths({"C:\\b.png", "C:\\a.png", "c:/A.PNG", "C:\\b.png", "D:\\a.png"}), QStringList({"C:\\b.png", "C:\\a.png", "D:\\a.png"}));
        QVERIFY(unique_pin_paths({}).isEmpty());
    }

    // ---- The window of the primary instance ----

    void theInboxHandsRequestsToItsHandler() {
        const QString name = "inbox-" + random_name();
        PinInbox inbox(name);
        QVERIFY(inbox.valid());
        Batches got;
        inbox.set_handler([&got](const QStringList &files) { got << files; });
        const auto result = send_pin_request(name, {"r1", {"C:\\a.png", "C:\\b.txt"}}, 2000);
        QCOMPARE(result, SendResult::Delivered);
        QCOMPARE(got, batches_of({QStringList{"C:\\a.png", "C:\\b.txt"}}));
    }

    void aRequestSentAgainIsTakenButNotHandedOverTwice() {
        const QString name = "inbox-" + random_name();
        PinInbox inbox(name);
        int calls = 0;
        inbox.set_handler([&calls](const QStringList &) { ++calls; });
        const PinRequest request{"same", {"C:\\a.png"}};
        QCOMPARE(send_pin_request(name, request, 2000), SendResult::Delivered);
        QCOMPARE(send_pin_request(name, request, 2000), SendResult::Delivered);
        QCOMPARE(calls, 1);
        QCOMPARE(send_pin_request(name, {"other", {"C:\\a.png"}}, 2000), SendResult::Delivered);
        QCOMPARE(calls, 2);
    }

    void requestsBeforeTheHandlerWaitForIt() {
        const QString name = "inbox-" + random_name();
        PinInbox inbox(name);
        QCOMPARE(send_pin_request(name, {"1", {"C:\\one.png"}}, 2000), SendResult::Delivered);
        QCOMPARE(send_pin_request(name, {"2", {"C:\\two.png"}}, 2000), SendResult::Delivered);
        Batches got;
        inbox.set_handler([&got](const QStringList &files) { got << files; });
        QCOMPARE(got, batches_of({QStringList{"C:\\one.png"}, QStringList{"C:\\two.png"}}));
        QCOMPARE(send_pin_request(name, {"3", {"C:\\three.png"}}, 2000), SendResult::Delivered);
        QCOMPARE(got.size(), 3);
    }

    void aWindowOfAnotherNameIsNotFound() {
        const QString name = "inbox-" + random_name();
        PinInbox inbox(name);
        QCOMPARE(send_pin_request("inbox-" + random_name(), {"x", {"C:\\a.png"}}, 200), SendResult::NoWindow);
    }

    void messagesThatAreNotRequestsAreRefused() {
        const QString name = "inbox-" + random_name();
        PinInbox inbox(name);
        int calls = 0;
        inbox.set_handler([&calls](const QStringList &) { ++calls; });
        HWND window = FindWindowExW(HWND_MESSAGE, nullptr, L"jietuba.PinRequest", name.toStdWString().c_str());
        QVERIFY(window);
        const auto send = [window](ULONG_PTR magic, const QByteArray &payload) {
            COPYDATASTRUCT data = {magic, static_cast<DWORD>(payload.size()), const_cast<char *>(payload.constData())};
            return SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
        };
        const QByteArray good = pack_pin_request({"m1", {"C:\\a.png"}});
        QCOMPARE(send(0x4A544250, "garbage"), static_cast<LRESULT>(FALSE));
        QCOMPARE(send(1234, good), static_cast<LRESULT>(FALSE));
        QCOMPARE(calls, 0);
        QCOMPARE(send(0x4A544250, good), static_cast<LRESULT>(TRUE));
        QCOMPARE(calls, 1);
    }

    void theWindowIsMessageOnlyAndNamedForTheExecutable() {
        const QString name = own_instance_name();
        QVERIFY(name.endsWith(".exe"));
        QCOMPARE(name, name.toLower());
        const QString other = "inbox-" + random_name();
        PinInbox inbox(other);
        HWND window = FindWindowExW(HWND_MESSAGE, nullptr, L"jietuba.PinRequest", other.toStdWString().c_str());
        QVERIFY(window);
        // Its parent is the system's message window, not the desktop.
        HWND parent = GetAncestor(window, GA_PARENT);
        QVERIFY(parent && parent != GetDesktopWindow());
    }

    // ---- Which process is the primary instance ----

    void theMutexHasOneHolder() {
        const QString name = "primary-" + random_name();
        PrimaryInstance first(name);
        QVERIFY(!first.holds());
        QVERIFY(first.try_claim(0));
        QVERIFY(first.holds());
        // Claiming again does not nest.
        QVERIFY(first.try_claim(0));
        {
            RemoteHolder other(name);
            QVERIFY(!other.held());
        }
        first.release();
        QVERIFY(!first.holds());
        RemoteHolder after(name);
        QVERIFY(after.held());
        QVERIFY(!first.try_claim(0));
    }

    void aHolderThatEndsLeavesTheMutexToTheNextClaimer() {
        const QString name = "primary-" + random_name();
        // Never released, never deleted: the thread ending is what frees it.
        PrimaryInstance *abandoned = nullptr;
        std::thread([&] {
            abandoned = new PrimaryInstance(name);
            abandoned->try_claim(0);
        }).join();
        QVERIFY(abandoned->holds());
        PrimaryInstance next(name);
        QVERIFY(next.try_claim(0));
        delete abandoned;
    }

    void aClaimWaitsForTheHolderToLetGo() {
        const QString name = "primary-" + random_name();
        auto holder = std::make_unique<RemoteHolder>(name);
        QVERIFY(holder->held());
        PrimaryInstance waiting(name);
        QVERIFY(!waiting.try_claim(0));
        std::thread release([&holder] {
            Sleep(150);
            holder->stop();
        });
        QVERIFY(waiting.try_claim(3000));
        release.join();
    }

    // ---- Where a --pin start goes ----

    void withARunningInstanceTheFilesGoToItsWindow() {
        const QString name = "route-" + random_name();
        PinInbox inbox(name);
        Batches got;
        inbox.set_handler([&got](const QStringList &files) { got << files; });
        PrimaryInstance primary(name);
        QCOMPARE(route_pin_request(primary, {"C:\\a.png", "C:\\b.png"}, {}, name), PinRoute::Delivered);
        QCOMPARE(got, batches_of({QStringList{"C:\\a.png", "C:\\b.png"}}));
        // Delivering does not make this process the primary one.
        QVERIFY(!primary.holds());
    }

    void withNobodyRunningThisProcessBecomesThePrimaryInstance() {
        const QString name = "route-" + random_name();
        PrimaryInstance primary(name);
        QCOMPARE(route_pin_request(primary, {"C:\\a.png"}, {}, name), PinRoute::Primary);
        QVERIFY(primary.holds());
    }

    void aHolderThatNeverShowsAWindowTimesOutTheRoute() {
        const QString name = "route-" + random_name();
        RemoteHolder holder(name);
        QVERIFY(holder.held());
        PrimaryInstance primary(name);
        PinDelivery quick;
        quick.deadline_ms = 300;
        quick.poll_ms = 20;
        QElapsedTimer waited;
        waited.start();
        QCOMPARE(route_pin_request(primary, {"C:\\a.png"}, quick, name), PinRoute::TimedOut);
        QVERIFY(waited.elapsed() >= 280);
        QVERIFY(waited.elapsed() < 3000);
        QVERIFY(!primary.holds());
    }

    void aStartingInstanceGetsTheFilesOnceItsWindowExists() {
        const QString name = "route-" + random_name();
        // Holds the mutex already, as an instance that is still starting does.
        RemoteHolder holder(name);
        QVERIFY(holder.held());
        std::atomic<int> route{-1};
        std::thread sender([&route, name] {
            PrimaryInstance primary(name);
            route = static_cast<int>(route_pin_request(primary, {"C:\\late.png"}, {5000, 1000, 20, {}}, name));
        });
        QTest::qWait(200);
        QCOMPARE(route.load(), -1);
        PinInbox inbox(name);
        Batches got;
        inbox.set_handler([&got](const QStringList &files) { got << files; });
        // The sender's message needs this thread's message loop.
        QTRY_COMPARE_WITH_TIMEOUT(route.load(), static_cast<int>(PinRoute::Delivered), 5000);
        sender.join();
        QCOMPARE(got, batches_of({QStringList{"C:\\late.png"}}));
    }

    void withAdminModeTheElevatedInstanceIsStartedFirstAndItsWindowAwaited() {
        const QString name = "route-" + random_name();
        PrimaryInstance primary(name);
        std::unique_ptr<PinInbox> elevated;
        Batches got;
        int starts = 0;
        PinDelivery delivery;
        delivery.start_elevated = [&] {
            ++starts;
            // The mutex was let go for the elevated instance before this was called.
            RemoteHolder probe(name);
            if (!probe.held()) return false;
            probe.stop();
            elevated = std::make_unique<PinInbox>(name);
            elevated->set_handler([&got](const QStringList &files) { got << files; });
            return true;
        };
        QCOMPARE(route_pin_request(primary, {"C:\\a.png"}, delivery, name), PinRoute::Delivered);
        QCOMPARE(starts, 1);
        QCOMPARE(got, batches_of({QStringList{"C:\\a.png"}}));
        QVERIFY(!primary.holds());
    }

    void whenTheElevatedInstanceCannotStartThisProcessTakesOver() {
        const QString name = "route-" + random_name();
        PrimaryInstance primary(name);
        int starts = 0;
        PinDelivery delivery;
        delivery.start_elevated = [&starts] {
            ++starts;
            return false;
        };
        QCOMPARE(route_pin_request(primary, {"C:\\a.png"}, delivery, name), PinRoute::Primary);
        QCOMPARE(starts, 1);
        QVERIFY(primary.holds());
    }

    // ---- Batches ----

    void requestsWithinTheQuietTimeArriveAsOneBatch() {
        Batches batches;
        PinRequestQueue queue([&batches](const QStringList &files) { batches << files; }, 60);
        queue.add({"C:\\1.png"});
        QTest::qWait(20);
        queue.add({"C:\\2.png", "C:\\1.png"});
        QTest::qWait(20);
        queue.add({"C:\\3.TXT", "c:/2.png"});
        QCOMPARE(batches.size(), 0);
        QTRY_COMPARE(batches.size(), 1);
        QCOMPARE(batches[0], QStringList({"C:\\1.png", "C:\\2.png", "C:\\3.TXT"}));
        // Later requests start a batch of their own.
        queue.add({"C:\\4.png"});
        QTRY_COMPARE(batches.size(), 2);
        QCOMPARE(batches[1], QStringList({"C:\\4.png"}));
        QTest::qWait(150);
        QCOMPARE(batches.size(), 2);
    }

    void theQuietTimeIsThreeHundredMilliseconds() { QCOMPARE(PinRequestQueue::QUIET_MS, 300); }

    // ---- Text files ----

    void textIsReadByItsByteOrderMarkOrAsUtf8() {
        const QString text = QString("abc ") + QChar(0x4E2D) + QChar(0x6587) + " " + QChar(0x00E9);
        const QByteArray utf8 = text.toUtf8();
        QCOMPARE(decode_text_bytes(utf8), text);
        QCOMPARE(decode_text_bytes("\xEF\xBB\xBF" + utf8), text);
        QByteArray le("\xFF\xFE", 2), be("\xFE\xFF", 2), le32("\xFF\xFE\x00\x00", 4);
        for (const QChar c : text) {
            le.append(static_cast<char>(c.unicode() & 0xFF)).append(static_cast<char>(c.unicode() >> 8));
            be.append(static_cast<char>(c.unicode() >> 8)).append(static_cast<char>(c.unicode() & 0xFF));
            le32.append(static_cast<char>(c.unicode() & 0xFF)).append(static_cast<char>(c.unicode() >> 8)).append(QByteArray(2, '\0'));
        }
        QCOMPARE(decode_text_bytes(le), text);
        QCOMPARE(decode_text_bytes(be), text);
        QCOMPARE(decode_text_bytes(le32), text);
        QCOMPARE(decode_text_bytes({}), QString());
    }

    void bytesThatAreNotUtf8AreReadInTheFallbackEncoding() {
        // 0xE9 alone is not UTF-8; in Latin 1 it is an e with an acute accent.
        QCOMPARE(decode_text_bytes("caf\xE9", QStringConverter::Latin1), QString("caf") + QChar(0x00E9));
        QVERIFY(!decode_text_bytes("caf\xE9", QStringConverter::Latin1).contains(QChar::ReplacementCharacter));
        // A file that is valid UTF-8 never takes the fallback.
        QCOMPARE(decode_text_bytes(QString(QChar(0x00E9)).toUtf8(), QStringConverter::Latin1), QString(QChar(0x00E9)));
    }

    void aFileLongerThanTheLimitIsCutAtACharacter() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto write = [&dir](const QString &name, const QByteArray &data) {
            QFile file(dir.filePath(name));
            if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) return QString();
            return dir.filePath(name);
        };
        const QString han = QString(QChar(0x4E2D)) + QChar(0x6587) + QChar(0x4E2D) + QChar(0x6587);
        // "ab" and four three-byte characters: 14 bytes. Seven bytes end inside the second character.
        const auto cut = load_text_file(write("cut.txt", "ab" + han.toUtf8()), QStringConverter::Latin1, 7);
        QVERIFY(cut);
        QVERIFY(cut->truncated);
        QCOMPARE(cut->text, QString("ab") + QChar(0x4E2D));
        // Eight bytes end between characters.
        const auto whole = load_text_file(write("whole.txt", "ab" + han.toUtf8()), QStringConverter::Latin1, 8);
        QVERIFY(whole && whole->truncated);
        QCOMPARE(whole->text, QString("ab") + QChar(0x4E2D) + QChar(0x6587));
        // Exactly the limit is not too long.
        const auto exact = load_text_file(write("exact.txt", "12345"), QStringConverter::Latin1, 5);
        QVERIFY(exact && !exact->truncated);
        QCOMPARE(exact->text, QString("12345"));
        const auto longer = load_text_file(write("longer.txt", "123456"), QStringConverter::Latin1, 5);
        QVERIFY(longer && longer->truncated);
        QCOMPARE(longer->text, QString("12345"));
        // The default limit is 64 KiB.
        QCOMPARE(TEXT_FILE_LIMIT, qint64(64 * 1024));
        const auto big = load_text_file(write("big.log", QByteArray(TEXT_FILE_LIMIT + 10, 'x')));
        QVERIFY(big && big->truncated);
        QCOMPARE(big->text.size(), int(TEXT_FILE_LIMIT));
        const auto empty = load_text_file(write("empty.txt", {}));
        QVERIFY(empty && empty->text.isEmpty() && !empty->truncated);
        QVERIFY(!load_text_file(dir.filePath("missing.txt")));
        QVERIFY(!load_text_file(dir.path()));
    }

    void anHtmlFileKeepsItsMarkupAndHasPlainText() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (const char *name : {"page.html", "PAGE.HTM", "page.txt"}) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("<p>Hello <b>bold</b> world</p>");
            file.close();
            const auto loaded = load_text_file(dir.filePath(name));
            QVERIFY(loaded);
            if (QString(name).endsWith("txt")) {
                QCOMPARE(loaded->text, QString("<p>Hello <b>bold</b> world</p>"));
                QVERIFY(loaded->html.isEmpty());
            } else {
                QCOMPARE(loaded->html, QString("<p>Hello <b>bold</b> world</p>"));
                QCOMPARE(loaded->text.trimmed(), QString("Hello bold world"));
            }
        }
    }

    // ---- The menu text and its keys ----

    void theMenuTitleFollowsTheInterfaceLanguage() {
        const std::pair<const char *, QString> titles[] = {
            {"en", "Pin to Screen with jietuba"},
            {"zh", QString::fromUtf8("用截图吧钉到屏幕")},
            {"zh_TW", QString::fromUtf8("用截圖吧釘到螢幕")},
            {"ja", QString::fromUtf8("jietuba で画面にピン留め")},
            {"ko", QString::fromUtf8("jietuba로 화면에 고정")},
        };
        for (const auto &[code, title] : titles) {
            QVERIFY(I18nManager::load_language(code));
            QCOMPARE(explorer_menu::menu_title(), title);
        }
    }

    void modesRoundTripThroughTheirWords() {
        QCOMPARE(explorer_menu::mode_from_word("off"), Mode::Off);
        QCOMPARE(explorer_menu::mode_from_word("images"), Mode::Images);
        QCOMPARE(explorer_menu::mode_from_word("images_text"), Mode::ImagesAndText);
        QCOMPARE(explorer_menu::mode_from_word("something else"), Mode::Off);
        QCOMPARE(explorer_menu::mode_from_word(""), Mode::Off);
        QCOMPARE(explorer_menu::word_of(Mode::Off), QString("off"));
        QCOMPARE(explorer_menu::word_of(Mode::Images), QString("images"));
        QCOMPARE(explorer_menu::word_of(Mode::ImagesAndText), QString("images_text"));
    }

    void extensionsAreToldApartByTheContractLists() {
        for (const auto &ext : IMAGE_EXTENSIONS) {
            QVERIFY(explorer_menu::is_image_extension(ext));
            QVERIFY(explorer_menu::is_image_extension(ext.toUpper()));
            QVERIFY(!explorer_menu::is_text_extension(ext));
        }
        for (const auto &ext : TEXT_EXTENSIONS) {
            QVERIFY(explorer_menu::is_text_extension(ext));
            QVERIFY(explorer_menu::is_text_extension(ext.toUpper()));
            QVERIFY(!explorer_menu::is_image_extension(ext));
        }
        QVERIFY(!explorer_menu::is_image_extension("exe"));
        QVERIFY(!explorer_menu::is_text_extension(""));
        QVERIFY(!explorer_menu::is_image_extension(".png"));
    }

    void verbsAreNamedPerExecutable() {
        QCOMPARE(explorer_menu::verb_name(LITE_EXE), QString("jietuba.jietuba_lite.Pin"));
        QCOMPARE(explorer_menu::verb_name("C:\\Other\\JIETUBA_PP.EXE"), QString("jietuba.jietuba_pp.Pin"));
        QCOMPARE(explorer_menu::verb_key("png", FULL_EXE), QString("SystemFileAssociations\\.png\\shell\\jietuba.jietuba_pp.Pin"));
    }

    // ---- The registry ----

    void withTheMenuOffNothingIsWrittenOrAnnounced() {
        TestRoot test;
        QVERIFY(!explorer_menu::reconcile_registry({Mode::Off, "Pin it", LITE_EXE}, test.environment));
        QCOMPARE(test.notified, 0);
        QVERIFY(!key_exists(test.root));
    }

    void picturesGetAVerbWithTitleIconAndCommand() {
        TestRoot test;
        const Menu menu{Mode::Images, "Pin it here", LITE_EXE};
        QVERIFY(explorer_menu::reconcile_registry(menu, test.environment));
        QCOMPARE(test.notified, 1);
        for (const auto &ext : IMAGE_EXTENSIONS) {
            const QString verb = test.verb(ext);
            QCOMPARE(key_value(verb), QString("Pin it here"));
            QCOMPARE(key_value(verb, "Icon"), QString("\"C:\\Apps\\jietuba_lite.exe\",0"));
            QCOMPARE(key_value(verb, "MultiSelectModel"), QString("Document"));
            QCOMPARE(key_value(verb, "JietubaExe"), LITE_EXE);
            QCOMPARE(key_value(verb + "\\command"), QString("\"C:\\Apps\\jietuba_lite.exe\" --pin \"%1\""));
        }
        for (const auto &ext : TEXT_EXTENSIONS) QVERIFY2(!key_exists(test.verb(ext)), qPrintable(ext));
        // The verb is named after the executable and sits where Explorer looks for a type's verbs.
        QVERIFY(key_exists(key_path(test.environment.classes_root, "SystemFileAssociations\\.png\\shell\\jietuba.jietuba_lite.Pin\\command")));
    }

    void picturesAndTextFilesGetAVerbEach() {
        TestRoot test;
        QVERIFY(explorer_menu::reconcile_registry({Mode::ImagesAndText, "Pin it", LITE_EXE}, test.environment));
        for (const auto &ext : IMAGE_EXTENSIONS + TEXT_EXTENSIONS) QCOMPARE(key_value(test.verb(ext)), QString("Pin it"));
        QCOMPARE(test.notified, 1);
    }

    void reconcilingAgainWritesAndAnnouncesNothing() {
        TestRoot test;
        const Menu menu{Mode::ImagesAndText, "Pin it", LITE_EXE};
        QVERIFY(explorer_menu::reconcile_registry(menu, test.environment));
        QMap<QString, quint64> written;
        for (const auto &ext : IMAGE_EXTENSIONS + TEXT_EXTENSIONS) written[ext] = last_write(test.verb(ext));
        QTest::qWait(60);
        QVERIFY(!explorer_menu::reconcile_registry(menu, test.environment));
        QCOMPARE(test.notified, 1);
        for (const auto &ext : IMAGE_EXTENSIONS + TEXT_EXTENSIONS) QVERIFY2(last_write(test.verb(ext)) == written[ext], qPrintable(ext));
    }

    void widerModesAddKeysAndLeaveTheRightOnesAlone() {
        TestRoot test;
        QVERIFY(explorer_menu::reconcile_registry({Mode::Images, "Pin it", LITE_EXE}, test.environment));
        const quint64 png_written = last_write(test.verb("png"));
        QTest::qWait(60);
        QVERIFY(explorer_menu::reconcile_registry({Mode::ImagesAndText, "Pin it", LITE_EXE}, test.environment));
        QCOMPARE(test.notified, 2);
        QVERIFY(key_exists(test.verb("txt")));
        QVERIFY(key_exists(test.verb("html")));
        QVERIFY(last_write(test.verb("png")) == png_written);
    }

    void narrowerModesRemoveTheirKeysAndNothingElse() {
        TestRoot test;
        // Something of the system's or another program's beside ours, in the same type's key.
        HKEY foreign = nullptr;
        const QString foreign_path = key_path(test.environment.classes_root, "SystemFileAssociations\\.txt\\shell\\other.verb");
        QCOMPARE(RegCreateKeyExW(HKEY_CURRENT_USER, foreign_path.toStdWString().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &foreign, nullptr), static_cast<LONG>(ERROR_SUCCESS));
        RegCloseKey(foreign);
        QVERIFY(explorer_menu::reconcile_registry({Mode::ImagesAndText, "Pin it", LITE_EXE}, test.environment));
        QVERIFY(explorer_menu::reconcile_registry({Mode::Images, "Pin it", LITE_EXE}, test.environment));
        QCOMPARE(test.notified, 2);
        QVERIFY(!key_exists(test.verb("txt")));
        QVERIFY(!key_exists(test.verb("json")));
        QVERIFY(key_exists(foreign_path));
        QVERIFY(key_exists(test.verb("png")));
        // Types that held only our verb lose their keys altogether.
        QVERIFY(!key_exists(key_path(test.environment.classes_root, "SystemFileAssociations\\.json")));
        QVERIFY(key_exists(key_path(test.environment.classes_root, "SystemFileAssociations\\.txt\\shell")));
        QVERIFY(explorer_menu::reconcile_registry({Mode::Off, "Pin it", LITE_EXE}, test.environment));
        for (const auto &ext : IMAGE_EXTENSIONS) QVERIFY2(!key_exists(key_path(test.environment.classes_root, "SystemFileAssociations\\." + ext)), qPrintable(ext));
        QVERIFY(key_exists(foreign_path));
        QCOMPARE(test.notified, 3);
        // Turned off again: nothing left to remove, nothing announced.
        QVERIFY(!explorer_menu::reconcile_registry({Mode::Off, "Pin it", LITE_EXE}, test.environment));
        QCOMPARE(test.notified, 3);
    }

    void aNewTitleOrPlaceIsWrittenOverTheOldOne() {
        TestRoot test;
        QVERIFY(explorer_menu::reconcile_registry({Mode::Images, "Old title", LITE_EXE}, test.environment));
        QVERIFY(explorer_menu::reconcile_registry({Mode::Images, "New title", LITE_EXE}, test.environment));
        QCOMPARE(key_value(test.verb("gif")), QString("New title"));
        QCOMPARE(test.notified, 2);
        const QString moved = "D:\\Elsewhere\\jietuba_lite.exe";
        QVERIFY(explorer_menu::reconcile_registry({Mode::Images, "New title", moved}, test.environment));
        QCOMPARE(key_value(test.verb("gif"), "Icon"), QString("\"D:\\Elsewhere\\jietuba_lite.exe\",0"));
        QCOMPARE(key_value(test.verb("gif") + "\\command"), QString("\"D:\\Elsewhere\\jietuba_lite.exe\" --pin \"%1\""));
        QCOMPARE(key_value(test.verb("gif"), "JietubaExe"), moved);
        QCOMPARE(test.notified, 3);
    }

    void aChangedValueIsPutBack() {
        TestRoot test;
        const Menu menu{Mode::Images, "Pin it", LITE_EXE};
        QVERIFY(explorer_menu::reconcile_registry(menu, test.environment));
        const QString command = test.verb("png") + "\\command";
        HKEY key = nullptr;
        QCOMPARE(RegOpenKeyExW(HKEY_CURRENT_USER, command.toStdWString().c_str(), 0, KEY_WRITE, &key), static_cast<LONG>(ERROR_SUCCESS));
        const std::wstring wrong = L"notepad.exe";
        RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(wrong.c_str()), static_cast<DWORD>((wrong.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        QVERIFY(explorer_menu::reconcile_registry(menu, test.environment));
        QCOMPARE(key_value(command), QString("\"C:\\Apps\\jietuba_lite.exe\" --pin \"%1\""));
    }

    void theTwoBuildsKeepVerbsOfTheirOwn() {
        TestRoot test;
        QVERIFY(explorer_menu::reconcile_registry({Mode::Images, "Pin lite", LITE_EXE}, test.environment));
        QVERIFY(explorer_menu::reconcile_registry({Mode::ImagesAndText, "Pin full", FULL_EXE}, test.environment));
        QCOMPARE(key_value(test.verb("png", LITE_EXE)), QString("Pin lite"));
        QCOMPARE(key_value(test.verb("png", FULL_EXE)), QString("Pin full"));
        QVERIFY(!key_exists(test.verb("txt", LITE_EXE)));
        QVERIFY(key_exists(test.verb("txt", FULL_EXE)));
        // Switching one off leaves the other.
        QVERIFY(explorer_menu::reconcile_registry({Mode::Off, "Pin full", FULL_EXE}, test.environment));
        QVERIFY(key_exists(test.verb("png", LITE_EXE)));
        QVERIFY(!key_exists(test.verb("png", FULL_EXE)));
    }

    void theSavedChoiceMakesTheMenu() {
        TestRoot test;
        QTemporaryDir dir;
        QSettings store(dir.filePath("settings.ini"), QSettings::IniFormat);
        ToolSettingsManager config(store);
        QCOMPARE(config.get_explorer_context_menu(), QString("off"));
        QVERIFY(I18nManager::load_language("en"));
        explorer_menu::sync(config, test.environment);
        QVERIFY(!key_exists(test.root));
        config.set_explorer_context_menu("images_text");
        explorer_menu::sync(config, test.environment);
        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        const QString verb = test.verb("png", exe);
        QCOMPARE(key_value(verb), QString("Pin to Screen with jietuba"));
        QVERIFY(key_exists(test.verb("md", exe)));
        // A new language rewrites the text.
        QVERIFY(I18nManager::load_language("zh"));
        explorer_menu::sync(config, test.environment);
        QCOMPARE(key_value(verb), QString::fromUtf8("用截图吧钉到屏幕"));
        config.set_explorer_context_menu("images");
        explorer_menu::sync(config, test.environment);
        QVERIFY(!key_exists(test.verb("md", exe)));
        QVERIFY(key_exists(verb));
        config.set_explorer_context_menu("off");
        explorer_menu::sync(config, test.environment);
        QVERIFY(!key_exists(verb));
        QVERIFY(!key_exists(test.root + "\\Classes\\SystemFileAssociations\\.png"));
    }

    // ---- The packaged build ----

    void thePackagedBuildWritesThePreferenceFileNotTheRegistry() {
        TestRoot test;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        test.environment.preference_path = dir.filePath("LocalState/explorer_menu.txt");
        set_packaged_for_test(true);
        explorer_menu::apply({Mode::ImagesAndText, "Pin it", FULL_EXE}, test.environment);
        QFile file(test.environment.preference_path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("images_text\nPin it\njietuba_pp.exe\n"));
        file.close();
        QVERIFY(!key_exists(test.root));
        QCOMPARE(test.notified, 0);
        // Turned off, the command hides itself.
        explorer_menu::apply({Mode::Off, "Pin it", FULL_EXE}, test.environment);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("off\nPin it\njietuba_pp.exe\n"));
        file.close();
        // The title is one line, whatever it holds, and any language survives as UTF-8.
        explorer_menu::apply({Mode::Images, QString::fromUtf8("用截图吧\n钉到屏幕\r"), FULL_EXE}, test.environment);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QString::fromUtf8("images\n用截图吧 钉到屏幕 \njietuba_pp.exe\n").toUtf8());
    }

    void thePreferenceFileIsNotRewrittenWhenItIsRight() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("explorer_menu.txt");
        const Menu menu{Mode::Images, "Pin it", LITE_EXE};
        QVERIFY(explorer_menu::write_preference_file(menu, path));
        const auto first = QFileInfo(path).lastModified();
        QTest::qWait(50);
        QVERIFY(explorer_menu::write_preference_file(menu, path));
        QCOMPARE(QFileInfo(path).lastModified(), first);
        QVERIFY(!explorer_menu::write_preference_file(menu, dir.filePath("no/such\\\\:folder/file.txt")));
    }

    void withoutAPackageThereIsNoPreferenceFile() {
        set_packaged_for_test(false);
        QVERIFY(explorer_menu::preference_file_path().isEmpty());
    }
};

QTEST_MAIN(ExplorerMenuTests)
#include "test_explorer_menu.moc"
