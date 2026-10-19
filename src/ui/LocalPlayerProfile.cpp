#include "veyra/ui/LocalPlayerProfile.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QUuid>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace veyra::ui {
namespace {

constexpr qint64 kMaximumSettingBytes = 16 * 1024 * 1024;
constexpr int kMaximumSettingFiles = 128;
const QString kPrefs = QStringLiteral("qml-preferences.v1.json");
const QString kMarker = QStringLiteral(".veyra-local-player-session");
const QString kRemoved = QStringLiteral(".local-player-removed.v1.json");

bool isReparsePoint(const QString& path) {
    const QFileInfo info(path);
    if (info.isSymbolicLink()) return true;
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD attributes = GetFileAttributesW(native.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

// Check every existing ancestor, rather than resolving a junction to its target
// and then mistakenly treating the resolved path as an owned profile directory.
bool safeAncestors(const QString& path, QString& error) {
    QString current = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (;;) {
        if (isReparsePoint(current)) {
            error = QStringLiteral("Profile path contains a symbolic link or reparse point");
            return false;
        }
        const QFileInfo info(current);
        const QString parent = QDir::cleanPath(info.absolutePath());
        if (parent == current) break;
        current = parent;
    }
    return true;
}

bool ensureDirectory(const QString& path, QString& error) {
    if (!safeAncestors(path, error)) return false;
    const QFileInfo info(path);
    if ((info.exists() && !info.isDir()) || !QDir().mkpath(path)) {
        error = QStringLiteral("Cannot create local player profile directory");
        return false;
    }
    return safeAncestors(path, error) && QFileInfo(path).isDir();
}

bool allowed(const QString& name) {
    if (name.isEmpty() || name.contains('/') || name.contains('\\')
        || name.contains(QStringLiteral("browser"), Qt::CaseInsensitive)) return false;
    if (name == kPrefs || name == QStringLiteral("node-anchors.json")
        || name == QStringLiteral("source-profiles.v1")
        || name == QStringLiteral("online-subtitle-token.v1.bin")) return true;
    static const QStringList bases{
        QStringLiteral("presets.v1"), QStringLiteral("chain-session.v1"),
        QStringLiteral("user-presets.v1"), QStringLiteral("nr-presets.v1")};
    for (const auto& base : bases) {
        if (name == base) return true;
        if (!name.startsWith(base + QLatin1Char('.'))) continue;
        const auto suffix = name.mid(base.size() + 1);
        if (suffix.isEmpty() || suffix.contains(QStringLiteral(".."))) return false;
        for (const auto character : suffix)
            if (!(character.isLetterOrNumber() || character == QLatin1Char('.')
                  || character == QLatin1Char('-') || character == QLatin1Char('_'))) return false;
        return true;
    }
    return false;
}

QJsonValue withoutHistory(const QJsonValue& value) {
    if (value.isArray()) {
        QJsonArray result;
        for (const auto& child : value.toArray()) result.append(withoutHistory(child));
        return result;
    }
    if (!value.isObject()) return value;
    QJsonObject result;
    const auto object = value.toObject();
    static const QStringList history{
        QStringLiteral("lastsource"), QStringLiteral("lastsourceextent"),
        QStringLiteral("recentfiles"), QStringLiteral("recentsources"),
        QStringLiteral("recentvideos"), QStringLiteral("recentlyplayed"),
        QStringLiteral("lastplayed"), QStringLiteral("lastplayedfile")};
    for (auto it = object.begin(); it != object.end(); ++it) {
        const auto key = it.key().toLower();
        if (history.contains(key) || key.startsWith(QStringLiteral("browser"))) continue;
        result.insert(it.key(), withoutHistory(it.value()));
    }
    return result;
}

bool normalize(const QString& name, QByteArray& bytes, QString& error) {
    if (name != kPrefs) return true;
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("Cannot sanitize malformed local player preferences");
        return false;
    }
    auto object = withoutHistory(document.object()).toObject();
    object.insert(QStringLiteral("autoResume"), false);
    bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return true;
}

bool readSetting(const QString& filePath, const QString& name, QByteArray& bytes, QString& error) {
    if (!safeAncestors(filePath, error)) return false;
    const QFileInfo info(filePath);
    if (!info.isFile() || info.size() > kMaximumSettingBytes) {
        error = QStringLiteral("Local player setting is not a bounded regular file");
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot read local player setting");
        return false;
    }
    bytes = file.read(kMaximumSettingBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > kMaximumSettingBytes) {
        error = QStringLiteral("Cannot read bounded local player setting");
        return false;
    }
    return normalize(name, bytes, error);
}

bool atomicWrite(const QString& path, const QByteArray& bytes, QString& error) {
    if (!safeAncestors(path, error)) return false;
    if (QFileInfo(path).exists() && !QFileInfo(path).isFile()) {
        error = QStringLiteral("Local player setting target is not a regular file");
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = QStringLiteral("Cannot atomically save local player setting");
        return false;
    }
    return true;
}

bool gatherSettings(const QString& directory, QMap<QString, QByteArray>& files, QString& error,
                    const QMap<QString, QByteArray>* preferred = nullptr,
                    const QSet<QString>* excluded = nullptr) {
    if (!safeAncestors(directory, error)) return false;
    if (!QFileInfo(directory).exists()) return true;
    if (!QFileInfo(directory).isDir()) {
        error = QStringLiteral("Local player settings source is not a directory");
        return false;
    }
    const auto entries = QDir(directory).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System
                                                     | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& info : entries) {
        const auto name = info.fileName();
        if (!allowed(name) || (preferred && preferred->contains(name))
            || (excluded && excluded->contains(name))) continue;
        QByteArray bytes;
        if (!readSetting(info.absoluteFilePath(), name, bytes, error)) return false;
        files.insert(name, bytes);
        if (files.size() > kMaximumSettingFiles) {
            error = QStringLiteral("Too many local player setting files");
            return false;
        }
    }
    return true;
}

} // namespace

