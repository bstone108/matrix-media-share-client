#include "UpdateUtilities.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest/QTest>

class UpdateUtilityTests : public QObject
{
    Q_OBJECT

private slots:
    void versionComparisonTreatsDateBuildTagsAsNewer();
    void updateCheckIsDueEveryTwoDays();
    void neverNagsTheSameVersionTwice();
    void selectsMatchingGithubReleaseAsset();
    void locatesStagedPayloads();
    void unixHelperReplacesAppImageAfterPidExits();
    void unixHelperOverlaysDirectoryAfterPidExits();
    void linuxNonAppImageUsesDownloadLinkOnly();
};

void UpdateUtilityTests::versionComparisonTreatsDateBuildTagsAsNewer()
{
    QCOMPARE(UpdateUtilities::normalizeReleaseVersion(QStringLiteral("v2026.8.28.1")), QStringLiteral("2026.8.28.1"));
    QCOMPARE(UpdateUtilities::compareVersionStrings(QStringLiteral("v2026.8.28.2"), QStringLiteral("2026.8.28.1")), 1);
    QCOMPARE(UpdateUtilities::compareVersionStrings(QStringLiteral("2026.8.28.1"), QStringLiteral("v2026.8.28.1")), 0);
    QCOMPARE(UpdateUtilities::compareVersionStrings(QStringLiteral("2026.8.27.9"), QStringLiteral("2026.8.28.1")), -1);
    QVERIFY(UpdateUtilities::isNewerVersion(QStringLiteral("v2026.8.28.2"), QStringLiteral("2026.8.28.1")));
    QVERIFY(!UpdateUtilities::isNewerVersion(QStringLiteral("2026.8.28.1"), QStringLiteral("2026.8.28.1")));
}

void UpdateUtilityTests::updateCheckIsDueEveryTwoDays()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-08-28T12:00:00Z"), Qt::ISODate);
    QVERIFY(UpdateUtilities::isUpdateCheckDue({}, now));

    const QDateTime oneDayAgo = now.addSecs(-24 * 60 * 60);
    QVERIFY(!UpdateUtilities::isUpdateCheckDue(oneDayAgo, now));

    const QDateTime twoDaysAgo = now.addSecs(-UpdateUtilities::kUpdateCheckIntervalSeconds);
    QVERIFY(UpdateUtilities::isUpdateCheckDue(twoDaysAgo, now));
}

void UpdateUtilityTests::neverNagsTheSameVersionTwice()
{
    QVERIFY(UpdateUtilities::shouldNagForVersion(QStringLiteral("2026.8.28.2"), QStringLiteral("2026.8.28.1"), {}));
    QVERIFY(!UpdateUtilities::shouldNagForVersion(QStringLiteral("2026.8.28.2"), QStringLiteral("2026.8.28.1"), QStringLiteral("v2026.8.28.2")));
    QVERIFY(UpdateUtilities::shouldNagForVersion(QStringLiteral("2026.8.28.3"), QStringLiteral("2026.8.28.1"), QStringLiteral("2026.8.28.2")));
    QVERIFY(!UpdateUtilities::shouldNagForVersion(QStringLiteral("2026.8.28.1"), QStringLiteral("2026.8.28.1"), {}));
}

