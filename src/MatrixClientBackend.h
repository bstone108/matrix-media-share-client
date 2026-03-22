#pragma once

#include "Domain.h"

#include <functional>
#include <QString>
#include <QStringList>

class MatrixClientBackend
{
public:
    using RuntimeChangedCallback = std::function<void(const BotRuntimeSnapshot &)>;
    using BackendErrorCallback = std::function<void(const QString &)>;

    virtual ~MatrixClientBackend() = default;

    virtual QString backendName() const = 0;
    virtual bool isAvailable() const = 0;
    virtual void setRuntimeChangedCallback(RuntimeChangedCallback callback) = 0;
    virtual void setBackendErrorCallback(BackendErrorCallback callback) = 0;

    virtual bool start(const AppSettings &settings, const QString &password, BotRuntimeSnapshot &runtime, QString &errorMessage) = 0;
    virtual bool stop(BotRuntimeSnapshot &runtime, QString &errorMessage) = 0;
    virtual bool saveSettings(const AppSettings &settings, const QString &password, QString &errorMessage) = 0;
    virtual bool resetHistoryScans(QString &errorMessage) = 0;
    virtual bool shareLocalFile(const QString &roomId, const QString &filePath, QString &errorMessage) = 0;
    virtual bool shareLocalFiles(const QString &roomId, const QStringList &filePaths, QString &errorMessage) = 0;
    virtual bool importIpfsLink(const QString &link, QString &errorMessage) = 0;
    virtual bool openDiscovery(const QString &roomId, const QString &eventId, QString &errorMessage) = 0;
    virtual bool refreshCatalog(QString &errorMessage) = 0;

    virtual bool joinRoom(const QString &roomIdOrAlias, QString &errorMessage) = 0;
    virtual bool leaveRoom(const QString &roomId, QString &errorMessage) = 0;

    virtual bool requestVerification(QString &errorMessage) = 0;
    virtual bool startSasVerification(QString &errorMessage) = 0;
    virtual bool approveVerification(QString &errorMessage) = 0;
    virtual bool declineVerification(QString &errorMessage) = 0;
};
