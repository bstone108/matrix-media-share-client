#include "UpdateUtilities.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariant>

#include <functional>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <process.h>
#endif

namespace {
QString firstMatchingPath(const QString &rootPath, const std::function<bool(const QFileInfo &)> &matcher)
{
    if (rootPath.trimmed().isEmpty() || !QFileInfo::exists(rootPath)) {
        return {};
    }

    QFileInfo rootInfo(rootPath);
    if (matcher(rootInfo)) {
        return QDir::cleanPath(rootInfo.absoluteFilePath());
    }

    QDirIterator iterator(
        rootPath,
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    QString bestPath;
    int bestDepth = 1000;
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (!matcher(info)) {
            continue;
        }
        const QString relative = QDir(rootPath).relativeFilePath(info.absoluteFilePath());
        const int depth = relative.count(QLatin1Char('/'));
        if (depth < bestDepth) {
            bestDepth = depth;
            bestPath = QDir::cleanPath(info.absoluteFilePath());
        }
    }
    return bestPath;
}

QString jsonString(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString().trimmed();
}

qint64 jsonInt64(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble());
    }
    bool ok = false;
    const qint64 parsed = value.toVariant().toLongLong(&ok);
    return ok ? parsed : -1;
}
}

namespace UpdateUtilities {

QString normalizeReleaseVersion(QString version)
{
    version = version.trimmed();
    if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        version.remove(0, 1);
    }
    return version;
}

QVector<int> parseVersionParts(const QString &version)
{
    QVector<int> parts;
    for (const QString &segment : normalizeReleaseVersion(version).split(QLatin1Char('.'), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int value = segment.toInt(&ok);
        if (!ok) {
            return {};
        }
        parts.append(value);
    }
    return parts;
}

int compareVersionStrings(const QString &lhs, const QString &rhs)
{
    const QVector<int> lhsParts = parseVersionParts(lhs);
    const QVector<int> rhsParts = parseVersionParts(rhs);
    if (lhsParts.isEmpty() || rhsParts.isEmpty()) {
        return QString::compare(normalizeReleaseVersion(lhs), normalizeReleaseVersion(rhs), Qt::CaseInsensitive);
    }

    const int segmentCount = qMax(lhsParts.size(), rhsParts.size());
    for (int index = 0; index < segmentCount; ++index) {
        const int lhsValue = index < lhsParts.size() ? lhsParts.at(index) : 0;
        const int rhsValue = index < rhsParts.size() ? rhsParts.at(index) : 0;
        if (lhsValue < rhsValue) {
            return -1;
        }
        if (lhsValue > rhsValue) {
            return 1;
        }
    }
    return 0;
}

bool isUpdateCheckDue(const QDateTime &lastCheckedAt, const QDateTime &now)
{
    if (!lastCheckedAt.isValid()) {
        return true;
    }
    return lastCheckedAt.secsTo(now) >= kUpdateCheckIntervalSeconds;
}

bool isNewerVersion(const QString &candidate, const QString &current)
{
    return compareVersionStrings(candidate, current) > 0;
}

bool shouldNagForVersion(
    const QString &latestVersion,
    const QString &currentVersion,
    const QString &lastNotifiedVersion)
{
    if (!isNewerVersion(latestVersion, currentVersion)) {
        return false;
    }
    const QString latest = normalizeReleaseVersion(latestVersion);
    const QString notified = normalizeReleaseVersion(lastNotifiedVersion);
    return notified.isEmpty() || notified != latest;
}

UpdatePackageKind currentPackageKind()
{
#if defined(Q_OS_MACOS)
#if defined(Q_PROCESSOR_ARM_64)
    return UpdatePackageKind::MacosArm64Zip;
#else
    return UpdatePackageKind::MacosX86_64Zip;
#endif
#elif defined(Q_OS_WIN)
#if defined(Q_PROCESSOR_ARM_64)
    return UpdatePackageKind::WindowsArm64Zip;
#else
    return UpdatePackageKind::WindowsX64Zip;
#endif
#else
#if defined(Q_PROCESSOR_ARM_64)
    return UpdatePackageKind::LinuxAarch64AppImageZip;
#else
    return UpdatePackageKind::LinuxX86_64AppImageZip;
#endif
#endif
}

QString assetSuffix(const UpdatePackageKind kind)
{
    switch (kind) {
    case UpdatePackageKind::MacosArm64Zip:
        return QStringLiteral("macos-arm64.zip");
    case UpdatePackageKind::MacosX86_64Zip:
        return QStringLiteral("macos-x86_64.zip");
    case UpdatePackageKind::WindowsX64Zip:
        return QStringLiteral("windows-x64.zip");
    case UpdatePackageKind::WindowsArm64Zip:
        return QStringLiteral("windows-arm64.zip");
    case UpdatePackageKind::LinuxX86_64AppImageZip:
        return QStringLiteral("linux-x86_64-appimage.zip");
    case UpdatePackageKind::LinuxAarch64AppImageZip:
        return QStringLiteral("linux-aarch64-appimage.zip");
    }
    return {};
}

bool assetNameMatches(const QString &assetName, const UpdatePackageKind kind)
{
    const QString name = assetName.trimmed();
    const QString suffix = assetSuffix(kind);
    return name.startsWith(QStringLiteral("MatrixMediaShareClientQt-"), Qt::CaseInsensitive)
        && name.endsWith(suffix, Qt::CaseInsensitive);
}

bool selectReleaseAsset(const QJsonArray &assets, const UpdatePackageKind kind, GithubReleaseInfo *info)
{
    if (info == nullptr) {
        return false;
    }

    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = jsonString(asset, QStringLiteral("name"));
        if (!assetNameMatches(name, kind)) {
            continue;
        }
        info->assetName = name;
        info->assetUrl = jsonString(asset, QStringLiteral("browser_download_url"));
        info->assetSize = jsonInt64(asset, QStringLiteral("size"));
        return !info->assetUrl.isEmpty();
    }
    return false;
}

