#include "VlcPlayerWidget.h"

#include <QFile>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QStringList>
#include <QShowEvent>
#include <QTimer>

struct libvlc_instance_t;
struct libvlc_media_t;
struct libvlc_media_player_t;

namespace {

using LibvlcNewFn = libvlc_instance_t *(*)(int, const char *const *);
using LibvlcReleaseFn = void (*)(libvlc_instance_t *);
using LibvlcMediaNewPathFn = libvlc_media_t *(*)(libvlc_instance_t *, const char *);
using LibvlcMediaReleaseFn = void (*)(libvlc_media_t *);
using LibvlcMediaPlayerNewFromMediaFn = libvlc_media_player_t *(*)(libvlc_media_t *);
using LibvlcMediaPlayerReleaseFn = void (*)(libvlc_media_player_t *);
using LibvlcMediaPlayerPlayFn = int (*)(libvlc_media_player_t *);
using LibvlcMediaPlayerStopFn = void (*)(libvlc_media_player_t *);
using LibvlcMediaPlayerSetHwndFn = void (*)(libvlc_media_player_t *, void *);
using LibvlcMediaPlayerSetNsobjectFn = void (*)(libvlc_media_player_t *, void *);
using LibvlcMediaPlayerSetXwindowFn = void (*)(libvlc_media_player_t *, unsigned);
using LibvlcErrmsgFn = const char *(*)();

struct VlcLibrary
{
    QLibrary library;
    bool loaded = false;
    QString pluginPath;
    QString error;
    LibvlcNewFn libvlc_new = nullptr;
    LibvlcReleaseFn libvlc_release = nullptr;
    LibvlcMediaNewPathFn libvlc_media_new_path = nullptr;
    LibvlcMediaReleaseFn libvlc_media_release = nullptr;
    LibvlcMediaPlayerNewFromMediaFn libvlc_media_player_new_from_media = nullptr;
    LibvlcMediaPlayerReleaseFn libvlc_media_player_release = nullptr;
    LibvlcMediaPlayerPlayFn libvlc_media_player_play = nullptr;
    LibvlcMediaPlayerStopFn libvlc_media_player_stop = nullptr;
    LibvlcMediaPlayerSetHwndFn libvlc_media_player_set_hwnd = nullptr;
    LibvlcMediaPlayerSetNsobjectFn libvlc_media_player_set_nsobject = nullptr;
    LibvlcMediaPlayerSetXwindowFn libvlc_media_player_set_xwindow = nullptr;
    LibvlcErrmsgFn libvlc_errmsg = nullptr;
};

QString cleanedPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QStringList candidateVlcLibraryPaths()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
#if defined(Q_OS_MACOS)
    candidates
        << cleanedPath(QDir(appDir).filePath(QStringLiteral("../Resources/vlc/lib/libvlc.dylib")))
        << QStringLiteral("/Applications/VLC.app/Contents/MacOS/lib/libvlc.dylib")
        << QDir::home().filePath(QStringLiteral("Applications/VLC.app/Contents/MacOS/lib/libvlc.dylib"));
#elif defined(Q_OS_WIN)
    candidates
        << cleanedPath(QDir(appDir).filePath(QStringLiteral("vlc/lib/libvlc.dll")))
        << cleanedPath(QDir(appDir).filePath(QStringLiteral("vlc/libvlc.dll")))
        << cleanedPath(QDir(appDir).filePath(QStringLiteral("libvlc.dll")))
        << QStringLiteral("C:/Program Files/VideoLAN/VLC/libvlc.dll")
        << QStringLiteral("C:/Program Files (x86)/VideoLAN/VLC/libvlc.dll");
#else
    candidates
        << cleanedPath(QDir(appDir).filePath(QStringLiteral("vlc/lib/libvlc.so.5")))
        << cleanedPath(QDir(appDir).filePath(QStringLiteral("vlc/lib/libvlc.so")))
        << QStringLiteral("libvlc.so.5")
        << QStringLiteral("libvlc.so");
#endif
    candidates.removeDuplicates();
    return candidates;
}