void UpdateUtilityTests::selectsMatchingGithubReleaseAsset()
{
    const QJsonDocument document = QJsonDocument::fromJson(QByteArrayLiteral(R"JSON({
      "tag_name": "v2026.8.28.1",
      "html_url": "https://github.com/bstone108/matrix-media-share-client/releases/tag/v2026.8.28.1",
      "name": "Matrix Media Share Client 2026.8.28.1",
      "published_at": "2026-08-28T22:40:22Z",
      "assets": [
        {"name": "MatrixMediaShareClientQt-2026.8.28.1-macos-arm64.dmg", "browser_download_url": "https://example.test/macos-arm64.dmg", "size": 1},
        {"name": "MatrixMediaShareClientQt-2026.8.28.1-macos-arm64.zip", "browser_download_url": "https://example.test/macos-arm64.zip", "size": 11},
        {"name": "MatrixMediaShareClientQt-2026.8.28.1-macos-x86_64.zip", "browser_download_url": "https://example.test/macos-x86_64.zip", "size": 12},
        {"name": "MatrixMediaShareClientQt-2026.8.28.1-windows-x64.zip", "browser_download_url": "https://example.test/windows-x64.zip", "size": 13},
        {"name": "MatrixMediaShareClientQt-2026.8.28.1-windows-arm64.zip", "browser_download_url": "https://example.test/windows-arm64.zip", "size": 14},
        {"name": "MatrixMediaShareClientQt-2026.8.28.1-linux-x86_64-appimage.zip", "browser_download_url": "https://example.test/linux-x86_64.zip", "size": 15},
        {"name": "MatrixMediaShareClientQt-2026.8.28.1-linux-aarch64-appimage.zip", "browser_download_url": "https://example.test/linux-aarch64.zip", "size": 16}
      ]
    })JSON"));

    GithubReleaseInfo macosArm;
    QVERIFY(UpdateUtilities::parseGithubRelease(document.object(), UpdatePackageKind::MacosArm64Zip, &macosArm));
    QCOMPARE(macosArm.version, QStringLiteral("2026.8.28.1"));
    QCOMPARE(macosArm.assetName, QStringLiteral("MatrixMediaShareClientQt-2026.8.28.1-macos-arm64.zip"));
    QCOMPARE(macosArm.assetUrl, QStringLiteral("https://example.test/macos-arm64.zip"));

    GithubReleaseInfo macosIntel;
    QVERIFY(UpdateUtilities::parseGithubRelease(document.object(), UpdatePackageKind::MacosX86_64Zip, &macosIntel));
    QCOMPARE(macosIntel.assetName, QStringLiteral("MatrixMediaShareClientQt-2026.8.28.1-macos-x86_64.zip"));

    GithubReleaseInfo windowsX64;
    QVERIFY(UpdateUtilities::parseGithubRelease(document.object(), UpdatePackageKind::WindowsX64Zip, &windowsX64));
    QCOMPARE(windowsX64.assetName, QStringLiteral("MatrixMediaShareClientQt-2026.8.28.1-windows-x64.zip"));

    GithubReleaseInfo windowsArm;
    QVERIFY(UpdateUtilities::parseGithubRelease(document.object(), UpdatePackageKind::WindowsArm64Zip, &windowsArm));
    QCOMPARE(windowsArm.assetName, QStringLiteral("MatrixMediaShareClientQt-2026.8.28.1-windows-arm64.zip"));

    GithubReleaseInfo linuxX64;
    QVERIFY(UpdateUtilities::parseGithubRelease(document.object(), UpdatePackageKind::LinuxX86_64AppImageZip, &linuxX64));
    QCOMPARE(linuxX64.assetName, QStringLiteral("MatrixMediaShareClientQt-2026.8.28.1-linux-x86_64-appimage.zip"));

    GithubReleaseInfo linuxArm;
    QVERIFY(UpdateUtilities::parseGithubRelease(document.object(), UpdatePackageKind::LinuxAarch64AppImageZip, &linuxArm));
    QCOMPARE(linuxArm.assetName, QStringLiteral("MatrixMediaShareClientQt-2026.8.28.1-linux-aarch64-appimage.zip"));

    QCOMPARE(UpdateUtilities::assetSuffix(UpdatePackageKind::MacosArm64Zip), QStringLiteral("macos-arm64.zip"));
    QVERIFY(!UpdateUtilities::assetNameMatches(QStringLiteral("MatrixMediaShareClientQt-2026.8.28.1-macos-arm64.dmg"), UpdatePackageKind::MacosArm64Zip));
}

void UpdateUtilityTests::locatesStagedPayloads()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString macApp = dir.filePath(QStringLiteral("MatrixMediaShareClientQt.app"));
    QVERIFY(QDir().mkpath(macApp + QStringLiteral("/Contents/MacOS")));
    const StagedUpdatePayload macPayload = UpdateUtilities::locateStagedPayload(dir.path(), UpdateInstallMode::ReplaceAppBundle);
    QCOMPARE(QFileInfo(macPayload.payloadPath).fileName(), QStringLiteral("MatrixMediaShareClientQt.app"));

    const QString windowsRoot = dir.filePath(QStringLiteral("windows"));
    QVERIFY(QDir().mkpath(windowsRoot));
    QFile windowsExe(windowsRoot + QStringLiteral("/MatrixMediaShareClientQt.exe"));
    QVERIFY(windowsExe.open(QIODevice::WriteOnly));
    windowsExe.write("exe");
    windowsExe.close();
    const StagedUpdatePayload windowsPayload = UpdateUtilities::locateStagedPayload(windowsRoot, UpdateInstallMode::OverlayDirectory);
    QCOMPARE(windowsPayload.payloadPath, QDir::cleanPath(windowsRoot));

    const QString linuxRoot = dir.filePath(QStringLiteral("linux"));
    QVERIFY(QDir().mkpath(linuxRoot));
    QFile appImage(linuxRoot + QStringLiteral("/MatrixMediaShareClientQt-x86_64.AppImage"));
    QVERIFY(appImage.open(QIODevice::WriteOnly));
    appImage.write("appimage");
    appImage.close();
    const StagedUpdatePayload linuxPayload = UpdateUtilities::locateStagedPayload(linuxRoot, UpdateInstallMode::ReplaceAppImage);
    QVERIFY(linuxPayload.payloadPath.endsWith(QStringLiteral(".AppImage")));
}

