// The packaged Explorer command: its preference file, selection filter and command line (plain
// logic, tested against hand-written expectations), the manifest that registers it, and the built
// DLL's COM surface. Nothing is registered with the system; the DLL is only loaded into this process.
#include "explorer_command_core.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

using namespace jietuba::explorer_command;
using jietuba::explorer_menu::Mode;

namespace {
std::string utf8(const char *text) { return text; }

std::vector<std::wstring> command_line_arguments(const std::wstring &line) {
    int count = 0;
    wchar_t **argv = CommandLineToArgvW(line.c_str(), &count);
    std::vector<std::wstring> arguments;
    for (int i = 0; i < count; ++i)
        arguments.emplace_back(argv[i]);
    LocalFree(argv);
    return arguments;
}

QString contract_extension_key(const wchar_t *extension) { return QString::fromWCharArray(extension); }

QSet<QString> contract_extensions() {
    QSet<QString> all;
    for (const wchar_t *extension : jietuba::explorer_menu::IMAGE_EXTENSIONS)
        all.insert(contract_extension_key(extension));
    for (const wchar_t *extension : jietuba::explorer_menu::TEXT_EXTENSIONS)
        all.insert(contract_extension_key(extension));
    return all;
}

struct Manifest {
    QString text;
    QStringList item_types;
    QStringList verb_clsids;
    QString class_id;
    QString class_path;
};

Manifest parse_manifest(const QString &text) {
    Manifest manifest;
    manifest.text = text;
    QXmlStreamReader xml(text);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        if (xml.qualifiedName() == QLatin1String("desktop5:ItemType"))
            manifest.item_types << xml.attributes().value(QLatin1String("Type")).toString();
        else if (xml.qualifiedName() == QLatin1String("desktop5:Verb"))
            manifest.verb_clsids << xml.attributes().value(QLatin1String("Clsid")).toString();
        else if (xml.qualifiedName() == QLatin1String("com:Class")) {
            manifest.class_id = xml.attributes().value(QLatin1String("Id")).toString();
            manifest.class_path = xml.attributes().value(QLatin1String("Path")).toString();
        }
    }
    return manifest;
}
}  // namespace

class ExplorerCommandTests : public QObject {
    Q_OBJECT

  private slots:
    void preferenceParsesTheThreeLines() {
        const auto lf = parse_preference(utf8("images_text\nPin to screen\njietuba_lite.exe\n"));
        QVERIFY(lf);
        QCOMPARE(lf->mode, Mode::ImagesAndText);
        QCOMPARE(lf->title, std::wstring(L"Pin to screen"));
        QCOMPARE(lf->exe_name, std::wstring(L"jietuba_lite.exe"));

        // CRLF, no final line end, and a byte order mark are all fine.
        const auto crlf = parse_preference(utf8("\xEF\xBB\xBF" "images\r\nPin\r\njietuba_pp.exe"));
        QVERIFY(crlf);
        QCOMPARE(crlf->mode, Mode::Images);
        QCOMPARE(crlf->title, std::wstring(L"Pin"));
        QCOMPARE(crlf->exe_name, std::wstring(L"jietuba_pp.exe"));

        const auto off = parse_preference(utf8("off\nPin\njietuba_lite.exe\n"));
        QVERIFY(off);
        QCOMPARE(off->mode, Mode::Off);

        // Extra lines are ignored.
        QVERIFY(parse_preference(utf8("images\nPin\njietuba_lite.exe\nsomething else\n")));
    }

    void preferenceKeepsNonAsciiTitles() {
        // 用截图吧钉到屏幕 in UTF-8
        const auto preference = parse_preference(
            utf8("images\n\xE7\x94\xA8\xE6\x88\xAA\xE5\x9B\xBE\xE5\x90\xA7\xE9\x92\x89\xE5\x88\xB0\xE5\xB1\x8F\xE5\xB9\x95\njietuba_lite.exe\n"));
        QVERIFY(preference);
        QCOMPARE(preference->title, std::wstring(L"\u7528\u622a\u56fe\u5427\u9489\u5230\u5c4f\u5e55"));
    }

