#pragma once

#include "MatrixClientBackend.h"

class StubMatrixClientBackend final : public MatrixClientBackend
{
public:
    QString backendName() const override;
    bool isAvailable() const override;
    void setRuntimeChangedCallback(RuntimeChangedCallback callback) override;
    void setBackendErrorCallback(BackendErrorCallback callback) override;

    bool start(const AppSettings &settings, const QString &password, BotRuntimeSnapshot &runtime, QString &errorMessage) override;
    bool stop(BotRuntimeSnapshot &runtime, QString &errorMessage) override;
    bool saveSettings(const AppSettings &settings, const QString &password, QString &errorMessage) override;
    bool resetHistoryScans(QString &errorMessage) override;
    bool shareLocalFile(const QString &roomId, const QString &filePath, QString &errorMessage) override;
    bool shareLocalFiles(const QString &roomId, const QStringList &filePaths, QString &errorMessage) override;
    bool importIpfsLink(const QString &link, QString &errorMessage) override;
    bool openDiscovery(const QString &roomId, const QString &eventId, QString &errorMessage) override;
    bool refreshCatalog(QString &errorMessage) override;

    bool joinRoom(const QString &roomIdOrAlias, QString &errorMessage) override;
    bool leaveRoom(const QString &roomId, QString &errorMessage) override;

    bool requestVerification(QString &errorMessage) override;
    bool startSasVerification(QString &errorMessage) override;
    bool approveVerification(QString &errorMessage) override;
    bool declineVerification(QString &errorMessage) override;
};
