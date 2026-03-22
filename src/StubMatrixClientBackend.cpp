#include "StubMatrixClientBackend.h"

namespace {
QString unavailableMessage()
{
    return QStringLiteral("The Qt port UI is in place, but the Rust-backed Matrix bridge has not been wired yet.");
}
}

QString StubMatrixClientBackend::backendName() const
{
    return QStringLiteral("stub");
}

bool StubMatrixClientBackend::isAvailable() const
{
    return false;
}

void StubMatrixClientBackend::setRuntimeChangedCallback(RuntimeChangedCallback callback)
{
    Q_UNUSED(callback);
}

void StubMatrixClientBackend::setBackendErrorCallback(BackendErrorCallback callback)
{
    Q_UNUSED(callback);
}

bool StubMatrixClientBackend::start(const AppSettings &settings, const QString &password, BotRuntimeSnapshot &runtime, QString &errorMessage)
{
    Q_UNUSED(settings);
    Q_UNUSED(password);
    runtime.connectionState = ConnectionState::Error;
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::stop(BotRuntimeSnapshot &runtime, QString &errorMessage)
{
    runtime = BotRuntimeSnapshot {};
    runtime.connectionState = ConnectionState::Stopped;
    errorMessage.clear();
    return true;
}

bool StubMatrixClientBackend::saveSettings(const AppSettings &settings, const QString &password, QString &errorMessage)
{
    Q_UNUSED(settings);
    Q_UNUSED(password);
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::resetHistoryScans(QString &errorMessage)
{
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::shareLocalFile(const QString &roomId, const QString &filePath, QString &errorMessage)
{
    Q_UNUSED(roomId);
    Q_UNUSED(filePath);
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::shareLocalFiles(const QString &roomId, const QStringList &filePaths, QString &errorMessage)
{
    Q_UNUSED(roomId);
    Q_UNUSED(filePaths);
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::importIpfsLink(const QString &link, QString &errorMessage)
{
    Q_UNUSED(link);
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::openDiscovery(const QString &roomId, const QString &eventId, QString &errorMessage)
{
    Q_UNUSED(roomId);
    Q_UNUSED(eventId);
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::refreshCatalog(QString &errorMessage)
{
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::joinRoom(const QString &roomIdOrAlias, QString &errorMessage)
{
    Q_UNUSED(roomIdOrAlias);
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::leaveRoom(const QString &roomId, QString &errorMessage)
{
    Q_UNUSED(roomId);
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::requestVerification(QString &errorMessage)
{
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::startSasVerification(QString &errorMessage)
{
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::approveVerification(QString &errorMessage)
{
    errorMessage = unavailableMessage();
    return false;
}

bool StubMatrixClientBackend::declineVerification(QString &errorMessage)
{
    errorMessage = unavailableMessage();
    return false;
}