bool parseGithubRelease(const QJsonObject &object, const UpdatePackageKind kind, GithubReleaseInfo *info)
{
    if (info == nullptr) {
        return false;
    }

    *info = GithubReleaseInfo {};
    info->version = normalizeReleaseVersion(jsonString(object, QStringLiteral("tag_name")));
    info->htmlUrl = jsonString(object, QStringLiteral("html_url"));
    info->name = jsonString(object, QStringLiteral("name"));
    info->publishedAt = QDateTime::fromString(jsonString(object, QStringLiteral("published_at")), Qt::ISODate);
    if (info->htmlUrl.isEmpty()) {
        info->htmlUrl = QString::fromLatin1(kGithubReleasesPageUrl);
    }
    selectReleaseAsset(object.value(QStringLiteral("assets")).toArray(), kind, info);
    return !info->version.isEmpty();
}

QString linuxAppImagePath()
{
    const QString path = qEnvironmentVariable("APPIMAGE").trimmed();
    if (path.isEmpty()) {
        return {};
    }
    return QFileInfo(path).exists() ? QDir::cleanPath(path) : QString();
}

bool isRunningFromAppImage()
{
    return !linuxAppImagePath().isEmpty();
}

bool canReplacePath(const QString &path)
{
    if (path.trimmed().isEmpty()) {
        return false;
    }

    const QFileInfo info(path);
    const QFileInfo parent(info.absolutePath());
    if (!parent.exists() || !parent.isWritable()) {
        return false;
    }
    if (!info.exists()) {
        return true;
    }
    return info.isWritable() || parent.isWritable();
}

UpdateInstallMode currentInstallMode()
{
#if defined(Q_OS_MACOS)
    return UpdateInstallMode::ReplaceAppBundle;
#elif defined(Q_OS_WIN)
    return UpdateInstallMode::OverlayDirectory;
#else
    if (isRunningFromAppImage() && canReplacePath(linuxAppImagePath())) {
        return UpdateInstallMode::ReplaceAppImage;
    }
    return UpdateInstallMode::DownloadLinkOnly;
#endif
}

QString currentInstallDestination()
{
#if defined(Q_OS_MACOS)
    QDir dir(QCoreApplication::applicationDirPath());
    dir.cdUp();
    dir.cdUp();
    return QDir::cleanPath(dir.absolutePath());
#elif defined(Q_OS_WIN)
    return QDir::cleanPath(QCoreApplication::applicationDirPath());
#else
    if (isRunningFromAppImage()) {
        return linuxAppImagePath();
    }
    return {};
#endif
}