QString pluginPathForLibrary(const QString &libraryPath)
{
#if defined(Q_OS_MACOS)
    const QFileInfo info(libraryPath);
    if (!info.exists()) {
        return {};
    }
    return cleanedPath(QDir(info.absolutePath()).filePath(QStringLiteral("../plugins")));
#elif defined(Q_OS_WIN)
    const QFileInfo info(libraryPath);
    if (!info.exists()) {
        return {};
    }
    const QDir libraryDir(info.absolutePath());
    const QString siblingPlugins = cleanedPath(libraryDir.filePath(QStringLiteral("../plugins")));
    if (QFileInfo::exists(siblingPlugins)) {
        return siblingPlugins;
    }
    return cleanedPath(libraryDir.filePath(QStringLiteral("plugins")));
#else
    const QFileInfo info(libraryPath);
    if (!info.exists()) {
        return {};
    }
    return cleanedPath(QDir(info.absolutePath()).filePath(QStringLiteral("../plugins")));
#endif
}

VlcLibrary &vlcLibrary()
{
    static VlcLibrary library;
    if (library.loaded || library.library.isLoaded()) {
        return library;
    }

    const QStringList candidates = candidateVlcLibraryPaths();
    for (const QString &candidate : candidates) {
        if (candidate.contains(QLatin1Char('/')) || candidate.contains(QLatin1Char('\\'))) {
            if (!QFileInfo::exists(candidate)) {
                continue;
            }
            library.library.setFileName(candidate);
        } else {
            library.library.setFileName(candidate);
        }

        if (!library.pluginPath.isEmpty()) {
            qunsetenv("VLC_PLUGIN_PATH");
        }
        const QString pluginPath = pluginPathForLibrary(candidate);
        if (!pluginPath.isEmpty() && QFileInfo::exists(pluginPath)) {
            qputenv("VLC_PLUGIN_PATH", pluginPath.toUtf8());
            library.pluginPath = pluginPath;
        }

        if (!library.library.load()) {
            library.error = library.library.errorString();
            continue;
        }

        library.libvlc_new = reinterpret_cast<LibvlcNewFn>(library.library.resolve("libvlc_new"));
        library.libvlc_release = reinterpret_cast<LibvlcReleaseFn>(library.library.resolve("libvlc_release"));
        library.libvlc_media_new_path = reinterpret_cast<LibvlcMediaNewPathFn>(library.library.resolve("libvlc_media_new_path"));
        library.libvlc_media_release = reinterpret_cast<LibvlcMediaReleaseFn>(library.library.resolve("libvlc_media_release"));
        library.libvlc_media_player_new_from_media =
            reinterpret_cast<LibvlcMediaPlayerNewFromMediaFn>(library.library.resolve("libvlc_media_player_new_from_media"));
        library.libvlc_media_player_release =
            reinterpret_cast<LibvlcMediaPlayerReleaseFn>(library.library.resolve("libvlc_media_player_release"));
        library.libvlc_media_player_play =
            reinterpret_cast<LibvlcMediaPlayerPlayFn>(library.library.resolve("libvlc_media_player_play"));
        library.libvlc_media_player_stop =
            reinterpret_cast<LibvlcMediaPlayerStopFn>(library.library.resolve("libvlc_media_player_stop"));
        library.libvlc_errmsg = reinterpret_cast<LibvlcErrmsgFn>(library.library.resolve("libvlc_errmsg"));
#if defined(Q_OS_WIN)
        library.libvlc_media_player_set_hwnd =
            reinterpret_cast<LibvlcMediaPlayerSetHwndFn>(library.library.resolve("libvlc_media_player_set_hwnd"));
#elif defined(Q_OS_MACOS)
        library.libvlc_media_player_set_nsobject =
            reinterpret_cast<LibvlcMediaPlayerSetNsobjectFn>(library.library.resolve("libvlc_media_player_set_nsobject"));
#else
        library.libvlc_media_player_set_xwindow =
            reinterpret_cast<LibvlcMediaPlayerSetXwindowFn>(library.library.resolve("libvlc_media_player_set_xwindow"));
#endif

        const bool hasCoreFns = library.libvlc_new != nullptr
            && library.libvlc_release != nullptr
            && library.libvlc_media_new_path != nullptr
            && library.libvlc_media_release != nullptr
            && library.libvlc_media_player_new_from_media != nullptr
            && library.libvlc_media_player_release != nullptr
            && library.libvlc_media_player_play != nullptr
            && library.libvlc_media_player_stop != nullptr;
#if defined(Q_OS_WIN)
        const bool hasDrawableFn = library.libvlc_media_player_set_hwnd != nullptr;
#elif defined(Q_OS_MACOS)
        const bool hasDrawableFn = library.libvlc_media_player_set_nsobject != nullptr;
#else
        const bool hasDrawableFn = library.libvlc_media_player_set_xwindow != nullptr;
#endif
        if (hasCoreFns && hasDrawableFn) {
            library.loaded = true;
            return library;
        }

        library.error = QStringLiteral("libVLC was found but is missing one or more required symbols.");
        library.library.unload();
    }

    if (library.error.isEmpty()) {
        library.error = QStringLiteral("libVLC runtime was not found.");
    }
    return library;
}

