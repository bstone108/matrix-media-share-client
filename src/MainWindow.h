#pragma once

#include "Domain.h"

#include <QMainWindow>

class AppController;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QMoveEvent;
class QResizeEvent;
class QShowEvent;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSizePolicy;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTextEdit;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(AppController *controller, QWidget *parent = nullptr);

private:
    void showEvent(QShowEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    void constrainToAvailableGeometry();
    void refreshView();
    void populateSectionSidebar();
    void populateRoomsPage();
    void populateBrowserPage();
    void populateLibraryPage();
    void populateTransfersPage();
    void populateSettingsPage();
    void populateVerificationPage();

    QWidget *buildRoomsPage();
    QWidget *buildBrowserPage();
    QWidget *buildLibraryPage();
    QWidget *buildTransfersPage();
    QWidget *buildSettingsPage();
    QWidget *buildVerificationPage();

    AppSettings gatherSettingsFromUi() const;
    QString roomDisplayTitle(const RoomRecord &room) const;

    AppController *controller_ = nullptr;

    QListWidget *sectionList_ = nullptr;
    QStackedWidget *stack_ = nullptr;

    QListWidget *roomsList_ = nullptr;
    QLineEdit *joinRoomEdit_ = nullptr;
    QLabel *roomDetailLabel_ = nullptr;
    QPushButton *leaveRoomButton_ = nullptr;

    QCheckBox *powerToggle_ = nullptr;
    QLabel *connectionLabel_ = nullptr;
    QLabel *ipfsStatusLabel_ = nullptr;
    QLabel *uploadLimitLabel_ = nullptr;
    QLabel *updateBannerLabel_ = nullptr;
    QLabel *browserDropHintLabel_ = nullptr;
    QComboBox *shareRoomCombo_ = nullptr;
    QListWidget *discoveriesList_ = nullptr;
    QPushButton *openDiscoveryButton_ = nullptr;
    QPushButton *downloadDiscoveryButton_ = nullptr;

    QListWidget *libraryList_ = nullptr;
    QPushButton *openLibraryButton_ = nullptr;

    QLabel *queueStatsLabel_ = nullptr;
    QListWidget *activeDownloadsList_ = nullptr;
    QTableWidget *waitingJobsTable_ = nullptr;
    QTableWidget *failedJobsTable_ = nullptr;

    QLineEdit *homeserverEdit_ = nullptr;
    QLineEdit *usernameEdit_ = nullptr;
    QLineEdit *passwordEdit_ = nullptr;
    QLineEdit *destinationEdit_ = nullptr;
    QLineEdit *libraryEdit_ = nullptr;
    QLineEdit *archiveEdit_ = nullptr;
    QLineEdit *manualDownloadsEdit_ = nullptr;
    QLineEdit *primaryGatewayEdit_ = nullptr;
    QTextEdit *preferredGatewaysEdit_ = nullptr;
    QLabel *currentVersionLabel_ = nullptr;
    QLabel *updateStatusLabel_ = nullptr;
    QLabel *latestReleaseLabel_ = nullptr;
    QLabel *lastCheckedLabel_ = nullptr;
    QSpinBox *messageLimitSpin_ = nullptr;
    QSpinBox *retryCooldownSpin_ = nullptr;
    QSpinBox *retryLimitSpin_ = nullptr;
    QSpinBox *downloadWorkersSpin_ = nullptr;
    QSpinBox *bandwidthSpin_ = nullptr;
    QSpinBox *previewWorkersSpin_ = nullptr;
    QCheckBox *autostartCheck_ = nullptr;
    QCheckBox *minimizeToTrayCheck_ = nullptr;
    QCheckBox *startHiddenCheck_ = nullptr;
    QCheckBox *archiveScanEnabledCheck_ = nullptr;
    QCheckBox *archiveHighPriorityCheck_ = nullptr;
    QCheckBox *autoJoinSpacesCheck_ = nullptr;
    QCheckBox *autoDownloadCheck_ = nullptr;
    QPushButton *checkUpdatesButton_ = nullptr;
    QPushButton *openLatestReleaseButton_ = nullptr;

    QLabel *verificationStatusLabel_ = nullptr;
    QLabel *verificationDeviceIdLabel_ = nullptr;
    QListWidget *verificationEmojiList_ = nullptr;
    QLabel *verificationDecimalsLabel_ = nullptr;

    bool settingsPageInitialized_ = false;
    bool constrainingWindowGeometry_ = false;
};
