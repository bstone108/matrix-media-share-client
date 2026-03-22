#pragma once

#include <QString>

class AppPaths
{
public:
    AppPaths();

    QString rootPath() const;
    QString appSupportPath() const;
    QString databasePath() const;
    QString matrixDataPath() const;
    QString matrixCachePath() const;
    QString tempDownloadsPath() const;
    QString libraryPath() const;
    QString manualDownloadsPath() const;
    QString landingPagesPath() const;
    QString kuboPath() const;
    QString kuboRepoPath() const;
    QString secretStorePath() const;

private:
    void ensureDirectory(const QString &path) const;
    void lockDownPermissions(const QString &path) const;

    QString rootPath_;
    QString appSupportPath_;
    QString databasePath_;
    QString matrixDataPath_;
    QString matrixCachePath_;
    QString tempDownloadsPath_;
    QString libraryPath_;
    QString manualDownloadsPath_;
    QString landingPagesPath_;
    QString kuboPath_;
    QString kuboRepoPath_;
    QString secretStorePath_;
};
