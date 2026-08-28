#include "AppUpdater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>

namespace {
QString macTeamIdentifier(const QString &codesignOutput)
{
    const QRegularExpression teamId(QStringLiteral("TeamIdentifier=([A-Z0-9]+)"));
    const QRegularExpressionMatch match = teamId.match(codesignOutput);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return {};
}
}

AppUpdater::AppUpdater(const AppPaths &paths, QObject *parent)
    : QObject(parent)
    , paths_(paths)
    , network_(new QNetworkAccessManager(this))
    , installMode_(UpdateUtilities::currentInstallMode())
{
}

void AppUpdater::setLogCallbacks(LogFn info, LogFn warning, LogFn error)
{
    infoLog_ = std::move(info);
    warningLog_ = std::move(warning);
    errorLog_ = std::move(error);
}

void AppUpdater::restoreFromState(const UpdateCheckState &state)
{
    stagedVersion_.clear();
    stagedPayloadPath_.clear();
    stagedReleaseUrl_ = QUrl(state.latestReleaseUrl);
    helperStarted_ = false;
    installOnExit_ = false;

    if (state.pendingInstallVersion.trimmed().isEmpty() || state.pendingInstallPath.trimmed().isEmpty()) {
        return;
    }
    if (!QFileInfo::exists(state.pendingInstallPath)) {
        return;
    }
    if (!UpdateUtilities::isNewerVersion(state.pendingInstallVersion, QCoreApplication::applicationVersion())) {
        return;
    }

    stagedVersion_ = UpdateUtilities::normalizeReleaseVersion(state.pendingInstallVersion);
    stagedPayloadPath_ = state.pendingInstallPath;
    installOnExit_ = true;
    emit stateChanged();
}

void AppUpdater::handleAvailableRelease(const UpdateCheckState &state, const bool forcePrompt)
{
    if (!UpdateUtilities::isNewerVersion(state.latestVersion, QCoreApplication::applicationVersion())) {
        return;
    }

    if (installMode_ == UpdateInstallMode::DownloadLinkOnly) {
        emit downloadLinkNotice(
            UpdateUtilities::normalizeReleaseVersion(state.latestVersion),
            QUrl(state.latestReleaseUrl),
            isRunningReason(),
            forcePrompt);
        return;
    }

    if (hasStagedUpdate()
        && UpdateUtilities::compareVersionStrings(stagedVersion_, state.latestVersion) == 0
        && QFileInfo::exists(stagedPayloadPath_)) {
        emit stagedUpdateReady(stagedVersion_, forcePrompt);
        return;
    }

    if (state.latestAssetUrl.trimmed().isEmpty()) {
        emit downloadLinkNotice(
            UpdateUtilities::normalizeReleaseVersion(state.latestVersion),
            QUrl(state.latestReleaseUrl),
            QStringLiteral("No matching download was published for this platform."),
            forcePrompt);
        return;
    }

    beginDownload(state, forcePrompt);
}

void AppUpdater::applyStagedUpdate(const bool relaunchNow)
{
    if (!startPendingInstallHelper(relaunchNow)) {
        return;
    }
    if (relaunchNow) {
        QCoreApplication::quit();
    }
}

void AppUpdater::scheduleInstallOnExit()
{
    if (hasStagedUpdate()) {
        installOnExit_ = true;
    }
}

bool AppUpdater::startPendingInstallHelper(const bool relaunchNow)
{
    if (helperStarted_) {
        return true;
    }
    if (!hasStagedUpdate()) {
        return false;
    }

    QString errorMessage;
    if (!launchHelper(relaunchNow, &errorMessage)) {
        logError(QStringLiteral("Failed to start the update installer: %1").arg(errorMessage));
        emit updateFailed(errorMessage);
        return false;
    }

    helperStarted_ = true;
    installOnExit_ = false;
    logInfo(
        QStringLiteral("Update %1 will be installed after the app exits%2.")
            .arg(stagedVersion_, relaunchNow ? QStringLiteral(" and the app will reopen") : QString()));
    return true;
}