struct LocalPlayerProfile::Impl {
    QString sharedRoot;
    QString sessionsRoot;
    QString defaultsRoot;
    QString sessionRoot;
    QString sessionId;
    QString error;
    QMap<QString, QByteArray> initial;
    bool ready = false;
    bool created = false;

    QString lockPath() const { return QDir(sharedRoot).filePath(QStringLiteral(".local-player-defaults.lock")); }
    QByteArray markerBytes() const { return (QStringLiteral("VEYRA_LOCAL_PLAYER_SESSION 1\n") + sessionId + QLatin1Char('\n')
                                            + sharedRoot + QLatin1Char('\n')).toUtf8(); }
    bool acquire(QLockFile& lock) {
        if (!safeAncestors(lockPath(), error)) return false;
        if (!lock.tryLock(3000)) {
            error = QStringLiteral("Local player settings are locked by another process");
            return false;
        }
        return true;
    }

    bool readRemoved(QSet<QString>& removed) {
        const auto path = QDir(defaultsRoot).filePath(kRemoved);
        if (!safeAncestors(path, error)) return false;
        if (!QFileInfo::exists(path)) return true;
        QByteArray bytes;
        if (!readSetting(path, kRemoved, bytes, error)) return false;
        QJsonParseError parseError{};
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isArray()
            || document.array().size() > kMaximumSettingFiles) {
            error = QStringLiteral("Malformed removed local player settings list");
            return false;
        }
        for (const auto& item : document.array()) {
            if (!item.isString() || !allowed(item.toString())) {
                error = QStringLiteral("Unapproved removed local player setting");
                return false;
            }
            removed.insert(item.toString());
        }
        return true;
    }
    bool writeRemoved(const QSet<QString>& removed) {
        QStringList names(removed.begin(), removed.end());
        names.sort();
        QJsonArray array;
        for (const auto& name : names) array.append(name);
        return atomicWrite(QDir(defaultsRoot).filePath(kRemoved), QJsonDocument(array).toJson(), error);
    }

    bool owned() {
        if (!created || sessionId.isEmpty() || sessionRoot.isEmpty()
            || QDir::cleanPath(QDir(sessionsRoot).filePath(sessionId)) != sessionRoot
            || QFileInfo(sessionRoot).absolutePath() != sessionsRoot) {
            error = QStringLiteral("Refusing cleanup outside the owned local player session");
            return false;
        }
        if (!safeAncestors(sessionRoot, error)) return false;
        QByteArray marker;
        if (!readSetting(QDir(sessionRoot).filePath(kMarker), kMarker, marker, error)) return false;
        if (marker != markerBytes()) {
            error = QStringLiteral("Local player session ownership marker changed");
            return false;
        }
        return true;
    }

    bool cleanup() {
        if (!created) return true;
        if (!owned()) return false;
        // Inventory first. Never follow a reparse point or delete another UUID,
        // even if a caller or another application modified this private tree.
        QStringList directories{sessionRoot};
        QStringList files;
        for (qsizetype index = 0; index < directories.size(); ++index) {
            const auto children = QDir(directories[index]).entryInfoList(
                QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
            for (const auto& child : children) {
                const QString path = child.absoluteFilePath();
                if (!path.startsWith(sessionRoot + QLatin1Char('/')) || isReparsePoint(path)) {
                    error = QStringLiteral("Refusing cleanup of a changed local player session tree");
                    return false;
                }
                if (child.isDir()) directories.append(path);
                else if (child.isFile()) files.append(path);
                else { error = QStringLiteral("Refusing cleanup of an unsupported profile entry"); return false; }
            }
        }
        for (const auto& file : files) {
            if (!safeAncestors(file, error) || !QFile::remove(file)) {
                if (error.isEmpty()) error = QStringLiteral("Cannot remove owned local player session file");
                return false;
            }
        }
        for (auto it = directories.crbegin(); it != directories.crend(); ++it) {
            if (!safeAncestors(*it, error) || !QDir().rmdir(*it)) {
                if (error.isEmpty()) error = QStringLiteral("Cannot remove owned local player session directory");
                return false;
            }
        }
        created = false;
        return true;
    }
};