void UpdateUtilityTests::unixHelperReplacesAppImageAfterPidExits()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString target = dir.filePath(QStringLiteral("MatrixMediaShareClientQt-x86_64.AppImage"));
    const QString staged = dir.filePath(QStringLiteral("MatrixMediaShareClientQt-x86_64.AppImage.new"));
    const QString helper = dir.filePath(QStringLiteral("update-helper.sh"));

    QFile oldFile(target);
    QVERIFY(oldFile.open(QIODevice::WriteOnly));
    oldFile.write("old-appimage");
    oldFile.close();

    QFile newFile(staged);
    QVERIFY(newFile.open(QIODevice::WriteOnly));
    newFile.write("new-appimage");
    newFile.close();

    QString writeError;
    QVERIFY(UpdateUtilities::writeTextFile(helper, UpdateUtilities::unixUpdateHelperScript(), &writeError));

    UpdateHelperSpec spec;
    spec.pid = 999999;
    spec.sourcePath = staged;
    spec.destinationPath = target;
    spec.installMode = UpdateInstallMode::ReplaceAppImage;
    spec.relaunch = false;
    spec.relaunchPath = target;

    QProcess process;
    QStringList arguments = {helper};
    arguments.append(UpdateUtilities::unixHelperArguments(spec));
    process.start(QStringLiteral("/bin/bash"), arguments);
    QVERIFY2(process.waitForFinished(10000), qPrintable(QString::fromUtf8(process.readAllStandardError())));
    QCOMPARE(process.exitCode(), 0);

    QFile updated(target);
    QVERIFY(updated.open(QIODevice::ReadOnly));
    QCOMPARE(updated.readAll(), QByteArray("new-appimage"));
    QVERIFY(!QFile::exists(staged));
}

void UpdateUtilityTests::unixHelperOverlaysDirectoryAfterPidExits()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString installDir = dir.filePath(QStringLiteral("install"));
    const QString stagedDir = dir.filePath(QStringLiteral("staged"));
    QVERIFY(QDir().mkpath(installDir));
    QVERIFY(QDir().mkpath(stagedDir));

    QFile oldExe(installDir + QStringLiteral("/MatrixMediaShareClientQt.exe"));
    QVERIFY(oldExe.open(QIODevice::WriteOnly));
    oldExe.write("old-exe");
    oldExe.close();

    QFile newExe(stagedDir + QStringLiteral("/MatrixMediaShareClientQt.exe"));
    QVERIFY(newExe.open(QIODevice::WriteOnly));
    newExe.write("new-exe");
    newExe.close();

    const QString helper = dir.filePath(QStringLiteral("update-helper.sh"));
    QVERIFY(UpdateUtilities::writeTextFile(helper, UpdateUtilities::unixUpdateHelperScript()));

    UpdateHelperSpec spec;
    spec.pid = 999999;
    spec.sourcePath = stagedDir;
    spec.destinationPath = installDir;
    spec.installMode = UpdateInstallMode::OverlayDirectory;
    spec.relaunch = false;

    QProcess process;
    QStringList arguments = {helper};
    arguments.append(UpdateUtilities::unixHelperArguments(spec));
    process.start(QStringLiteral("/bin/bash"), arguments);
    QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.exitCode(), 0);

    QFile updated(installDir + QStringLiteral("/MatrixMediaShareClientQt.exe"));
    QVERIFY(updated.open(QIODevice::ReadOnly));
    QCOMPARE(updated.readAll(), QByteArray("new-exe"));
}

void UpdateUtilityTests::linuxNonAppImageUsesDownloadLinkOnly()
{
#ifdef Q_OS_LINUX
    qunsetenv("APPIMAGE");
    QCOMPARE(UpdateUtilities::currentInstallMode(), UpdateInstallMode::DownloadLinkOnly);
    QVERIFY(!UpdateUtilities::isRunningFromAppImage());
#else
    QSKIP("Linux-only install-mode check");
#endif
}

QTEST_MAIN(UpdateUtilityTests)

#include "UpdateUtilityTests.moc"