    void preferenceRejectsWhatIsNotOurs_data() {
        QTest::addColumn<QByteArray>("text");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("one line") << QByteArray("images\n");
        QTest::newRow("two lines") << QByteArray("images\nPin\n");
        QTest::newRow("unknown mode") << QByteArray("all\nPin\njietuba_lite.exe\n");
        QTest::newRow("mode is case sensitive") << QByteArray("Images\nPin\njietuba_lite.exe\n");
        QTest::newRow("empty title") << QByteArray("images\n\njietuba_lite.exe\n");
        QTest::newRow("empty exe") << QByteArray("images\nPin\n\n");
        QTest::newRow("exe with a folder") << QByteArray("images\nPin\nsub\\jietuba_lite.exe\n");
        QTest::newRow("exe with a slash") << QByteArray("images\nPin\n../jietuba_lite.exe\n");
        QTest::newRow("exe with a drive") << QByteArray("images\nPin\nC:jietuba_lite.exe\n");
        QTest::newRow("not an exe") << QByteArray("images\nPin\njietuba_lite.dll\n");
        QTest::newRow("only the suffix") << QByteArray("images\nPin\n.exe\n");
        QTest::newRow("invalid utf-8 title") << QByteArray("images\n\xC3\x28\njietuba_lite.exe\n");
    }
    void preferenceRejectsWhatIsNotOurs() {
        QFETCH(QByteArray, text);
        QVERIFY(!parse_preference(std::string_view(text.constData(), static_cast<std::size_t>(text.size()))));
    }

    void preferenceFileIsReadFromDisk() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::wstring path = (dir.path() + "/explorer_menu.txt").toStdWString();
        QVERIFY(!read_preference_file(path));  // missing

        QFile file(dir.path() + "/explorer_menu.txt");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("images\nPin\njietuba_lite.exe\n");
        file.close();
        const auto preference = read_preference_file(path);
        QVERIFY(preference);
        QCOMPARE(preference->mode, Mode::Images);