QString currentRelaunchPath()
{
#if defined(Q_OS_MACOS)
    return currentInstallDestination();
#elif defined(Q_OS_WIN)
    return QDir::cleanPath(QCoreApplication::applicationFilePath());
#else
    if (isRunningFromAppImage()) {
        return linuxAppImagePath();
    }
    return QDir::cleanPath(QCoreApplication::applicationFilePath());
#endif
}

QString findMacAppBundle(const QString &rootPath)
{
    return firstMatchingPath(rootPath, [](const QFileInfo &info) {
        return info.isDir() && info.fileName().endsWith(QStringLiteral(".app"), Qt::CaseInsensitive);
    });
}

QString findWindowsExeDirectory(const QString &rootPath)
{
    const QString exePath = firstMatchingPath(rootPath, [](const QFileInfo &info) {
        return info.isFile() && info.fileName().compare(QStringLiteral("MatrixMediaShareClientQt.exe"), Qt::CaseInsensitive) == 0;
    });
    if (exePath.isEmpty()) {
        return {};
    }
    return QFileInfo(exePath).absolutePath();
}

QString findAppImage(const QString &rootPath)
{
    return firstMatchingPath(rootPath, [](const QFileInfo &info) {
        return info.isFile() && info.fileName().endsWith(QStringLiteral(".AppImage"), Qt::CaseInsensitive);
    });
}

StagedUpdatePayload locateStagedPayload(const QString &extractDir, const UpdateInstallMode mode)
{
    StagedUpdatePayload payload;
    payload.rootPath = QDir::cleanPath(extractDir);
    payload.installMode = mode;
    switch (mode) {
    case UpdateInstallMode::ReplaceAppBundle:
        payload.payloadPath = findMacAppBundle(extractDir);
        break;
    case UpdateInstallMode::OverlayDirectory:
        payload.payloadPath = findWindowsExeDirectory(extractDir);
        break;
    case UpdateInstallMode::ReplaceAppImage:
        payload.payloadPath = findAppImage(extractDir);
        break;
    case UpdateInstallMode::DownloadLinkOnly:
        break;
    }
    return payload;
}

QString unixUpdateHelperScript()
{
    return QStringLiteral(
        R"SH(#!/usr/bin/env bash
set -euo pipefail

PID="${1:?}"
SRC="${2:?}"
DST="${3:?}"
MODE="${4:?}"
RELAUNCH="${5:?}"
RELAUNCH_PATH="${6:-}"

wait_for_exit() {
  local tries=0
  while kill -0 "${PID}" 2>/dev/null; do
    tries=$((tries + 1))
    if [[ "${tries}" -gt 240 ]]; then
      echo "Timed out waiting for process ${PID} to exit." >&2
      exit 1
    fi
    sleep 0.25
  done
}

replace_payload() {
  case "${MODE}" in
    appimage)
      chmod +x "${SRC}"
      mv -f "${SRC}" "${DST}"
      chmod +x "${DST}"
      ;;
    appbundle)
      local parent
      parent="$(dirname "${DST}")"
      mkdir -p "${parent}"
      rm -rf "${DST}"
      if command -v ditto >/dev/null 2>&1; then
        ditto "${SRC}" "${DST}"
      else
        cp -R "${SRC}" "${DST}"
      fi
      ;;
    directory)
      mkdir -p "${DST}"
      cp -R "${SRC}"/. "${DST}"/
      ;;
    *)
      echo "Unknown update install mode: ${MODE}" >&2
      exit 1
      ;;
  esac
}

shell_single_quote() {
  local value="$1"
  value="${value//\'/\'\\\'\'}"
  printf "'%s'" "${value}"
}

run_privileged_if_needed() {
  if [[ "${MODE}" != "appbundle" ]]; then
    replace_payload
    return
  fi
  local parent
  parent="$(dirname "${DST}")"
  if [[ -w "${parent}" ]] && { [[ ! -e "${DST}" ]] || [[ -w "${DST}" ]]; }; then
    replace_payload
    return
  fi
  if ! command -v osascript >/dev/null 2>&1; then
    echo "Destination is not writable and osascript is unavailable." >&2
    exit 1
  fi
  local quoted_src quoted_dst quoted_parent
  quoted_src="$(shell_single_quote "${SRC}")"
  quoted_dst="$(shell_single_quote "${DST}")"
  quoted_parent="$(shell_single_quote "${parent}")"
  osascript -e "do shell script \"mkdir -p ${quoted_parent} && rm -rf ${quoted_dst} && ditto ${quoted_src} ${quoted_dst}\" with administrator privileges"
}

