#pragma once

#include "AppPaths.h"
#include "Domain.h"
#include "UpdateUtilities.h"

#ifdef Q_OS_MACOS
#include "SparkleBridge.h"
#endif

#include <QObject>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

class AppUpdater : public QObject
{
    Q_OBJECT

public:
    explicit AppUpdater(const AppPaths &paths, QObject *parent = nullptr);

    using LogFn = std::function<void(const QString &message)>;
    void setLogCallbacks(LogFn info, LogFn warning, LogFn error);

    void restoreFromState(const UpdateCheckState &state);
    void handleAvailableRelease(const UpdateCheckState &state, bool forcePrompt);
    void applyStagedUpdate(bool relaunchNow);
    void scheduleInstallOnExit();
    bool startPendingInstallHelper(bool relaunchNow);

    bool isBusy() const;
    bool isDownloading() const;
    bool hasStagedUpdate() const;
    bool canAutoInstall() const;
    QString stagedVersion() const;
    QString stagedPayloadPath() const;
    QString statusText(const QString &currentVersion, const UpdateCheckState &state) const;
    qint64 downloadReceivedBytes() const;
    qint64 downloadTotalBytes() const;
    UpdateInstallMode installMode() const;

signals:
    void stateChanged();
    void stagedUpdateReady(const QString &version, bool forcePrompt);
    void downloadLinkNotice(const QString &version, const QUrl &pageUrl, const QString &reason, bool forcePrompt);
    void updateFailed(const QString &message);

private:
    QString pendingRoot() const;
    QString downloadPath() const;
    QString extractPath() const;
    QString helperPath() const;
    void beginDownload(const UpdateCheckState &state, bool forcePrompt);
    void finishDownload(QNetworkReply *reply, const UpdateCheckState &state, bool forcePrompt);
    bool extractArchive(const QString &archivePath, const QString &destination, QString *errorMessage);
    bool verifyMacAppBundle(const QString &appBundle, QString *errorMessage) const;
    bool launchHelper(bool relaunchNow, QString *errorMessage);
    QString isRunningReason() const;
    void ensureSparkleStarted();
    void logInfo(const QString &message);
    void logWarning(const QString &message);
    void logError(const QString &message);

    AppPaths paths_;
    QNetworkAccessManager *network_ = nullptr;
    QNetworkReply *downloadReply_ = nullptr;
    LogFn infoLog_;
    LogFn warningLog_;
    LogFn errorLog_;
    UpdateInstallMode installMode_ = UpdateInstallMode::DownloadLinkOnly;
    QString stagedVersion_;
    QString stagedPayloadPath_;
    QUrl stagedReleaseUrl_;
    qint64 downloadReceivedBytes_ = 0;
    qint64 downloadTotalBytes_ = -1;
    bool helperStarted_ = false;
    bool installOnExit_ = false;
#ifdef Q_OS_MACOS
    SparkleBridge *sparkle_ = nullptr;
    bool sparkleStarted_ = false;
#endif
};
