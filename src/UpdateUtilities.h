#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QVector>

enum class UpdatePackageKind {
    MacosArm64Zip,
    MacosX86_64Zip,
    WindowsX64Zip,
    WindowsArm64Zip,
    LinuxX86_64AppImageZip,
    LinuxAarch64AppImageZip,
};

enum class UpdateInstallMode {
    ReplaceAppBundle,
    OverlayDirectory,
    ReplaceAppImage,
    DownloadLinkOnly,
};

struct GithubReleaseInfo {
    QString version;
    QString htmlUrl;
    QString name;
    QDateTime publishedAt;
    QString assetName;
    QString assetUrl;
    qint64 assetSize = -1;
};

struct StagedUpdatePayload {
    QString rootPath;
    QString payloadPath;
    UpdateInstallMode installMode = UpdateInstallMode::DownloadLinkOnly;
};

struct UpdateHelperSpec {
    qint64 pid = 0;
    QString sourcePath;
    QString destinationPath;
    UpdateInstallMode installMode = UpdateInstallMode::DownloadLinkOnly;
    bool relaunch = false;
    QString relaunchPath;
};

namespace UpdateUtilities {
constexpr qint64 kUpdateCheckIntervalSeconds = 2LL * 24LL * 60LL * 60LL;
constexpr auto kGithubReleasesApiUrl =
    "https://api.github.com/repos/bstone108/matrix-media-share-client/releases/latest";
constexpr auto kGithubReleasesPageUrl = "https://github.com/bstone108/matrix-media-share-client/releases";
constexpr auto kExpectedMacTeamId = "K6N4J68LTY";
constexpr auto kAppExecutableName =
#ifdef Q_OS_WIN
    "MatrixMediaShareClientQt.exe";
#else
    "MatrixMediaShareClientQt";
#endif
constexpr auto kMacAppBundleName = "MatrixMediaShareClientQt.app";

QString normalizeReleaseVersion(QString version);
QVector<int> parseVersionParts(const QString &version);
int compareVersionStrings(const QString &lhs, const QString &rhs);
bool isUpdateCheckDue(const QDateTime &lastCheckedAt, const QDateTime &now = QDateTime::currentDateTimeUtc());
bool isNewerVersion(const QString &candidate, const QString &current);
bool shouldNagForVersion(
    const QString &latestVersion,
    const QString &currentVersion,
    const QString &lastNotifiedVersion);

UpdatePackageKind currentPackageKind();
QString assetSuffix(UpdatePackageKind kind);
bool assetNameMatches(const QString &assetName, UpdatePackageKind kind);
bool selectReleaseAsset(const QJsonArray &assets, UpdatePackageKind kind, GithubReleaseInfo *info);
bool parseGithubRelease(const QJsonObject &object, UpdatePackageKind kind, GithubReleaseInfo *info);

QString linuxAppImagePath();
bool isRunningFromAppImage();
bool canReplacePath(const QString &path);
UpdateInstallMode currentInstallMode();
QString currentInstallDestination();
QString currentRelaunchPath();

QString findMacAppBundle(const QString &rootPath);
QString findWindowsExeDirectory(const QString &rootPath);
QString findAppImage(const QString &rootPath);
StagedUpdatePayload locateStagedPayload(const QString &extractDir, UpdateInstallMode mode);

QString unixUpdateHelperScript();
QString windowsUpdateHelperScript();
bool writeTextFile(const QString &path, const QString &contents, QString *errorMessage = nullptr);
QStringList unixHelperArguments(const UpdateHelperSpec &spec);
QStringList windowsHelperArguments(const UpdateHelperSpec &spec);
qint64 currentProcessId();
}