bool AppUpdater::isBusy() const
{
    return downloadReply_ != nullptr;
}

bool AppUpdater::isDownloading() const
{
    return downloadReply_ != nullptr;
}

bool AppUpdater::hasStagedUpdate() const
{
    return !stagedVersion_.isEmpty() && QFileInfo::exists(stagedPayloadPath_);
}

bool AppUpdater::canAutoInstall() const
{
    return installMode_ != UpdateInstallMode::DownloadLinkOnly;
}

QString AppUpdater::stagedVersion() const
{
    return stagedVersion_;
}

QString AppUpdater::stagedPayloadPath() const
{
    return stagedPayloadPath_;
}

QString AppUpdater::statusText(const QString &currentVersion, const UpdateCheckState &state) const
{
    if (isDownloading()) {
        if (downloadTotalBytes_ > 0) {
            return QStringLiteral("Downloading update %1 (%2 / %3 MB)...")
                .arg(UpdateUtilities::normalizeReleaseVersion(state.latestVersion))
                .arg(QString::number(static_cast<double>(downloadReceivedBytes_) / (1024.0 * 1024.0), 'f', 1))
                .arg(QString::number(static_cast<double>(downloadTotalBytes_) / (1024.0 * 1024.0), 'f', 1));
        }
        return QStringLiteral("Downloading update %1...")
            .arg(UpdateUtilities::normalizeReleaseVersion(state.latestVersion));
    }
    if (hasStagedUpdate()) {
        return QStringLiteral("Update %1 is ready and will install when you restart.").arg(stagedVersion_);
    }
    if (UpdateUtilities::isNewerVersion(state.latestVersion, currentVersion)) {
        if (installMode_ == UpdateInstallMode::DownloadLinkOnly) {
            return QStringLiteral("Update available: %1. This copy cannot be replaced automatically.")
                .arg(UpdateUtilities::normalizeReleaseVersion(state.latestVersion));
        }
        return QStringLiteral("Update available: %1").arg(UpdateUtilities::normalizeReleaseVersion(state.latestVersion));
    }
    return {};
}

qint64 AppUpdater::downloadReceivedBytes() const
{
    return downloadReceivedBytes_;
}

qint64 AppUpdater::downloadTotalBytes() const
{
    return downloadTotalBytes_;
}

UpdateInstallMode AppUpdater::installMode() const
{
    return installMode_;
}

QString AppUpdater::pendingRoot() const
{
    return paths_.rootPath() + QStringLiteral("/pending-update");
}

QString AppUpdater::downloadPath() const
{
    return pendingRoot() + QStringLiteral("/download.zip");
}

QString AppUpdater::extractPath() const
{
    return pendingRoot() + QStringLiteral("/extract");
}

QString AppUpdater::helperPath() const
{
#ifdef Q_OS_WIN
    return QDir::temp().filePath(QStringLiteral("matrix-media-share-client-update.cmd"));
#else
    return QDir::temp().filePath(QStringLiteral("matrix-media-share-client-update.sh"));
#endif
}

QString AppUpdater::isRunningReason() const
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (!UpdateUtilities::isRunningFromAppImage()) {
        return QStringLiteral("This Linux build is not a writable AppImage.");
    }
    return QStringLiteral("This AppImage is not writable.");
#elif defined(Q_OS_WIN)
    return QStringLiteral("The current Windows install folder is not writable.");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("This Mac app bundle cannot be replaced automatically.");
#else
    return QStringLiteral("This install cannot be replaced automatically.");
#endif
}

