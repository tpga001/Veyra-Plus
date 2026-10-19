#include "veyra/ui/LocalPlayerProfile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMap>
#include <cstddef>
#include <QJsonObject>
#include <QLockFile>
#include <QStringList>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

using veyra::ui::LocalPlayerProfile;
namespace {
int checks = 0;
int failures = 0;
void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
QString file(const QString& directory, const char* name) { return QDir(directory).filePath(QString::fromUtf8(name)); }
bool write(const QString& path, const QByteArray& bytes) {
    QFile stream(path);
    return stream.open(QIODevice::WriteOnly | QIODevice::Truncate) && stream.write(bytes) == bytes.size();
}
QByteArray read(const QString& path) {
    QFile stream(path);
    if (!stream.open(QIODevice::ReadOnly)) return {};
    return stream.readAll();
}
QJsonObject object(const QString& path) { return QJsonDocument::fromJson(read(path)).object(); }
bool writeObject(const QString& path, const QJsonObject& value) { return write(path, QJsonDocument(value).toJson()); }

// A real directory junction is available to an ordinary Windows user and tests
// the Windows-specific reparse branch without relying on Developer Mode.
bool makeDirectoryLink(const QString& link, const QString& target) {
#ifdef Q_OS_WIN
    if (!QDir().mkpath(link)) return false;
    const auto nativeLink = QDir::toNativeSeparators(link).toStdWString();
    const auto print = QDir::toNativeSeparators(QFileInfo(target).absoluteFilePath()).toStdWString();
    const auto substitute = std::wstring(L"\\??\\") + print;
    const auto content = substitute + L'\0' + print + L'\0';
    struct JunctionData {
        DWORD tag;
        WORD bytes;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
        WCHAR text[1];
    };
    QByteArray buffer(int(offsetof(JunctionData, text) + content.size() * sizeof(wchar_t)), '\0');
    auto* data = reinterpret_cast<JunctionData*>(buffer.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->bytes = WORD(buffer.size() - 8);
    data->substituteLength = WORD(substitute.size() * sizeof(wchar_t));
    data->printOffset = WORD((substitute.size() + 1) * sizeof(wchar_t));
    data->printLength = WORD(print.size() * sizeof(wchar_t));
    std::memcpy(data->text, content.data(), content.size() * sizeof(wchar_t));
    HANDLE handle = CreateFileW(nativeLink.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data, DWORD(buffer.size()),
                                    nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    return ok;
#else
    return QFile::link(target, link);
#endif
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        std::cerr << "usage: veyra_local_player_profile_tests <new absolute scratch directory>\n";
        return 2;
    }
    const QString scratch = QDir::cleanPath(app.arguments()[1]);
    if (!QDir::isAbsolutePath(scratch) || QFileInfo::exists(scratch) || !QDir().mkpath(scratch)) {
        std::cerr << "requires a new absolute private scratch directory\n"; return 2;
    }
    const QString shared = file(scratch, "resident");
    expect(QDir().mkpath(shared), "create synthetic resident profile");
    const QJsonObject originalPrefs{
        {"autoResume", true}, {"rememberPosition", true}, {"subtitleSize", 24},
        {"lastSource", QJsonObject{{"label", "private.mp4"}}},
        {"lastSourceExtent", QJsonObject{{"width", 1920}}},
        {"positions", QJsonObject{{"private.mp4", 10}}},
        {"playbackPositions", QJsonObject{{"private.mp4", 20}}},
        {"recentFiles", QJsonArray{"private.mp4"}},
        {"browserRtxSourceSized", true},
        {"presentation", QJsonObject{{"customFps", 60}, {"positions", "nested history"}}}
    };
    const QMap<QString, QByteArray> rootFiles{
        {"presets.v1", "resident preset"}, {"presets.v1.flow", "resident flow"},
        {"chain-session.v1.field-render", "local chain"}, {"user-presets.v1", "resident users"},
        {"nr-presets.v1", "resident nr"}, {"source-profiles.v1", "VEYRA_SOURCE_PROFILES 1\n"},
        {"node-anchors.json", "{\"x\":10}"}, {"online-subtitle-token.v1.bin", "synthetic-encrypted-value"},
        {"qml-preferences.v1.json", QJsonDocument(originalPrefs).toJson()},
        {"ui-session.v1", "private.mp4 recent-file-list"},
        {"browser-chain-session.v1", "browser chain"}, {"chain-session.v1.browser", "browser suffix"},
        {"desktop-preferences.v1.json", "resident tray settings"}, {"unknown.v1", "unapproved"}
    };
    for (auto it = rootFiles.cbegin(); it != rootFiles.cend(); ++it)
        expect(write(QDir(shared).filePath(it.key()), it.value()), "write synthetic root file");
    const QString stale = file(shared, "local-player-sessions/old-session-not-owned");
    expect(QDir().mkpath(stale) && write(file(stale, "keep.txt"), "do not remove"), "create unrelated old session");
    QString firstPath, secondPath;
    const QString defaults = file(shared, "local-player-defaults");
    {
        LocalPlayerProfile first(shared), second(shared);
        expect(first.ready() && second.ready(), "two independent local profiles are ready");
        firstPath = first.path(); secondPath = second.path();
        expect(!firstPath.isEmpty() && firstPath != secondPath && firstPath != shared, "local players use distinct UUID directories");
        expect(read(file(firstPath, "presets.v1")) == "resident preset", "fallback inherits resident local preset");
        expect(read(file(firstPath, "chain-session.v1.field-render")) == "local chain", "local preset suffix inherited");
        expect(read(file(firstPath, "online-subtitle-token.v1.bin")) == "synthetic-encrypted-value", "subtitle encrypted setting inherited without interpretation");
        expect(!QFileInfo::exists(file(firstPath, "ui-session.v1")), "recent-file session never copied");
        expect(!QFileInfo::exists(file(firstPath, "browser-chain-session.v1"))
               && !QFileInfo::exists(file(firstPath, "chain-session.v1.browser")), "browser chains excluded");
        expect(!QFileInfo::exists(file(firstPath, "desktop-preferences.v1.json"))
               && !QFileInfo::exists(file(firstPath, "unknown.v1")), "resident desktop and unknown settings excluded");
        auto prefs = object(file(firstPath, "qml-preferences.v1.json"));
        expect(!prefs.contains("lastSource") && !prefs.contains("lastSourceExtent") && !prefs.contains("recentFiles"), "visible recent/source history stripped");
        expect(!prefs.value("autoResume").toBool(true) && prefs.value("rememberPosition").toBool(false), "automatic replay disabled and remember-position preference retained");
        expect(prefs.value("positions").toObject().contains("private.mp4")
               && prefs.value("playbackPositions").toObject().contains("private.mp4"), "playback position memory retained without visible recents");
        expect(prefs.value("subtitleSize").toInt() == 24 && !prefs.contains("browserRtxSourceSized"), "player preferences retained without browser options");
        expect(prefs.value("presentation").toObject().contains("positions"), "non-recent nested preferences preserved");
        expect(first.commit() && second.commit() && !QFileInfo::exists(defaults), "unchanged sessions do not create or overwrite defaults");
        expect(write(file(firstPath, "presets.v1"), "first edit") && first.commit(), "first player saves changed preset");
        expect(read(file(defaults, "presets.v1")) == "first edit", "changed file atomically reaches local defaults");
        expect(second.commit() && read(file(defaults, "presets.v1")) == "first edit", "unmodified second player cannot overwrite first player's edit");
        expect(write(file(secondPath, "user-presets.v1"), "second users") && second.commit(), "second player saves a different file");
        expect(read(file(defaults, "presets.v1")) == "first edit" && read(file(defaults, "user-presets.v1")) == "second users", "changes in distinct files merge");
        expect(write(file(firstPath, "presets.v1"), "first again") && first.commit(), "first edits same setting again");
        expect(write(file(secondPath, "presets.v1"), "last edit") && second.commit(), "last changed-file commit wins");
        expect(first.commit() && read(file(defaults, "presets.v1")) == "last edit", "repeated unchanged commit cannot undo later writer");
        prefs["lastSource"] = QJsonObject{{"label", "another-private.mp4"}};
        expect(writeObject(file(firstPath, "qml-preferences.v1.json"), prefs) && first.commit(), "last-source-only preferences normalize without save");
        expect(!QFileInfo::exists(file(defaults, "qml-preferences.v1.json")), "history-only changes never persist to defaults");
        prefs["subtitleSize"] = 32;
        expect(writeObject(file(firstPath, "qml-preferences.v1.json"), prefs) && first.commit(), "real preference edit saved");
        const auto saved = object(file(defaults, "qml-preferences.v1.json"));
        expect(saved.value("subtitleSize").toInt() == 32 && saved.contains("positions") && !saved.contains("lastSource"), "saved preferences retain position memory without last-source display data");
        expect(write(file(firstPath, "presets.v1.custom"), "new allowed") && write(file(firstPath, "ui-session.v1"), "session private")
               && first.commit(), "new allowlisted settings can be saved");
        expect(read(file(defaults, "presets.v1.custom")) == "new allowed" && !QFileInfo::exists(file(defaults, "ui-session.v1")), "unapproved new files never saved");
        {
            LocalPlayerProfile later(shared);
            expect(later.ready() && read(file(later.path(), "presets.v1")) == "last edit", "new process prioritizes saved local defaults");
            expect(object(file(later.path(), "qml-preferences.v1.json")).value("subtitleSize").toInt() == 32, "new player inherits edited local UI preferences");
            expect(read(file(later.path(), "nr-presets.v1")) == "resident nr", "missing saved file independently falls back to resident");
        }
        expect(write(file(secondPath, "presets.v1"), "locked edit"), "prepare change for lock test");
        {
            QLockFile lock(file(shared, ".local-player-defaults.lock"));
            expect(lock.tryLock(100), "hold synthetic defaults lock");
            expect(!second.commit() && !second.error().isEmpty(), "settings lock prevents concurrent interleaved commits");
            expect(read(file(defaults, "presets.v1")) == "last edit", "failed locked commit leaves defaults unchanged");
        }
        expect(second.commit() && second.error().isEmpty(), "retry after lock release commits and clears error");
        expect(write(file(firstPath, "qml-preferences.v1.json"), "not-json") && !first.commit(), "malformed preferences fail closed");
        expect(object(file(defaults, "qml-preferences.v1.json")).value("subtitleSize").toInt() == 32, "malformed changes cannot corrupt defaults");
        expect(writeObject(file(firstPath, "qml-preferences.v1.json"), prefs) && first.commit(), "repaired session commits normally");
        expect(QFile::remove(file(secondPath, "online-subtitle-token.v1.bin")) && second.commit(), "cleared subtitle credential commits deletion");
        {
            LocalPlayerProfile cleared(shared);
            expect(cleared.ready() && !QFileInfo::exists(file(cleared.path(), "online-subtitle-token.v1.bin")),
                   "deleted credential cannot revive from resident fallback");
            expect(write(file(cleared.path(), "online-subtitle-token.v1.bin"), "replacement-synthetic-credential")
                   && cleared.commit(), "new credential removes deletion tombstone");
        }
        {
            LocalPlayerProfile restored(shared);
            expect(restored.ready() && read(file(restored.path(), "online-subtitle-token.v1.bin")) == "replacement-synthetic-credential",
                   "new player inherits replacement credential");
        }
    }
    expect(!QFileInfo::exists(firstPath) && !QFileInfo::exists(secondPath), "destructors clean only their own session directories");
    expect(read(file(stale, "keep.txt")) == "do not remove", "unrelated old session untouched");
    for (auto it = rootFiles.cbegin(); it != rootFiles.cend(); ++it)
        expect(read(QDir(shared).filePath(it.key())) == it.value(), "resident root files remain byte-for-byte unchanged");
    // Valid saved defaults must be sufficient even if the older resident copy
    // of that same setting is damaged or currently being replaced.
    expect(write(file(shared, "qml-preferences.v1.json"), "resident damaged later"), "damage only synthetic fallback copy");
    {
        LocalPlayerProfile preferred(shared);
        expect(preferred.ready() && object(file(preferred.path(), "qml-preferences.v1.json")).value("subtitleSize").toInt() == 32,
               "valid saved preferences take precedence over malformed resident fallback");
    }
    expect(write(file(shared, "qml-preferences.v1.json"), rootFiles.value("qml-preferences.v1.json")), "restore synthetic root for isolation checks");
    const QString outside = file(scratch, "outside-sentinel");
    expect(QDir().mkpath(outside) && write(file(outside, "sentinel.txt"), "outside untouched"), "create synthetic outside sentinel");
    const QString linkedRoot = file(scratch, "linked-resident");
    expect(makeDirectoryLink(linkedRoot, shared), "create actual platform directory link fixture");
    {
        LocalPlayerProfile linked(linkedRoot);
        expect(!linked.ready() && linked.path().isEmpty() && !linked.error().isEmpty(), "profile root through reparse point rejected");
    }
    const QString unsafeDefaultsRoot = file(scratch, "unsafe-defaults");
    expect(QDir().mkpath(unsafeDefaultsRoot) && makeDirectoryLink(file(unsafeDefaultsRoot, "local-player-defaults"), outside), "create reparse defaults fixture");
    {
        LocalPlayerProfile linked(unsafeDefaultsRoot);
        expect(!linked.ready(), "defaults junction cannot redirect writes outside shared root");
    }
    QString refusedCleanup;
    {
        LocalPlayerProfile modified(shared);
        expect(modified.ready(), "create private cleanup guard fixture");
        refusedCleanup = modified.path();
        expect(makeDirectoryLink(file(refusedCleanup, "unexpected-linked-cache"), outside), "insert directory junction inside own session");
    }
    expect(QFileInfo::exists(refusedCleanup), "cleanup refuses session tree containing a reparse point");
    expect(read(file(outside, "sentinel.txt")) == "outside untouched", "all junction targets remain unchanged");
    QString retained;
    {
        LocalPlayerProfile broken(shared);
        expect(broken.ready(), "create failed-save recovery fixture");
        retained = broken.path();
        expect(write(file(retained, "qml-preferences.v1.json"), "invalid json"), "create synthetic failed-save condition");
    }
    expect(QFileInfo::exists(retained), "destructor preserves unsaved session after commit failure");
    {
        LocalPlayerProfile relative(QStringLiteral("relative-profile"));
        expect(!relative.ready() && relative.path().isEmpty(), "relative root rejected");
    }
    std::cout << (failures == 0 ? "PASS" : "FAIL") << " LocalPlayerProfile: " << checks
              << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
