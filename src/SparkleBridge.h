#pragma once

#include <QObject>
#include <QString>

class SparkleBridge : public QObject
{
    Q_OBJECT

public:
    explicit SparkleBridge(QObject *parent = nullptr);
    ~SparkleBridge() override;

    bool start(QString *errorMessage = nullptr);
    bool isStarted() const;
    void checkForUpdates(bool userInitiated);
    void installNow();
    void installLater();

    bool isBusy() const;
    bool hasStagedUpdate() const;
    bool isDownloading() const;
    QString stagedVersion() const;
    qint64 downloadReceivedBytes() const;
    qint64 downloadTotalBytes() const;

    void notifyStaged(const QString &version);
    void notifyFailed(const QString &message);
    void notifyDownloadProgress(qint64 received, qint64 total);
    void notifyBusyChanged();
    bool consumeForcePrompt();

signals:
    void stateChanged();
    void stagedUpdateReady(const QString &version, bool forcePrompt);
    void updateFailed(const QString &message);

private:
    struct Private;
    Private *d_ = nullptr;
};