void AppUpdater::beginDownload(const UpdateCheckState &state, const bool forcePrompt)
{
    if (downloadReply_ != nullptr) {
        return;
    }

    QDir().mkpath(pendingRoot());
    QFile::remove(downloadPath());

    QNetworkRequest request(QUrl(state.latestAssetUrl));
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("MatrixMediaShareClientQt/%1").arg(QCoreApplication::applicationVersion()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(16);

    downloadReply_ = network_->get(request);
    downloadReceivedBytes_ = 0;
    downloadTotalBytes_ = state.latestAssetSize;
    emit stateChanged();
    logInfo(QStringLiteral("Downloading update %1 (%2).")
                .arg(UpdateUtilities::normalizeReleaseVersion(state.latestVersion), state.latestAssetName));

    QFile *output = new QFile(downloadPath(), downloadReply_);
    if (!output->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString message = QStringLiteral("Could not write update download: %1").arg(output->errorString());
        downloadReply_->abort();
        downloadReply_->deleteLater();
        downloadReply_ = nullptr;
        logError(message);
        emit updateFailed(message);
        emit stateChanged();
        return;
    }

    connect(downloadReply_, &QNetworkReply::downloadProgress, this, [this](const qint64 received, const qint64 total) {
        downloadReceivedBytes_ = received;
        if (total > 0) {
            downloadTotalBytes_ = total;
        }
        emit stateChanged();
    });
    connect(downloadReply_, &QNetworkReply::readyRead, this, [this, output]() {
        if (downloadReply_ == nullptr) {
            return;
        }
        output->write(downloadReply_->readAll());
    });
    connect(downloadReply_, &QNetworkReply::finished, this, [this, output, state, forcePrompt]() {
        output->write(downloadReply_->readAll());
        output->close();
        finishDownload(downloadReply_, state, forcePrompt);
    });
}

void AppUpdater::finishDownload(QNetworkReply *reply, const UpdateCheckState &state, const bool forcePrompt)
{
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply->error() == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300;
    reply->deleteLater();
    downloadReply_ = nullptr;

    if (!ok) {
        const QString message = reply->errorString().trimmed().isEmpty()
            ? QStringLiteral("Update download failed with HTTP %1.").arg(httpStatus)
            : reply->errorString().trimmed();
        QFile::remove(downloadPath());
        logWarning(QStringLiteral("Update download failed: %1").arg(message));
        emit updateFailed(message);
        emit stateChanged();
        return;
    }

    QDir extractDir(extractPath());
    extractDir.removeRecursively();
    extractDir.mkpath(QStringLiteral("."));

    QString errorMessage;
    if (!extractArchive(downloadPath(), extractPath(), &errorMessage)) {
        logError(QStringLiteral("Failed to unpack the update: %1").arg(errorMessage));
        emit updateFailed(errorMessage);
        emit stateChanged();
        return;
    }

    const StagedUpdatePayload payload = UpdateUtilities::locateStagedPayload(extractPath(), installMode_);
    if (payload.payloadPath.isEmpty()) {
        errorMessage = QStringLiteral("The downloaded archive did not contain a matching app payload.");
        logError(errorMessage);
        emit updateFailed(errorMessage);
        emit stateChanged();
        return;
    }

#ifdef Q_OS_MACOS
    if (installMode_ == UpdateInstallMode::ReplaceAppBundle && !verifyMacAppBundle(payload.payloadPath, &errorMessage)) {
        logError(errorMessage);
        emit updateFailed(errorMessage);
        emit stateChanged();
        return;
    }
#endif

    stagedVersion_ = UpdateUtilities::normalizeReleaseVersion(state.latestVersion);
    stagedPayloadPath_ = payload.payloadPath;
    stagedReleaseUrl_ = QUrl(state.latestReleaseUrl);
    installOnExit_ = true;
    QFile::remove(downloadPath());
    logInfo(QStringLiteral("Staged update %1 at %2.").arg(stagedVersion_, stagedPayloadPath_));
    emit stateChanged();
    emit stagedUpdateReady(stagedVersion_, forcePrompt);
}

bool AppUpdater::extractArchive(const QString &archivePath, const QString &destination, QString *errorMessage)
{
    QProcess process;
    QString program;
    QStringList arguments;
#ifdef Q_OS_MACOS
    program = QStringLiteral("ditto");
    arguments = {QStringLiteral("-x"), QStringLiteral("-k"), archivePath, destination};
#elif defined(Q_OS_WIN)
    program = QStringLiteral("tar");
    arguments = {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"), destination};
#else
    program = QStringLiteral("unzip");
    arguments = {QStringLiteral("-o"), QStringLiteral("-q"), archivePath, QStringLiteral("-d"), destination};
#endif
    process.start(program, arguments);
    if (!process.waitForFinished(120000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
#ifdef Q_OS_WIN
        QProcess powershell;
        powershell.start(
            QStringLiteral("powershell.exe"),
            {QStringLiteral("-NoProfile"),
             QStringLiteral("-Command"),
             QStringLiteral("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
                 .arg(archivePath, destination)});
        if (powershell.waitForFinished(120000) && powershell.exitCode() == 0) {
            return true;
        }
#endif
        if (errorMessage != nullptr) {
            const QString details = QString::fromUtf8(process.readAllStandardError()).trimmed();
            *errorMessage = details.isEmpty() ? QStringLiteral("Archive extraction failed.") : details;
        }
        return false;
    }
    return true;
}

bool AppUpdater::verifyMacAppBundle(const QString &appBundle, QString *errorMessage) const
{
#ifndef Q_OS_MACOS
    Q_UNUSED(appBundle);
    Q_UNUSED(errorMessage);
    return true;
#else
    QProcess verify;
    verify.start(QStringLiteral("codesign"), {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), appBundle});
    if (!verify.waitForFinished(30000) || verify.exitCode() != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("The downloaded Mac app is not a valid code-signed bundle.");
        }
        return false;
    }

    QProcess display;
    display.setProcessChannelMode(QProcess::MergedChannels);
    display.start(QStringLiteral("codesign"), {QStringLiteral("-dv"), QStringLiteral("--verbose=2"), appBundle});
    if (!display.waitForFinished(30000)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to read the downloaded Mac app signature.");
        }
        return false;
    }
    const QString output = QString::fromUtf8(display.readAll());
    const QString teamId = macTeamIdentifier(output);
    if (teamId != QLatin1String(UpdateUtilities::kExpectedMacTeamId)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("The downloaded Mac app is not signed by the expected Developer ID team.");
        }
        return false;
    }
    return true;
#endif
}