const char *const *vlcArgsData(QList<QByteArray> &storage)
{
    static thread_local QVector<const char *> pointers;
    pointers.clear();
    pointers.reserve(storage.size());
    for (const QByteArray &item : storage) {
        pointers.append(item.constData());
    }
    return pointers.constData();
}

} // namespace

struct VlcPlayerWidget::Private
{
    bool initialized = false;
    libvlc_instance_t *instance = nullptr;
    libvlc_media_player_t *player = nullptr;
    QString lastError;
    QString pendingFilePath;
    bool pendingStartScheduled = false;
};

VlcPlayerWidget::VlcPlayerWidget(QWidget *parent)
    : QWidget(parent)
    , d_(new Private)
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumSize(320, 240);
}

VlcPlayerWidget::~VlcPlayerWidget()
{
    stopPlayback();
    VlcLibrary &lib = vlcLibrary();
    if (d_ != nullptr && d_->instance != nullptr && lib.loaded) {
        lib.libvlc_release(d_->instance);
        d_->instance = nullptr;
    }
    delete d_;
}

bool VlcPlayerWidget::isAvailable() const
{
    return const_cast<VlcPlayerWidget *>(this)->ensureInitialized();
}

QString VlcPlayerWidget::lastError() const
{
    return d_ != nullptr ? d_->lastError : QString();
}

bool VlcPlayerWidget::playFile(const QString &filePath)
{
    if (!ensureInitialized()) {
        return false;
    }

    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        d_->lastError = QStringLiteral("The media file does not exist.");
        return false;
    }

    stopPlayback();
    d_->pendingFilePath = info.absoluteFilePath();
    d_->lastError.clear();

    if (!isVisible()) {
        schedulePendingPlayback();
        return true;
    }

    const bool started = startPlaybackNow(d_->pendingFilePath);
    if (started) {
        d_->pendingFilePath.clear();
        emit playbackStarted();
        return true;
    }

    emit playbackFailed(d_->lastError);
    return false;
}

void VlcPlayerWidget::stopPlayback()
{
    if (d_ == nullptr || d_->player == nullptr) {
        if (d_ != nullptr) {
            d_->pendingFilePath.clear();
            d_->pendingStartScheduled = false;
        }
        return;
    }

    VlcLibrary &lib = vlcLibrary();
    if (lib.loaded) {
        lib.libvlc_media_player_stop(d_->player);
        lib.libvlc_media_player_release(d_->player);
    }
    d_->player = nullptr;
    d_->pendingFilePath.clear();
    d_->pendingStartScheduled = false;
}

QSize VlcPlayerWidget::sizeHint() const
{
    return QWidget::sizeHint().expandedTo(QSize(480, 320));
}

void VlcPlayerWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    schedulePendingPlayback();
}

bool VlcPlayerWidget::startPlaybackNow(const QString &filePath)
{
    if (!ensureInitialized() || filePath.trimmed().isEmpty()) {
        return false;
    }

    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        d_->lastError = QStringLiteral("The media file does not exist.");
        return false;
    }

    VlcLibrary &lib = vlcLibrary();
    libvlc_media_t *media = lib.libvlc_media_new_path(d_->instance, QFile::encodeName(info.absoluteFilePath()).constData());
    if (media == nullptr) {
        d_->lastError = lib.libvlc_errmsg != nullptr && lib.libvlc_errmsg() != nullptr
            ? QString::fromUtf8(lib.libvlc_errmsg())
            : QStringLiteral("libVLC could not open this media file.");
        return false;
    }

    d_->player = lib.libvlc_media_player_new_from_media(media);
    lib.libvlc_media_release(media);
    if (d_->player == nullptr) {
        d_->lastError = lib.libvlc_errmsg != nullptr && lib.libvlc_errmsg() != nullptr
            ? QString::fromUtf8(lib.libvlc_errmsg())
            : QStringLiteral("libVLC could not create a media player.");
        return false;
    }

    const WId widgetId = winId();
#if defined(Q_OS_WIN)
    lib.libvlc_media_player_set_hwnd(d_->player, reinterpret_cast<void *>(widgetId));
#elif defined(Q_OS_MACOS)
    lib.libvlc_media_player_set_nsobject(d_->player, reinterpret_cast<void *>(widgetId));
#else
    lib.libvlc_media_player_set_xwindow(d_->player, static_cast<unsigned>(widgetId));
#endif

    if (lib.libvlc_media_player_play(d_->player) != 0) {
        d_->lastError = lib.libvlc_errmsg != nullptr && lib.libvlc_errmsg() != nullptr
            ? QString::fromUtf8(lib.libvlc_errmsg())
            : QStringLiteral("libVLC failed to start playback.");
        lib.libvlc_media_player_release(d_->player);
        d_->player = nullptr;
        return false;
    }

    d_->lastError.clear();
    return true;
}

void VlcPlayerWidget::schedulePendingPlayback()
{
    if (d_ == nullptr || d_->pendingStartScheduled || d_->pendingFilePath.isEmpty()) {
        return;
    }
    d_->pendingStartScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        if (d_ == nullptr) {
            return;
        }
        d_->pendingStartScheduled = false;
        if (d_->pendingFilePath.isEmpty() || !isVisible()) {
            return;
        }
        const QString pendingPath = d_->pendingFilePath;
        d_->pendingFilePath.clear();
        if (startPlaybackNow(pendingPath)) {
            emit playbackStarted();
            return;
        }
        emit playbackFailed(d_->lastError);
    });
}

bool VlcPlayerWidget::ensureInitialized()
{
    if (d_ == nullptr) {
        return false;
    }
    if (d_->initialized) {
        return d_->instance != nullptr;
    }
    d_->initialized = true;

    VlcLibrary &lib = vlcLibrary();
    if (!lib.loaded) {
        d_->lastError = lib.error;
        return false;
    }

    QList<QByteArray> args {
        QByteArray("--quiet"),
        QByteArray("--no-video-title-show"),
        QByteArray("--no-snapshot-preview"),
        QByteArray("--no-sub-autodetect-file"),
    };
#if defined(Q_OS_MACOS)
    args.append(QByteArray("--vout=macosx"));
#endif
    d_->instance = lib.libvlc_new(static_cast<int>(args.size()), vlcArgsData(args));
    if (d_->instance == nullptr) {
        d_->lastError = lib.libvlc_errmsg != nullptr && lib.libvlc_errmsg() != nullptr
            ? QString::fromUtf8(lib.libvlc_errmsg())
            : QStringLiteral("libVLC failed to initialize.");
        return false;
    }
    d_->lastError.clear();
    return true;
}