wait_for_exit
run_privileged_if_needed

if [[ "${RELAUNCH}" == "1" ]]; then
  if [[ "${MODE}" == "appbundle" ]]; then
    open "${DST}"
  elif [[ -n "${RELAUNCH_PATH}" ]]; then
    nohup "${RELAUNCH_PATH}" >/dev/null 2>&1 &
  fi
fi
)SH");
}

QString windowsUpdateHelperScript()
{
    return QStringLiteral(
        R"PS(# Matrix Media Share Client Windows update helper. Do not open a browser.
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Destination,
    [Parameter(Mandatory = $true)][string]$Relaunch,
    [string]$RelaunchPath = ""
)

function Wait-ForProcessExit {
    param([int]$TargetPid)
    for ($i = 0; $i -lt 240; $i++) {
        if (-not (Get-Process -Id $TargetPid -ErrorAction SilentlyContinue)) {
            return
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for process $TargetPid to exit."
}

function Install-UpdatePayload {
    if (-not (Test-Path -LiteralPath $Destination)) {
        New-Item -ItemType Directory -Path $Destination | Out-Null
    }
    Copy-Item -Path (Join-Path $Source '*') -Destination $Destination -Recurse -Force
}

Wait-ForProcessExit -TargetPid $ProcessId
$installed = $false
try {
    Install-UpdatePayload
    $installed = $true
} catch {
    $sourceLiteral = $Source.Replace("'", "''")
    $destinationLiteral = $Destination.Replace("'", "''")
    $elevated = @"
if (-not (Test-Path -LiteralPath '$destinationLiteral')) { New-Item -ItemType Directory -Path '$destinationLiteral' | Out-Null }
Copy-Item -Path (Join-Path '$sourceLiteral' '*') -Destination '$destinationLiteral' -Recurse -Force
"@
    $elevatedProcess = Start-Process -FilePath "powershell.exe" -Verb RunAs -Wait -PassThru -ArgumentList @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-Command", $elevated
    )
    if ($null -eq $elevatedProcess -or $elevatedProcess.ExitCode -ne 0) {
        throw "Elevated Windows update install failed."
    }
    $installed = $true
}

if (-not $installed) {
    throw "Failed to install the staged Windows update."
}

if ($Relaunch -eq "1" -and $RelaunchPath) {
    Start-Process -FilePath $RelaunchPath
}
)PS");
}

bool writeTextFile(const QString &path, const QString &contents, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    const QByteArray payload = contents.toUtf8();
    if (file.write(payload) != payload.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    file.close();
#ifdef Q_OS_UNIX
    QFile::setPermissions(
        path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    return true;
}

QString installModeToken(const UpdateInstallMode mode)
{
    switch (mode) {
    case UpdateInstallMode::ReplaceAppBundle:
        return QStringLiteral("appbundle");
    case UpdateInstallMode::OverlayDirectory:
        return QStringLiteral("directory");
    case UpdateInstallMode::ReplaceAppImage:
        return QStringLiteral("appimage");
    case UpdateInstallMode::DownloadLinkOnly:
        return QStringLiteral("link");
    }
    return QStringLiteral("link");
}

QStringList unixHelperArguments(const UpdateHelperSpec &spec)
{
    return {
        QString::number(spec.pid),
        spec.sourcePath,
        spec.destinationPath,
        installModeToken(spec.installMode),
        spec.relaunch ? QStringLiteral("1") : QStringLiteral("0"),
        spec.relaunchPath,
    };
}

QStringList windowsHelperArguments(const UpdateHelperSpec &spec)
{
    return {
        QStringLiteral("-ProcessId"),
        QString::number(spec.pid),
        QStringLiteral("-Source"),
        spec.sourcePath,
        QStringLiteral("-Destination"),
        spec.destinationPath,
        QStringLiteral("-Relaunch"),
        spec.relaunch ? QStringLiteral("1") : QStringLiteral("0"),
        QStringLiteral("-RelaunchPath"),
        spec.relaunchPath,
    };
}

qint64 currentProcessId()
{
#ifdef Q_OS_WIN
    return static_cast<qint64>(_getpid());
#else
    return static_cast<qint64>(getpid());
#endif
}

}