bool AppUpdater::launchHelper(const bool relaunchNow, QString *errorMessage)
{
    const QString script =
#ifdef Q_OS_WIN
        UpdateUtilities::windowsUpdateHelperScript();
#else
        UpdateUtilities::unixUpdateHelperScript();
#endif
    if (!UpdateUtilities::writeTextFile(helperPath(), script, errorMessage)) {
        return false;
    }

    UpdateHelperSpec spec;
    spec.pid = UpdateUtilities::currentProcessId();
    spec.sourcePath = stagedPayloadPath_;
    spec.destinationPath = UpdateUtilities::currentInstallDestination();
    spec.installMode = installMode_;
    spec.relaunch = relaunchNow;
    spec.relaunchPath = UpdateUtilities::currentRelaunchPath();

#ifdef Q_OS_WIN
    const QString program = QStringLiteral("cmd.exe");
    QStringList arguments = {QStringLiteral("/C"), helperPath()};
    arguments.append(UpdateUtilities::windowsHelperArguments(spec));
#else
    const QString program = QStringLiteral("/bin/bash");
    QStringList arguments = {helperPath()};
    arguments.append(UpdateUtilities::unixHelperArguments(spec));
#endif

    const bool started = QProcess::startDetached(program, arguments, QDir::tempPath());
    if (!started && errorMessage != nullptr) {
        *errorMessage = QStringLiteral("Could not start the detached update helper.");
    }
    return started;
}

void AppUpdater::logInfo(const QString &message)
{
    if (infoLog_) {
        infoLog_(message);
    }
}

void AppUpdater::logWarning(const QString &message)
{
    if (warningLog_) {
        warningLog_(message);
    }
}

void AppUpdater::logError(const QString &message)
{
    if (errorLog_) {
        errorLog_(message);
    }
}