        // A file far larger than three lines is not ours.
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("images\nPin\njietuba_lite.exe\n");
        file.write(QByteArray(100 * 1024, 'x'));
        file.close();
        QVERIFY(!read_preference_file(path));
    }

    void preferencePathFollowsTheContract() {
        QCOMPARE(preference_path(L"C:\\Users\\a\\AppData\\Local", L"Pub.App_abc123"),
                 std::wstring(L"C:\\Users\\a\\AppData\\Local\\Packages\\Pub.App_abc123\\LocalState\\explorer_menu.txt"));
        QCOMPARE(preference_path(L"C:\\L\\", L"F"), std::wstring(L"C:\\L\\Packages\\F\\LocalState\\explorer_menu.txt"));
    }

    void pinPathsAreAbsoluteAndPlain_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("valid");
        QTest::newRow("drive") << "C:\\a\\b.png" << true;
        QTest::newRow("lower drive and slash") << "d:/a/b.png" << true;
        QTest::newRow("root file") << "C:\\b.png" << true;
        QTest::newRow("unc") << "\\\\server\\share\\b.png" << true;
        QTest::newRow("relative") << "a\\b.png" << false;
        QTest::newRow("drive relative") << "C:b.png" << false;
        QTest::newRow("drive only") << "C:\\" << false;
        QTest::newRow("empty") << "" << false;
        QTest::newRow("unc without share") << "\\\\server\\" << false;
        QTest::newRow("unc without server") << "\\\\\\share\\b.png" << false;
        QTest::newRow("unc server only") << "\\\\server" << false;
        QTest::newRow("quote") << "C:\\a\"b.png" << false;
        QTest::newRow("control character") << QString("C:\\a\tb.png") << false;
        QTest::newRow("newline") << QString("C:\\a\nb.png") << false;
        QTest::newRow("delete character") << QString("C:\\a") + QChar(0x7F) + "b.png" << false;
        QTest::newRow("spaces and unicode are fine") << QString("C:\\\u6587 \u4ef6\\a b.png") << true;
    }
    void pinPathsAreAbsoluteAndPlain() {
        QFETCH(QString, path);
        QFETCH(bool, valid);
        QCOMPARE(is_pin_path(path.toStdWString()), valid);
    }

    void extensionsFollowTheModeAndIgnoreCase_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("matches");
        const int off = static_cast<int>(Mode::Off), images = static_cast<int>(Mode::Images),
                  both = static_cast<int>(Mode::ImagesAndText);
        QTest::newRow("png images") << "C:\\a.png" << images << true;
        QTest::newRow("PNG upper") << "C:\\a.PNG" << images << true;
        QTest::newRow("mixed jpeg") << "C:\\a.JpEg" << both << true;
        QTest::newRow("svg") << "C:\\a.svg" << images << true;
        QTest::newRow("png off") << "C:\\a.png" << off << false;
        QTest::newRow("txt images only") << "C:\\a.txt" << images << false;
        QTest::newRow("txt both") << "C:\\a.txt" << both << true;
        QTest::newRow("HTML both") << "C:\\a.HTML" << both << true;
        QTest::newRow("txt off") << "C:\\a.txt" << off << false;
        QTest::newRow("zip") << "C:\\a.zip" << both << false;
        QTest::newRow("exe") << "C:\\a.exe" << both << false;
        QTest::newRow("no extension") << "C:\\a" << both << false;
        QTest::newRow("dot only in the folder") << "C:\\a.png\\b" << both << false;
        QTest::newRow("dot only in the folder, slash") << "C:/a.png/b" << both << false;
        QTest::newRow("ends with a dot") << "C:\\a." << both << false;
        QTest::newRow("longer extension") << "C:\\a.pngx" << both << false;
        QTest::newRow("shorter extension") << "C:\\a.pn" << both << false;
        QTest::newRow("double extension") << "C:\\a.txt.png" << images << true;
        QTest::newRow("double extension, other order") << "C:\\a.png.txt" << images << false;
        QTest::newRow("hidden file name") << "C:\\.png" << images << true;
    }
    void extensionsFollowTheModeAndIgnoreCase() {
        QFETCH(QString, path);
        QFETCH(int, mode);
        QFETCH(bool, matches);
        QCOMPARE(extension_matches(path.toStdWString(), static_cast<Mode>(mode)), matches);
    }

    void everyContractExtensionIsHandledInItsMode() {
        for (const wchar_t *extension : jietuba::explorer_menu::IMAGE_EXTENSIONS) {
            const std::wstring path = std::wstring(L"C:\\x.") + extension;
            QVERIFY2(extension_matches(path, Mode::Images), qPrintable(QString::fromWCharArray(extension)));
            QVERIFY(extension_matches(path, Mode::ImagesAndText));
        }
        for (const wchar_t *extension : jietuba::explorer_menu::TEXT_EXTENSIONS) {
            const std::wstring path = std::wstring(L"C:\\x.") + extension;
            QVERIFY2(!extension_matches(path, Mode::Images), qPrintable(QString::fromWCharArray(extension)));
            QVERIFY(extension_matches(path, Mode::ImagesAndText));
        }
    }

    void wantedPathsNeedBothTheExtensionAndAPlainPath() {
        QVERIFY(wants_path(L"C:\\a.png", Mode::Images));
        QVERIFY(!wants_path(L"a.png", Mode::Images));
        QVERIFY(!wants_path(L"C:\\a\"b.png", Mode::Images));
        QVERIFY(!wants_path(L"C:\\a.txt", Mode::Images));
        QVERIFY(!wants_path(L"C:\\a.png", Mode::Off));
    }

    void argumentsAreQuotedForTheCrtRules_data() {
        QTest::addColumn<QString>("argument");
        QTest::addColumn<QString>("quoted");
        QTest::newRow("plain") << "C:\\a.png" << "\"C:\\a.png\"";
        QTest::newRow("space") << "C:\\My Pictures\\a b.png" << "\"C:\\My Pictures\\a b.png\"";
        QTest::newRow("quote") << "a\"b" << "\"a\\\"b\"";
        QTest::newRow("backslash before a quote") << "a\\\"b" << "\"a\\\\\\\"b\"";
        QTest::newRow("two backslashes before a quote") << "a\\\\\"b" << "\"a\\\\\\\\\\\"b\"";
        QTest::newRow("trailing backslash") << "C:\\dir\\" << "\"C:\\dir\\\\\"";
        QTest::newRow("trailing backslashes") << "C:\\dir\\\\" << "\"C:\\dir\\\\\\\\\"";
        QTest::newRow("empty") << "" << "\"\"";
        QTest::newRow("only a backslash") << "\\" << "\"\\\\\"";
    }
    void argumentsAreQuotedForTheCrtRules() {
        QFETCH(QString, argument);
        QFETCH(QString, quoted);
        std::wstring text;
        append_quoted(argument.toStdWString(), text);
        QCOMPARE(QString::fromStdWString(text), quoted);
        // And what the application's argument parser gives back is the original.
        const auto arguments = command_line_arguments(L"x.exe " + text);
        QCOMPARE(arguments.size(), std::size_t(2));
        QCOMPARE(QString::fromStdWString(arguments[1]), argument);
    }

    void theCommandLineCarriesOnePinPerFile() {
        const std::vector<std::wstring> files = {L"C:\\a b\\one.png", L"\\\\server\\share\\two.txt", L"D:\\three.GIF"};
        const CommandLine line = build_command_line(L"C:\\Program Files\\App\\jietuba_lite.exe", files);
        QCOMPARE(line.included, std::size_t(3));
        QCOMPARE(QString::fromStdWString(line.text),
                 QString("\"C:\\Program Files\\App\\jietuba_lite.exe\" --pin \"C:\\a b\\one.png\" "
                         "--pin \"\\\\server\\share\\two.txt\" --pin \"D:\\three.GIF\""));
        const auto arguments = command_line_arguments(line.text);
        QCOMPARE(arguments.size(), std::size_t(7));
        QCOMPARE(QString::fromStdWString(arguments[0]), QString("C:\\Program Files\\App\\jietuba_lite.exe"));
        QCOMPARE(QString::fromStdWString(arguments[1]), QString("--pin"));
        QCOMPARE(QString::fromStdWString(arguments[2]), QString("C:\\a b\\one.png"));
        QCOMPARE(QString::fromStdWString(arguments[4]), QString("\\\\server\\share\\two.txt"));
        QCOMPARE(QString::fromStdWString(arguments[6]), QString("D:\\three.GIF"));
    }

    void noFilesGiveNoCommandLine() {
        const CommandLine line = build_command_line(L"C:\\a.exe", {});
        QCOMPARE(line.included, std::size_t(0));
        QVERIFY(line.text.empty());
    }

    void aTooLongSelectionKeepsTheFirstFilesThatFit() {
        // "C:\a.exe" quoted is 12 characters; each file adds 1 + 5 + 1 + 102 = 109 (a 100 character path in quotes).
        // 12 + 109 * 300 = 32712 fits below the 32767 limit, the 301st (32821) does not.
        const std::wstring path = L"C:\\" + std::wstring(97, L'a');
        QCOMPARE(path.size(), std::size_t(100));
        const std::vector<std::wstring> files(400, path);
        const CommandLine line = build_command_line(L"C:\\a\\a.exe", files);
        QCOMPARE(line.included, std::size_t(300));
        QCOMPARE(line.text.size(), std::size_t(32712));
        QVERIFY(line.text.size() < MAX_COMMAND_LINE);
        QCOMPARE(command_line_arguments(line.text).size(), std::size_t(1 + 2 * 300));
    }

    void theLimitIsTheCreateProcessOneIncludingTheTerminator() {
        // "C:\a.exe" quoted is 10 characters, then " --pin " is 7 and the file's two quotes: 19 + length.
        const auto length_for = [](std::size_t path_length) {
            return build_command_line(L"C:\\a.exe", {std::wstring(path_length, L'a')});
        };
        const CommandLine fits = length_for(32747);  // 32766 characters, 32767 with the terminator
        QCOMPARE(fits.included, std::size_t(1));
        QCOMPARE(fits.text.size(), std::size_t(32766));
        const CommandLine over = length_for(32748);
        QCOMPARE(over.included, std::size_t(0));
        QVERIFY(over.text.empty());
    }

    void aSecondFileThatDoesNotFitIsLeftOutWholeNotCut() {
        const std::vector<std::wstring> files = {L"C:\\one.png", std::wstring(40000, L'x'), L"C:\\three.png"};
        const CommandLine line = build_command_line(L"C:\\a.exe", files);
        // The long one stops the line; later short ones are not squeezed in behind it.
        QCOMPARE(line.included, std::size_t(1));
        QCOMPARE(QString::fromStdWString(line.text), QString("\"C:\\a.exe\" --pin \"C:\\one.png\""));
    }

    void theManifestIsFilledFromTheHeaders_data() {
        QTest::addColumn<QString>("variant");
        QTest::addColumn<QString>("clsid");
        QTest::newRow("lite") << "lite" << QString::fromWCharArray(COMMAND_CLSID_LITE);
        QTest::newRow("full") << "full" << QString::fromWCharArray(COMMAND_CLSID_FULL);
    }
    void theManifestIsFilledFromTheHeaders() {
        QFETCH(QString, variant);
        QFETCH(QString, clsid);
        const QString cpp = QStringLiteral(JIETUBA_CPP_DIR);
        const QString template_path = cpp + "/packaging/msix/AppxManifest.xml.in";
        QFile template_file(template_path);
        QVERIFY(template_file.open(QIODevice::ReadOnly));
        const QString template_text = QString::fromUtf8(template_file.readAll());
        for (const char *placeholder : {"@EXPLORER_COMMAND_CLSID@", "@EXPLORER_COMMAND_DLL@", "@EXPLORER_ITEM_TYPES@"})
            QVERIFY2(template_text.contains(QLatin1String(placeholder)), placeholder);
        // The new elements are in namespaces older systems must be allowed to ignore.
        QVERIFY(template_text.contains("IgnorableNamespaces=\"uap desktop desktop4 desktop5 com rescap\""));
        QVERIFY(template_text.contains("windows.comServer"));
        QVERIFY(template_text.contains("windows.fileExplorerContextMenus"));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString output = dir.path() + "/manifest.xml";
        // Single quotes are the script's own; none of these paths contain one.
        const QString script = QString(". '%1/scripts/explorer_menu_manifest.ps1'; "
                                       "$text = Get-Content -LiteralPath '%2' -Raw -Encoding UTF8; "
                                       "$text = Expand-ExplorerMenuManifest $text '%1' '%3'; "
                                       "[System.IO.File]::WriteAllText('%4', $text, (New-Object System.Text.UTF8Encoding($false)))")
                                   .arg(QDir::toNativeSeparators(cpp), QDir::toNativeSeparators(template_path), variant,
                                        QDir::toNativeSeparators(output));
        QProcess powershell;
        powershell.start("powershell.exe", {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script});
        QVERIFY(powershell.waitForStarted(10000));
        QVERIFY2(powershell.waitForFinished(120000), "powershell did not finish");
        QVERIFY2(powershell.exitCode() == 0, powershell.readAllStandardError().constData());

        QFile result(output);
        QVERIFY(result.open(QIODevice::ReadOnly));
        const Manifest manifest = parse_manifest(QString::fromUtf8(result.readAll()));
        QVERIFY(!manifest.text.contains(QRegularExpression("@EXPLORER_")));

        // One item type per extension of the contract, each with a dot, once, all pointing at this variant's class.
        QSet<QString> expected;
        for (const QString &extension : contract_extensions())
            expected.insert("." + extension);
        QCOMPARE(QSet<QString>(manifest.item_types.cbegin(), manifest.item_types.cend()), expected);
        QCOMPARE(manifest.item_types.size(), expected.size());
        for (const QString &known : {QStringLiteral(".png"), QStringLiteral(".jpg"), QStringLiteral(".txt"), QStringLiteral(".html")})
            QVERIFY2(manifest.item_types.contains(known), qPrintable(known));
        QCOMPARE(manifest.verb_clsids.size(), manifest.item_types.size());
        const QString bare = clsid.mid(1, clsid.size() - 2);
        for (const QString &verb : manifest.verb_clsids)
            QCOMPARE(verb, bare);
        QCOMPARE(manifest.class_id, bare);
        QCOMPARE(manifest.class_path, QString("jietuba_explorer_command.dll"));
    }

    void theVariantsHaveDifferentClasses() {
        QVERIFY(std::wstring(COMMAND_CLSID_LITE) != std::wstring(COMMAND_CLSID_FULL));
        CLSID lite = {}, full = {};
        QCOMPARE(CLSIDFromString(COMMAND_CLSID_LITE, &lite), S_OK);
        QCOMPARE(CLSIDFromString(COMMAND_CLSID_FULL, &full), S_OK);
    }

    void theDllExposesTheCommandOutsideAPackage() {
        HMODULE module = LoadLibraryW(QDir::toNativeSeparators(QStringLiteral(JIETUBA_EXPLORER_COMMAND_DLL)).toStdWString().c_str());
        QVERIFY2(module, "the command DLL does not load");
        using GetClassObject = HRESULT(__stdcall *)(REFCLSID, REFIID, void **);
        using CanUnload = HRESULT(__stdcall *)();
        const auto get_class_object = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
        const auto can_unload = reinterpret_cast<CanUnload>(GetProcAddress(module, "DllCanUnloadNow"));
        QVERIFY(get_class_object);
        QVERIFY(can_unload);
        QCOMPARE(can_unload(), S_OK);

        CLSID lite = {};
        CLSID full = {};
        CLSIDFromString(COMMAND_CLSID_LITE, &lite);
        CLSIDFromString(COMMAND_CLSID_FULL, &full);

        // A class that is not ours.
        void *stranger = nullptr;
        QCOMPARE(get_class_object(IID_IShellItem, IID_IClassFactory, &stranger), CLASS_E_CLASSNOTAVAILABLE);
        QVERIFY(!stranger);

        for (const CLSID &clsid : {lite, full}) {
            IClassFactory *factory = nullptr;
            QCOMPARE(get_class_object(clsid, IID_IClassFactory, reinterpret_cast<void **>(&factory)), S_OK);
            QVERIFY(factory);
            QCOMPARE(can_unload(), S_FALSE);

            // Only IClassFactory is offered by the factory, and no aggregation.
            IExplorerCommand *wrong = nullptr;
            QCOMPARE(factory->CreateInstance(nullptr, IID_IShellItem, reinterpret_cast<void **>(&wrong)), E_NOINTERFACE);

            IExplorerCommand *command = nullptr;
            QCOMPARE(factory->CreateInstance(nullptr, IID_IExplorerCommand, reinterpret_cast<void **>(&command)), S_OK);
            QVERIFY(command);

            GUID name = {};
            QCOMPARE(command->GetCanonicalName(&name), S_OK);
            QVERIFY(IsEqualGUID(name, clsid));
            EXPCMDFLAGS flags = ECF_HASSUBCOMMANDS;
            QCOMPARE(command->GetFlags(&flags), S_OK);
            QCOMPARE(static_cast<int>(flags), static_cast<int>(ECF_DEFAULT));
            IEnumExplorerCommand *sub = reinterpret_cast<IEnumExplorerCommand *>(1);
            QCOMPARE(command->EnumSubCommands(&sub), E_NOTIMPL);
            QVERIFY(!sub);

            // This process has no package identity, so there is no preference: the command hides.
            EXPCMDSTATE state = ECS_ENABLED;
            QCOMPARE(command->GetState(nullptr, FALSE, &state), S_OK);
            QCOMPARE(static_cast<int>(state), static_cast<int>(ECS_HIDDEN));
            PWSTR title = nullptr;
            QVERIFY(FAILED(command->GetTitle(nullptr, &title)));
            QVERIFY(!title);
            // Invoking it does nothing and starts nothing.
            QCOMPARE(command->Invoke(nullptr, nullptr), S_OK);

            command->Release();
            factory->Release();
        }
        QCOMPARE(can_unload(), S_OK);
        FreeLibrary(module);
    }
};

QTEST_GUILESS_MAIN(ExplorerCommandTests)
#include "test_explorer_command.moc"
