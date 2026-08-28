#pragma once

#include "Domain.h"

#include <QHash>
#include <QIcon>
#include <QMainWindow>
#include <QSet>

class AppController;
class QCloseEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QMoveEvent;
class QNetworkAccessManager;
class QResizeEvent;
class QShowEvent;
class QCheckBox;
class QComboBox;
class QDialog;
class QAudioOutput;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMediaPlayer;
class QNetworkReply;
class QProgressBar;
class QPushButton;
class QScrollBar;
class QScrollArea;
class QSizePolicy;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTextEdit;
class VideoFrameWidget;
class VlcPlayerWidget;
class WebVideoWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(AppController *controller, QWidget *parent = nullptr);

private:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    void constrainToAvailableGeometry();
    void scheduleWindowConstraint();
    void refreshView();
    void populateSectionNavigation();
    void populateRoomSidebar();
    void populateRoomsPage();
    void populateBrowserPage();
    void populateLibraryPage();
    void populateTransfersPage();
    void populateLogsPage();
    void populateSettingsPage();
    void populateVerificationPage();
    void refreshViewerDialog();

    QWidget *buildRoomsPage();
    QWidget *buildBrowserPage();
    QWidget *buildLibraryPage();
    QWidget *buildTransfersPage();
    QWidget *buildLogsPage();
    QWidget *buildSettingsPage();
    QWidget *buildVerificationPage();

    AppSettings gatherSettingsFromUi() const;
    bool saveSettingsFromUi(bool interactive);
    void markSettingsDirty();
    void applyDiscoveryPresentation(QListWidgetItem *item, const AttachmentDiscovery &discovery);
    const ActiveDownloadSnapshot *activeDownloadForDiscovery(const AttachmentDiscovery &discovery) const;
    const DownloadJobRecord *jobForDiscovery(const AttachmentDiscovery &discovery) const;
    void resetBrowserPageState();
    void loadMoreBrowserDiscoveries(bool reset = false);
    void maybeLoadMoreBrowserDiscoveries();
    void syncBrowserLoadedDiscoveries();
    void requestVisibleBrowserThumbnails();
    void scheduleBackgroundBrowserThumbnailPrefetch();
    void trimBrowserDiscoveryWindow();
    QString currentBrowserSelectedEventId() const;
    void restoreBrowserSelectionByEventId(const QString &eventId);
    QSet<QString> currentVisibleBrowserThumbnailKeys() const;
    QString browserThumbnailKey(const AttachmentDiscovery &discovery) const;
    QString browserThumbnailUrl(const AttachmentDiscovery &discovery) const;
    QIcon placeholderDiscoveryIcon(const AttachmentDiscovery &discovery) const;
    bool primeBrowserThumbnailIconCache(const AttachmentDiscovery &discovery);
    void clearBrowserThumbnailRequests();
    void enqueueBrowserThumbnailRequest(const AttachmentDiscovery &discovery, bool highPriority);
    void reprioritizeBrowserThumbnailRequests(const QSet<QString> &visibleKeys);
    void pumpBrowserThumbnailRequests();
    void requestBrowserThumbnail(const AttachmentDiscovery &discovery, bool foreground);
    void updateBrowserThumbnailItems(const QString &cacheKey, const QIcon &icon);
    void cacheBrowserThumbnailIcon(const QString &cacheKey, const QIcon &icon);
    void resetLogsPageState();
    void loadMoreLogs(bool reset = false);
    void maybeLoadMoreLogs();
    void trimLogWindow();
    void ensureViewerDialog();
    void loadViewerMedia(const ViewerSnapshot &viewer);
    void openSelectedSharedItem();
    void deleteSelectedSharedItem();
    QString sharedItemLocalPath(const SharedItemRecord &item) const;
    QString sharedItemThumbnailPath(const SharedItemRecord &item) const;
    QString sharedItemOriginLabel(const SharedItemRecord &item) const;
    QString sharedItemSignature(const QVector<SharedItemRecord> &items) const;
    QIcon sharedItemIcon(const SharedItemRecord &item);
    void openLocalViewerFile(const QString &filePath, const QString &displayName, const QString &mimeType, MediaCategory category);
    AppSection currentSection() const;
    QVector<RoomRecord> roomSidebarRoomsForCurrentSection() const;
    QString roomDisplayTitle(const RoomRecord &room) const;
    QString selectedRoomId() const;
    QString selectedBrowserRoomId() const;
    bool isUploadableBrowserRoom(const QString &roomId) const;
    bool currentSectionUsesRoomSidebar() const;
    void updateRoomSidebarVisibility();
    void applyTheme(bool darkModeEnabled);

    AppController *controller_ = nullptr;

    QComboBox *sectionCombo_ = nullptr;
    QWidget *roomSidebarContainer_ = nullptr;
    QLabel *roomSidebarTitleLabel_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    QNetworkAccessManager *thumbnailNetworkManager_ = nullptr;
    QTimer *windowConstraintTimer_ = nullptr;

    QListWidget *roomsList_ = nullptr;
    QLineEdit *joinRoomEdit_ = nullptr;
    QLabel *roomDetailLabel_ = nullptr;
    QPushButton *leaveRoomButton_ = nullptr;

    QCheckBox *powerToggle_ = nullptr;
    QLabel *connectionLabel_ = nullptr;
    QLabel *ipfsStatusLabel_ = nullptr;
    QLabel *uploadLimitLabel_ = nullptr;
    QLabel *pendingUploadsLabel_ = nullptr;
    QLabel *previewGenerationLabel_ = nullptr;
    QLabel *updateBannerLabel_ = nullptr;
    QLabel *browserSelectedRoomLabel_ = nullptr;
    QLabel *browserDropHintLabel_ = nullptr;
    QWidget *browserPage_ = nullptr;
    QListWidget *discoveriesList_ = nullptr;
    QPushButton *shareFilesButton_ = nullptr;
    QPushButton *openDiscoveryButton_ = nullptr;
    QPushButton *downloadDiscoveryButton_ = nullptr;

    QListWidget *libraryList_ = nullptr;
    QPushButton *openLibraryButton_ = nullptr;
    QPushButton *deleteLibraryButton_ = nullptr;

    QLabel *queueStatsLabel_ = nullptr;
    QListWidget *pendingUploadsList_ = nullptr;
    QListWidget *activeDownloadsList_ = nullptr;
    QTableWidget *waitingJobsTable_ = nullptr;
    QTableWidget *failedJobsTable_ = nullptr;

    QLabel *logsSummaryLabel_ = nullptr;
    QCheckBox *logProblemsOnlyCheck_ = nullptr;
    QTableWidget *logsTable_ = nullptr;

    QLineEdit *homeserverEdit_ = nullptr;
    QLineEdit *usernameEdit_ = nullptr;
    QLineEdit *passwordEdit_ = nullptr;
    QLineEdit *destinationEdit_ = nullptr;
    QLineEdit *libraryEdit_ = nullptr;
    QLineEdit *archiveEdit_ = nullptr;
    QLineEdit *primaryGatewayEdit_ = nullptr;
    QTextEdit *preferredGatewaysEdit_ = nullptr;
    QLabel *currentVersionLabel_ = nullptr;
    QLabel *updateStatusLabel_ = nullptr;
    QLabel *latestReleaseLabel_ = nullptr;
    QLabel *lastCheckedLabel_ = nullptr;
    QLabel *settingsDatabasePathLabel_ = nullptr;
    QLabel *secretStorePathLabel_ = nullptr;
    QSpinBox *messageLimitSpin_ = nullptr;
    QSpinBox *retryCooldownSpin_ = nullptr;
    QSpinBox *retryLimitSpin_ = nullptr;
    QSpinBox *downloadWorkersSpin_ = nullptr;
    QSpinBox *bandwidthSpin_ = nullptr;
    QSpinBox *previewWorkersSpin_ = nullptr;
    QCheckBox *autostartCheck_ = nullptr;
    QCheckBox *minimizeToTrayCheck_ = nullptr;
    QCheckBox *startHiddenCheck_ = nullptr;
    QCheckBox *darkModeCheck_ = nullptr;
    QCheckBox *archiveScanEnabledCheck_ = nullptr;
    QCheckBox *archiveHighPriorityCheck_ = nullptr;
    QCheckBox *selfHealCheck_ = nullptr;
    QCheckBox *flatFolderLayoutCheck_ = nullptr;
    QCheckBox *autoJoinSpacesCheck_ = nullptr;
    QCheckBox *autoDownloadCheck_ = nullptr;
    QPushButton *checkUpdatesButton_ = nullptr;
    QPushButton *openLatestReleaseButton_ = nullptr;

    QLabel *verificationStatusLabel_ = nullptr;
    QLabel *verificationDeviceIdLabel_ = nullptr;
    QLabel *verificationMessageLabel_ = nullptr;
    QListWidget *verificationEmojiList_ = nullptr;
    QLabel *verificationDecimalsLabel_ = nullptr;
    QPushButton *requestVerificationButton_ = nullptr;
    QPushButton *startVerificationButton_ = nullptr;
    QPushButton *approveVerificationButton_ = nullptr;
    QPushButton *declineVerificationButton_ = nullptr;

    QDialog *viewerDialog_ = nullptr;
    QLabel *viewerTitleLabel_ = nullptr;
    QLabel *viewerStatusLabel_ = nullptr;
    QProgressBar *viewerProgressBar_ = nullptr;
    QStackedWidget *viewerContentStack_ = nullptr;
    QScrollArea *viewerImageScrollArea_ = nullptr;
    QLabel *viewerImageLabel_ = nullptr;
    VideoFrameWidget *viewerVideoWidget_ = nullptr;
    VlcPlayerWidget *viewerVlcWidget_ = nullptr;
    WebVideoWidget *viewerWebVideoWidget_ = nullptr;
    QLabel *viewerFallbackLabel_ = nullptr;
    QMediaPlayer *viewerMediaPlayer_ = nullptr;
    QAudioOutput *viewerAudioOutput_ = nullptr;
    quint64 viewerLoadedSessionId_ = 0;
    quint64 viewerDismissedSessionId_ = 0;
    QString viewerLoadedLocalPath_;
    ViewerState viewerLoadedState_ = ViewerState::Idle;
    ViewerSnapshot localViewerSnapshot_;
    bool localViewerActive_ = false;
    quint64 nextLocalViewerSessionId_ = 1;

    bool settingsPageInitialized_ = false;
    bool settingsDirty_ = false;
    bool populatingSettingsUi_ = false;
    bool constrainingWindowGeometry_ = false;
    QString browserSelectedRoomId_;
    QString roomsPageSelectedRoomId_;
    QString browserLoadedRoomId_;
    int browserLoadedOffset_ = 0;
    QVector<AttachmentDiscovery> browserLoadedDiscoveries_;
    int browserTotalDiscoveryCount_ = 0;
    bool browserLoadingPage_ = false;
    bool browserBackgroundThumbnailPrefetchScheduled_ = false;
    QHash<QString, QIcon> browserThumbnailIconCache_;
    QStringList browserThumbnailCacheOrder_;
    QHash<QString, AttachmentDiscovery> browserThumbnailForegroundPending_;
    QStringList browserThumbnailForegroundOrder_;
    QHash<QString, AttachmentDiscovery> browserThumbnailBackgroundPending_;
    QStringList browserThumbnailBackgroundOrder_;
    QSet<QString> browserThumbnailRequestsInFlight_;
    QSet<QString> browserThumbnailForegroundRequestsInFlight_;
    QHash<QString, QNetworkReply *> browserThumbnailReplies_;
    QVector<ActivityLogEntry> loadedLogEntries_;
    int logsLoadedOffset_ = 0;
    int logTotalCount_ = 0;
    bool logsLoadingPage_ = false;
    bool logsProblemsOnly_ = false;
    bool updatePromptVisible_ = false;
    QString sharedItemsSignature_;
    QHash<QString, QIcon> sharedItemIconCache_;
};
