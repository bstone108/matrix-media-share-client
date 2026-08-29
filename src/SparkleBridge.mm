#include "SparkleBridge.h"

#import <Sparkle/Sparkle.h>

#include <QCoreApplication>

@interface MmsSparkleDriver : NSObject <SPUUserDriver, SPUUpdaterDelegate>
@property (nonatomic, assign) SparkleBridge *bridge;
@property (nonatomic, copy) void (^readyToInstallReply)(SPUUserUpdateChoice);
@property (nonatomic, copy) NSString *stagedVersion;
@property (nonatomic, assign) BOOL busy;
@property (nonatomic, assign) BOOL downloading;
@property (nonatomic, assign) uint64_t downloadReceived;
@property (nonatomic, assign) uint64_t downloadTotal;
- (void)clearReadyReplyWithChoice:(SPUUserUpdateChoice)choice;
@end

@implementation MmsSparkleDriver

- (NSString *)feedURLStringForUpdater:(SPUUpdater *)updater
{
    Q_UNUSED(updater);
#if defined(__arm64__)
    return @"https://github.com/bstone108/matrix-media-share-client/releases/latest/download/appcast-macos-arm64.xml";
#else
    return @"https://github.com/bstone108/matrix-media-share-client/releases/latest/download/appcast-macos-x86_64.xml";
#endif
}

- (void)showUpdatePermissionRequest:(SPUUpdatePermissionRequest *)request
                              reply:(void (^)(SUUpdatePermissionResponse *))reply
{
    Q_UNUSED(request);
    reply([[SUUpdatePermissionResponse alloc] initWithAutomaticUpdateChecks:YES sendSystemProfile:NO]);
}

- (void)showUserInitiatedUpdateCheckWithCancellation:(void (^)(void))cancellation
{
    Q_UNUSED(cancellation);
    self.busy = YES;
    if (self.bridge != nullptr) {
        self.bridge->notifyBusyChanged();
    }
}

- (void)showUpdateFoundWithAppcastItem:(SUAppcastItem *)appcastItem
                                 state:(SPUUserUpdateState *)state
                                 reply:(void (^)(SPUUserUpdateChoice))reply
{
    Q_UNUSED(state);
    if (appcastItem.informationOnlyUpdate) {
        reply(SPUUserUpdateChoiceDismiss);
        return;
    }
    self.stagedVersion = appcastItem.displayVersionString ?: appcastItem.versionString;
    self.busy = YES;
    reply(SPUUserUpdateChoiceInstall);
    if (self.bridge != nullptr) {
        self.bridge->notifyBusyChanged();
    }
}

- (void)showUpdateReleaseNotesWithDownloadData:(SPUDownloadData *)downloadData
{
    Q_UNUSED(downloadData);
}

- (void)showUpdateReleaseNotesFailedToDownloadWithError:(NSError *)error
{
    Q_UNUSED(error);
}

- (void)showUpdateNotFoundWithError:(NSError *)error acknowledgement:(void (^)(void))acknowledgement
{
    Q_UNUSED(error);
    self.busy = NO;
    self.downloading = NO;
    if (self.bridge != nullptr) {
        self.bridge->notifyBusyChanged();
    }
    acknowledgement();
}

- (void)showUpdaterError:(NSError *)error acknowledgement:(void (^)(void))acknowledgement
{
    const QString message = error.localizedDescription != nil
        ? QString::fromNSString(error.localizedDescription)
        : QStringLiteral("Sparkle update failed.");
    self.busy = NO;
    self.downloading = NO;
    if (self.bridge != nullptr) {
        self.bridge->notifyFailed(message);
    }
    acknowledgement();
}

- (void)showDownloadInitiatedWithCancellation:(void (^)(void))cancellation
{
    Q_UNUSED(cancellation);
    self.downloading = YES;
    self.busy = YES;
    self.downloadReceived = 0;
    self.downloadTotal = 0;
    if (self.bridge != nullptr) {
        self.bridge->notifyDownloadProgress(0, -1);
    }
}

- (void)showDownloadDidReceiveExpectedContentLength:(uint64_t)expectedContentLength
{
    self.downloadTotal = expectedContentLength;
    if (self.bridge != nullptr) {
        self.bridge->notifyDownloadProgress(static_cast<qint64>(self.downloadReceived), static_cast<qint64>(self.downloadTotal));
    }
}

- (void)showDownloadDidReceiveDataOfLength:(uint64_t)length
{
    self.downloadReceived += length;
    if (self.bridge != nullptr) {
        self.bridge->notifyDownloadProgress(static_cast<qint64>(self.downloadReceived), static_cast<qint64>(self.downloadTotal));
    }
}

- (void)showDownloadDidStartExtractingUpdate
{
    self.downloading = NO;
    if (self.bridge != nullptr) {
        self.bridge->notifyBusyChanged();
    }
}

- (void)showExtractionReceivedProgress:(double)progress
{
    Q_UNUSED(progress);
}

- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply
{
    self.readyToInstallReply = reply;
    self.busy = NO;
    self.downloading = NO;
    const QString version = self.stagedVersion != nil ? QString::fromNSString(self.stagedVersion) : QString();
    if (self.bridge != nullptr) {
        self.bridge->notifyStaged(version);
    }
}

- (void)showInstallingUpdateWithApplicationTerminated:(BOOL)applicationTerminated
                         retryTerminatingApplication:(void (^)(void))retryTerminatingApplication
{
    Q_UNUSED(applicationTerminated);
    Q_UNUSED(retryTerminatingApplication);
}

- (void)showUpdateInstalledAndRelaunched:(BOOL)relaunched acknowledgement:(void (^)(void))acknowledgement
{
    Q_UNUSED(relaunched);
    acknowledgement();
}

- (void)dismissUpdateInstallation
{
    self.busy = NO;
    self.downloading = NO;
    self.readyToInstallReply = nil;
    if (self.bridge != nullptr) {
        self.bridge->notifyBusyChanged();
    }
}

- (void)clearReadyReplyWithChoice:(SPUUserUpdateChoice)choice
{
    if (self.readyToInstallReply != nil) {
        void (^reply)(SPUUserUpdateChoice) = self.readyToInstallReply;
        self.readyToInstallReply = nil;
        reply(choice);
    }
}

@end

struct SparkleBridge::Private {
    MmsSparkleDriver *driver = nil;
    SPUUpdater *updater = nil;
    bool forcePrompt = false;
    bool staged = false;
    QString stagedVersion;
    qint64 received = 0;
    qint64 total = -1;
};

SparkleBridge::SparkleBridge(QObject *parent)
    : QObject(parent)
    , d_(new Private)
{
}

SparkleBridge::~SparkleBridge()
{
    if (d_->driver != nil) {
        d_->driver.bridge = nullptr;
    }
    delete d_;
}

bool SparkleBridge::start(QString *errorMessage)
{
    if (d_->updater != nil) {
        return true;
    }

    d_->driver = [MmsSparkleDriver new];
    d_->driver.bridge = this;
    d_->updater = [[SPUUpdater alloc] initWithHostBundle:[NSBundle mainBundle]
                                       applicationBundle:[NSBundle mainBundle]
                                              userDriver:d_->driver
                                                delegate:d_->driver];
    d_->updater.automaticallyChecksForUpdates = NO;
    d_->updater.automaticallyDownloadsUpdates = YES;

    NSError *error = nil;
    if (![d_->updater startUpdater:&error]) {
        d_->updater = nil;
        if (errorMessage != nullptr) {
            *errorMessage = error != nil
                ? QString::fromNSString(error.localizedDescription)
                : QStringLiteral("Sparkle failed to start.");
        }
        return false;
    }
    return true;
}

bool SparkleBridge::isStarted() const
{
    return d_->updater != nil;
}

void SparkleBridge::checkForUpdates(const bool userInitiated)
{
    if (d_->updater == nil) {
        return;
    }
    d_->forcePrompt = userInitiated;
    if (userInitiated) {
        [d_->updater checkForUpdates];
    } else {
        [d_->updater checkForUpdatesInBackground];
    }
}

void SparkleBridge::installNow()
{
    [d_->driver clearReadyReplyWithChoice:SPUUserUpdateChoiceInstall];
}

void SparkleBridge::installLater()
{
    [d_->driver clearReadyReplyWithChoice:SPUUserUpdateChoiceDismiss];
}

bool SparkleBridge::isBusy() const
{
    return d_->driver != nil && d_->driver.busy;
}

bool SparkleBridge::hasStagedUpdate() const
{
    return d_->staged;
}

bool SparkleBridge::isDownloading() const
{
    return d_->driver != nil && d_->driver.downloading;
}

QString SparkleBridge::stagedVersion() const
{
    return d_->stagedVersion;
}

qint64 SparkleBridge::downloadReceivedBytes() const
{
    return d_->received;
}

qint64 SparkleBridge::downloadTotalBytes() const
{
    return d_->total;
}

void SparkleBridge::notifyStaged(const QString &version)
{
    d_->staged = true;
    d_->stagedVersion = version;
    const bool forcePrompt = consumeForcePrompt();
    emit stagedUpdateReady(version, forcePrompt);
    emit stateChanged();
}

void SparkleBridge::notifyFailed(const QString &message)
{
    d_->staged = false;
    emit updateFailed(message);
    emit stateChanged();
}

void SparkleBridge::notifyDownloadProgress(const qint64 received, const qint64 total)
{
    d_->received = received;
    d_->total = total;
    emit stateChanged();
}

void SparkleBridge::notifyBusyChanged()
{
    emit stateChanged();
}

bool SparkleBridge::consumeForcePrompt()
{
    const bool value = d_->forcePrompt;
    d_->forcePrompt = false;
    return value;
}