LocalPlayerProfile::LocalPlayerProfile(const QString& sharedDataDirectory) : p_(std::make_unique<Impl>()) {
    if (sharedDataDirectory.isEmpty() || !QDir::isAbsolutePath(sharedDataDirectory)) {
        p_->error = QStringLiteral("Shared profile directory must be absolute");
        return;
    }
    p_->sharedRoot = QDir::cleanPath(sharedDataDirectory);
    p_->sessionsRoot = QDir(p_->sharedRoot).filePath(QStringLiteral("local-player-sessions"));
    p_->defaultsRoot = QDir(p_->sharedRoot).filePath(QStringLiteral("local-player-defaults"));
    if (!ensureDirectory(p_->sharedRoot, p_->error) || !ensureDirectory(p_->sessionsRoot, p_->error)
        || !safeAncestors(p_->defaultsRoot, p_->error)) return;
    QLockFile lock(p_->lockPath());
    if (!p_->acquire(lock)) return;
    QSet<QString> removed;
    if (!p_->readRemoved(removed)) return;
    QMap<QString, QByteArray> saved;
    if (!gatherSettings(p_->defaultsRoot, saved, p_->error, nullptr, &removed)) return;
    QMap<QString, QByteArray> inherited;
    if (!gatherSettings(p_->sharedRoot, inherited, p_->error, &saved, &removed)) return;
    for (auto it = saved.cbegin(); it != saved.cend(); ++it) inherited.insert(it.key(), it.value());
    // Always start with explicit no-history/no-resume preferences, even when
    // neither the resident nor defaults has created a preference file yet.
    if (!inherited.contains(kPrefs)) {
        QByteArray bytes("{}");
        if (!normalize(kPrefs, bytes, p_->error)) return;
        inherited.insert(kPrefs, bytes);
    }
    p_->sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p_->sessionRoot = QDir(p_->sessionsRoot).filePath(p_->sessionId);
    if (QFileInfo(p_->sessionRoot).exists() || !QDir(p_->sessionsRoot).mkdir(p_->sessionId)
        || !safeAncestors(p_->sessionRoot, p_->error)) {
        if (p_->error.isEmpty()) p_->error = QStringLiteral("Cannot create unique local player session");
        return;
    }
    p_->created = true;
    if (!atomicWrite(QDir(p_->sessionRoot).filePath(kMarker), p_->markerBytes(), p_->error)) return;
    for (auto it = inherited.cbegin(); it != inherited.cend(); ++it)
        if (!atomicWrite(QDir(p_->sessionRoot).filePath(it.key()), it.value(), p_->error)) return;
    p_->initial = inherited;
    p_->ready = true;
}

LocalPlayerProfile::~LocalPlayerProfile() {
    // Retain recovery data if a save fails. Never recursively sweep stale UUIDs.
    if (p_->ready && !commit()) {
        qWarning() << "Local player settings retained in private session:" << p_->error;
        return;
    }
    if (p_->created && !p_->cleanup()) qWarning() << "Local player session cleanup refused:" << p_->error;
}

bool LocalPlayerProfile::ready() const { return p_->ready; }
QString LocalPlayerProfile::path() const { return p_->ready ? p_->sessionRoot : QString(); }
QString LocalPlayerProfile::error() const { return p_->error; }

bool LocalPlayerProfile::commit() {
    if (!p_->ready) return false;
    p_->error.clear();
    if (!p_->owned()) return false;
    QMap<QString, QByteArray> current;
    if (!gatherSettings(p_->sessionRoot, current, p_->error)) return false;
    QMap<QString, QByteArray> changed;
    for (auto it = current.cbegin(); it != current.cend(); ++it)
        if (!p_->initial.contains(it.key()) || p_->initial.value(it.key()) != it.value())
            changed.insert(it.key(), it.value());
    QSet<QString> deleted;
    for (auto it = p_->initial.cbegin(); it != p_->initial.cend(); ++it)
        if (!current.contains(it.key())) deleted.insert(it.key());
    if (changed.isEmpty() && deleted.isEmpty()) return true;
    QLockFile lock(p_->lockPath());
    if (!p_->acquire(lock) || !ensureDirectory(p_->defaultsRoot, p_->error)) return false;
    QSet<QString> removed;
    if (!p_->readRemoved(removed)) return false;
    // Publish deletion tombstones before removing a credential/settings file, so
    // a crash cannot revive its older resident fallback on the next launch.
    if (!deleted.isEmpty()) {
        removed.unite(deleted);
        if (!p_->writeRemoved(removed)) return false;
        for (const auto& name : deleted) {
            const auto target = QDir(p_->defaultsRoot).filePath(name);
            if (!safeAncestors(target, p_->error)) return false;
            if (QFileInfo::exists(target) && (!QFileInfo(target).isFile() || !QFile::remove(target))) {
                p_->error = QStringLiteral("Cannot remove deleted local player setting");
                return false;
            }
        }
    }
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) {
        if (!atomicWrite(QDir(p_->defaultsRoot).filePath(it.key()), it.value(), p_->error)) return false;
        removed.remove(it.key());
    }
    if (!p_->writeRemoved(removed)) return false;
    p_->initial = current;
    return true;
}

} // namespace veyra::ui
