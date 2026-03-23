#include "MainWindow.h"

#include "AppController.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScreen>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

class QMoveEvent;

namespace {

QString jobTitle(const DownloadJobRecord &job)
{
    if (!job.originalFilename.isEmpty()) {
        return job.originalFilename;
    }
    if (!job.savedRelativePath.isEmpty()) {
        return job.savedRelativePath;
    }
    return job.eventId;
}

QString uploadLimitText(const qint64 bytes)
{
    if (bytes <= 0) {
        return QStringLiteral("Unknown");
    }
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MiB").arg(QString::number(mib, 'f', 1));
}

bool isSavedState(const DownloadJobState state)
{
    return state == DownloadJobState::Completed || state == DownloadJobState::DuplicateCompleted;
}

QString displayDateTime(const QDateTime &timestamp)
{
    if (!timestamp.isValid()) {
        return QStringLiteral("Never");
    }
    return QLocale().toString(timestamp.toLocalTime(), QLocale::ShortFormat);
}

}

MainWindow::MainWindow(AppController *controller, QWidget *parent)
    : QMainWindow(parent)
    , controller_(controller)
{
    setWindowTitle(QStringLiteral("Matrix Media Share Client"));
    resize(1380, 860);
    setAcceptDrops(true);

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);

    sectionList_ = new QListWidget(central);
    sectionList_->setFixedWidth(220);
    stack_ = new QStackedWidget(central);

    populateSectionSidebar();
    stack_->addWidget(buildRoomsPage());
    stack_->addWidget(buildBrowserPage());
    stack_->addWidget(buildLibraryPage());
    stack_->addWidget(buildTransfersPage());
    stack_->addWidget(buildSettingsPage());
    stack_->addWidget(buildVerificationPage());

    layout->addWidget(sectionList_);
    layout->addWidget(stack_, 1);
    setCentralWidget(central);

    connect(sectionList_, &QListWidget::currentRowChanged, stack_, &QStackedWidget::setCurrentIndex);
    connect(controller_, &AppController::stateChanged, this, &MainWindow::refreshView);

    constrainToAvailableGeometry();
    sectionList_->setCurrentRow(0);
    refreshView();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    constrainToAvailableGeometry();
}

void MainWindow::moveEvent(QMoveEvent *event)
{
    QMainWindow::moveEvent(event);
    constrainToAvailableGeometry();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    constrainToAvailableGeometry();
}

void MainWindow::constrainToAvailableGeometry()
{
    if (constrainingWindowGeometry_) {
        return;
    }

    QScreen *targetScreen = nullptr;
    if (windowHandle() != nullptr) {
        targetScreen = windowHandle()->screen();
    }
    if (targetScreen == nullptr) {
        targetScreen = QGuiApplication::screenAt(frameGeometry().center());
    }
    if (targetScreen == nullptr) {
        targetScreen = QGuiApplication::primaryScreen();
    }
    if (targetScreen == nullptr) {
        return;
    }

    const QRect availableFrame = targetScreen->availableGeometry();
    const QRect currentFrame = frameGeometry();
    const QRect currentClient = geometry();
    const int leftFrame = qMax(0, currentClient.left() - currentFrame.left());
    const int topFrame = qMax(0, currentClient.top() - currentFrame.top());
    const int rightFrame = qMax(0, currentFrame.right() - currentClient.right());
    const int bottomFrame = qMax(0, currentFrame.bottom() - currentClient.bottom());

    QRect availableClient = availableFrame.adjusted(
        leftFrame,
        topFrame,
        -rightFrame,
        -bottomFrame);
    if (availableClient.width() < 320 || availableClient.height() < 240) {
        availableClient = availableFrame;
    }

    const QSize maxClientSize = availableClient.size();
    setMaximumSize(maxClientSize);

    QRect clampedClient = currentClient;
    clampedClient.setWidth(qBound(320, clampedClient.width(), maxClientSize.width()));
    clampedClient.setHeight(qBound(240, clampedClient.height(), maxClientSize.height()));

    if (clampedClient.left() < availableClient.left()) {
        clampedClient.moveLeft(availableClient.left());
    }
    if (clampedClient.top() < availableClient.top()) {
        clampedClient.moveTop(availableClient.top());
    }
    if (clampedClient.right() > availableClient.right()) {
        clampedClient.moveRight(availableClient.right());
    }
    if (clampedClient.bottom() > availableClient.bottom()) {
        clampedClient.moveBottom(availableClient.bottom());
    }

    if (clampedClient != currentClient) {
        constrainingWindowGeometry_ = true;
        setGeometry(clampedClient);
        constrainingWindowGeometry_ = false;
    }
}

void MainWindow::refreshView()
{
    populateRoomsPage();
    populateBrowserPage();
    populateLibraryPage();
    populateTransfersPage();
    populateSettingsPage();
    populateVerificationPage();

    if (!controller_->lastErrorMessage().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Matrix Media Share Client"), controller_->lastErrorMessage());
        controller_->dismissError();
    }
}

void MainWindow::populateSectionSidebar()
{
    sectionList_->clear();
    for (const AppSection section : allSections()) {
        sectionList_->addItem(sectionTitle(section));
    }
}

QWidget *MainWindow::buildRoomsPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *joinRow = new QHBoxLayout();
    joinRoomEdit_ = new QLineEdit(page);
    joinRoomEdit_->setPlaceholderText(QStringLiteral("!room:server, #alias:server, or !space:server"));
    auto *joinButton = new QPushButton(QStringLiteral("Join"), page);
    joinRow->addWidget(joinRoomEdit_);
    joinRow->addWidget(joinButton);
    layout->addLayout(joinRow);

    roomsList_ = new QListWidget(page);
    roomsList_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(roomsList_, 1);

    roomDetailLabel_ = new QLabel(page);
    roomDetailLabel_->setWordWrap(true);
    leaveRoomButton_ = new QPushButton(QStringLiteral("Leave Selected"), page);
    layout->addWidget(roomDetailLabel_);
    layout->addWidget(leaveRoomButton_);

    connect(joinButton, &QPushButton::clicked, this, [this]() {
        controller_->joinRoom(joinRoomEdit_->text());
    });
    connect(leaveRoomButton_, &QPushButton::clicked, this, [this]() {
        const QListWidgetItem *item = roomsList_->currentItem();
        if (item != nullptr) {
            controller_->leaveRoom(item->data(Qt::UserRole).toString());
        }
    });
    connect(roomsList_, &QListWidget::currentRowChanged, this, [this]() {
        populateRoomsPage();
    });

    return page;
}

QWidget *MainWindow::buildBrowserPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *statusRow = new QHBoxLayout();
    powerToggle_ = new QCheckBox(QStringLiteral("Power"), page);
    connectionLabel_ = new QLabel(page);
    ipfsStatusLabel_ = new QLabel(page);
    uploadLimitLabel_ = new QLabel(page);
    updateBannerLabel_ = new QLabel(page);
    updateBannerLabel_->setWordWrap(true);
    statusRow->addWidget(powerToggle_);
    statusRow->addWidget(connectionLabel_);
    statusRow->addWidget(ipfsStatusLabel_);
    statusRow->addWidget(uploadLimitLabel_);
    statusRow->addWidget(updateBannerLabel_);
    statusRow->addStretch();
    layout->addLayout(statusRow);

    auto *shareRow = new QHBoxLayout();
    shareRoomCombo_ = new QComboBox(page);
    auto *shareButton = new QPushButton(QStringLiteral("Upload Files"), page);
    auto *importButton = new QPushButton(QStringLiteral("Import IPFS Link"), page);
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"), page);
    shareRow->addWidget(new QLabel(QStringLiteral("Room"), page));
    shareRow->addWidget(shareRoomCombo_, 1);
    shareRow->addWidget(shareButton);
    shareRow->addWidget(importButton);
    shareRow->addWidget(refreshButton);
    layout->addLayout(shareRow);

    browserDropHintLabel_ = new QLabel(
        QStringLiteral("Drop one or more files anywhere on this window while Browser is open to queue uploads into the selected room."),
        page);
    browserDropHintLabel_->setWordWrap(true);
    layout->addWidget(browserDropHintLabel_);

    discoveriesList_ = new QListWidget(page);
    discoveriesList_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(discoveriesList_, 1);

    auto *browserButtonRow = new QHBoxLayout();
    openDiscoveryButton_ = new QPushButton(QStringLiteral("Open Selected"), page);
    downloadDiscoveryButton_ = new QPushButton(QStringLiteral("Download Selected"), page);
    browserButtonRow->addWidget(openDiscoveryButton_);
    browserButtonRow->addWidget(downloadDiscoveryButton_);
    browserButtonRow->addStretch();
    layout->addLayout(browserButtonRow);

    connect(powerToggle_, &QCheckBox::toggled, controller_, &AppController::togglePower);
    connect(refreshButton, &QPushButton::clicked, controller_, &AppController::refreshCatalog);
    connect(shareRoomCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        populateBrowserPage();
    });
    connect(shareButton, &QPushButton::clicked, this, [this]() {
        const QString roomId = shareRoomCombo_->currentData().toString();
        const QStringList filePaths = QFileDialog::getOpenFileNames(this, QStringLiteral("Choose Files To Share"));
        if (!roomId.isEmpty() && !filePaths.isEmpty()) {
            controller_->shareLocalFiles(roomId, filePaths);
        }
    });
    connect(importButton, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString link = QInputDialog::getText(
            this,
            QStringLiteral("Import IPFS Link"),
            QStringLiteral("Paste an IPFS URL or CID"),
            QLineEdit::Normal,
            QString(),
            &ok);
        if (ok && !link.trimmed().isEmpty()) {
            controller_->importIpfsLink(link.trimmed());
        }
    });
    connect(downloadDiscoveryButton_, &QPushButton::clicked, this, [this]() {
        const QListWidgetItem *item = discoveriesList_->currentItem();
        if (item != nullptr) {
            controller_->queueDiscoveryDownload(
                item->data(Qt::UserRole).toString(),
                item->data(Qt::UserRole + 1).toString());
        }
    });
    connect(openDiscoveryButton_, &QPushButton::clicked, this, [this]() {
        const QListWidgetItem *item = discoveriesList_->currentItem();
        if (item != nullptr) {
            controller_->openDiscovery(
                item->data(Qt::UserRole).toString(),
                item->data(Qt::UserRole + 1).toString());
        }
    });
    connect(discoveriesList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item != nullptr) {
            controller_->openDiscovery(
                item->data(Qt::UserRole).toString(),
                item->data(Qt::UserRole + 1).toString());
        }
    });
    connect(discoveriesList_, &QListWidget::currentRowChanged, this, [this]() {
        openDiscoveryButton_->setEnabled(discoveriesList_->currentItem() != nullptr);
        downloadDiscoveryButton_->setEnabled(discoveriesList_->currentItem() != nullptr);
    });

    return page;
}

QWidget *MainWindow::buildLibraryPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    libraryList_ = new QListWidget(page);
    openLibraryButton_ = new QPushButton(QStringLiteral("Open Selected File"), page);
    layout->addWidget(libraryList_, 1);
    layout->addWidget(openLibraryButton_);

    connect(openLibraryButton_, &QPushButton::clicked, this, [this]() {
        const QListWidgetItem *item = libraryList_->currentItem();
        if (item == nullptr) {
            return;
        }
        const QString relativePath = item->data(Qt::UserRole).toString();
        if (relativePath.isEmpty()) {
            return;
        }
        const QString fullPath = QDir::isAbsolutePath(relativePath)
            ? relativePath
            : controller_->settings().destinationRootPath + QStringLiteral("/") + relativePath;
        QDesktopServices::openUrl(QUrl::fromLocalFile(fullPath));
    });
    connect(libraryList_, &QListWidget::currentRowChanged, this, [this]() {
        openLibraryButton_->setEnabled(libraryList_->currentItem() != nullptr);
    });

    return page;
}

QWidget *MainWindow::buildTransfersPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    queueStatsLabel_ = new QLabel(page);
    layout->addWidget(queueStatsLabel_);

    activeDownloadsList_ = new QListWidget(page);
    layout->addWidget(activeDownloadsList_);

    waitingJobsTable_ = new QTableWidget(page);
    waitingJobsTable_->setColumnCount(4);
    waitingJobsTable_->setHorizontalHeaderLabels({QStringLiteral("File"), QStringLiteral("Room"), QStringLiteral("State"), QStringLiteral("Error")});
    waitingJobsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(waitingJobsTable_, 1);

    failedJobsTable_ = new QTableWidget(page);
    failedJobsTable_->setColumnCount(4);
    failedJobsTable_->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("File"), QStringLiteral("Room"), QStringLiteral("Error")});
    failedJobsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(failedJobsTable_, 1);

    return page;
}

QWidget *MainWindow::buildSettingsPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *scrollArea = new QScrollArea(page);
    scrollArea->setWidgetResizable(true);
    auto *content = new QWidget(scrollArea);
    auto *contentLayout = new QVBoxLayout(content);
    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    homeserverEdit_ = new QLineEdit(content);
    usernameEdit_ = new QLineEdit(content);
    passwordEdit_ = new QLineEdit(content);
    passwordEdit_->setEchoMode(QLineEdit::Password);
    primaryGatewayEdit_ = new QLineEdit(content);
    preferredGatewaysEdit_ = new QTextEdit(content);
    currentVersionLabel_ = new QLabel(content);
    updateStatusLabel_ = new QLabel(content);
    latestReleaseLabel_ = new QLabel(content);
    lastCheckedLabel_ = new QLabel(content);
    checkUpdatesButton_ = new QPushButton(QStringLiteral("Check For Updates"), content);
    openLatestReleaseButton_ = new QPushButton(QStringLiteral("Open Latest Release"), content);
    preferredGatewaysEdit_->setFixedHeight(90);
    messageLimitSpin_ = new QSpinBox(content);
    messageLimitSpin_->setMaximum(1'000'000);
    retryCooldownSpin_ = new QSpinBox(content);
    retryCooldownSpin_->setMaximum(10'000);
    retryLimitSpin_ = new QSpinBox(content);
    retryLimitSpin_->setMaximum(1000);
    downloadWorkersSpin_ = new QSpinBox(content);
    downloadWorkersSpin_->setRange(1, 6);
    bandwidthSpin_ = new QSpinBox(content);
    bandwidthSpin_->setMaximum(10'000'000);
    previewWorkersSpin_ = new QSpinBox(content);
    previewWorkersSpin_->setRange(1, 4);
    autostartCheck_ = new QCheckBox(content);
    minimizeToTrayCheck_ = new QCheckBox(content);
    startHiddenCheck_ = new QCheckBox(content);
    archiveScanEnabledCheck_ = new QCheckBox(content);
    archiveHighPriorityCheck_ = new QCheckBox(content);
    autoJoinSpacesCheck_ = new QCheckBox(content);
    autoDownloadCheck_ = new QCheckBox(content);

    auto configureWideLineEdit = [](QLineEdit *edit) {
        edit->setClearButtonEnabled(true);
        edit->setMinimumWidth(540);
        edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    };

    configureWideLineEdit(homeserverEdit_);
    configureWideLineEdit(usernameEdit_);
    configureWideLineEdit(passwordEdit_);
    configureWideLineEdit(primaryGatewayEdit_);
    preferredGatewaysEdit_->setMinimumWidth(540);
    for (QLabel *label : {currentVersionLabel_, updateStatusLabel_, latestReleaseLabel_, lastCheckedLabel_}) {
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }

    auto addPathPickerRow = [this, content, form, &configureWideLineEdit](
                                const QString &label,
                                QLineEdit *&edit,
                                const QString &dialogTitle) {
        auto *rowWidget = new QWidget(content);
        auto *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        edit = new QLineEdit(rowWidget);
        configureWideLineEdit(edit);
        auto *browseButton = new QPushButton(QStringLiteral("Browse..."), rowWidget);
        rowLayout->addWidget(edit, 1);
        rowLayout->addWidget(browseButton);
        form->addRow(label, rowWidget);

        connect(browseButton, &QPushButton::clicked, this, [this, edit, dialogTitle]() {
            QString startPath = edit->text().trimmed();
            if (startPath.isEmpty()) {
                startPath = QDir::homePath();
            }
            const QString selectedPath = QFileDialog::getExistingDirectory(
                this,
                dialogTitle,
                startPath,
                QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
            if (!selectedPath.isEmpty()) {
                edit->setText(QDir::toNativeSeparators(selectedPath));
            }
        });
    };

    addPathPickerRow(QStringLiteral("Destination Root"), destinationEdit_, QStringLiteral("Choose Destination Root"));
    addPathPickerRow(QStringLiteral("Shared Files Root"), libraryEdit_, QStringLiteral("Choose Shared Files Root"));
    addPathPickerRow(QStringLiteral("Archive Root"), archiveEdit_, QStringLiteral("Choose Archive Root"));
    addPathPickerRow(QStringLiteral("Downloads Root"), manualDownloadsEdit_, QStringLiteral("Choose Downloads Root"));

    form->addRow(QStringLiteral("Homeserver"), homeserverEdit_);
    form->addRow(QStringLiteral("Username"), usernameEdit_);
    form->addRow(QStringLiteral("Password"), passwordEdit_);
    form->addRow(QStringLiteral("Current Version"), currentVersionLabel_);
    form->addRow(QStringLiteral("Update Status"), updateStatusLabel_);
    form->addRow(QStringLiteral("Latest Release"), latestReleaseLabel_);
    form->addRow(QStringLiteral("Last Checked"), lastCheckedLabel_);
    form->addRow(QStringLiteral("Archive Scan Enabled"), archiveScanEnabledCheck_);
    form->addRow(QStringLiteral("Archive Scan High Priority"), archiveHighPriorityCheck_);
    form->addRow(QStringLiteral("Primary Gateway"), primaryGatewayEdit_);
    form->addRow(QStringLiteral("Preferred Gateways"), preferredGatewaysEdit_);
    form->addRow(QStringLiteral("Message Limit"), messageLimitSpin_);
    form->addRow(QStringLiteral("Retry Cooldown (min)"), retryCooldownSpin_);
    form->addRow(QStringLiteral("Retry Limit"), retryLimitSpin_);
    form->addRow(QStringLiteral("Download Workers"), downloadWorkersSpin_);
    form->addRow(QStringLiteral("Bandwidth Limit (KiB/s)"), bandwidthSpin_);
    form->addRow(QStringLiteral("Preview Workers"), previewWorkersSpin_);
    form->addRow(QStringLiteral("Autostart"), autostartCheck_);
    form->addRow(QStringLiteral("Minimize To Tray"), minimizeToTrayCheck_);
    form->addRow(QStringLiteral("Start Hidden"), startHiddenCheck_);
    form->addRow(QStringLiteral("Auto Join Space Rooms"), autoJoinSpacesCheck_);
    form->addRow(QStringLiteral("Auto Download New Media"), autoDownloadCheck_);
    contentLayout->addLayout(form);

    auto *buttonRow = new QHBoxLayout();
    auto *saveButton = new QPushButton(QStringLiteral("Save Settings"), content);
    auto *resetButton = new QPushButton(QStringLiteral("Reset History Scans"), content);
    buttonRow->addWidget(saveButton);
    buttonRow->addWidget(resetButton);
    buttonRow->addWidget(checkUpdatesButton_);
    buttonRow->addWidget(openLatestReleaseButton_);
    buttonRow->addStretch();
    contentLayout->addLayout(buttonRow);
    contentLayout->addStretch();

    scrollArea->setWidget(content);
    layout->addWidget(scrollArea);

    connect(saveButton, &QPushButton::clicked, this, [this]() {
        controller_->saveSettings(gatherSettingsFromUi(), passwordEdit_->text());
    });
    connect(resetButton, &QPushButton::clicked, controller_, &AppController::resetHistoryScans);
    connect(checkUpdatesButton_, &QPushButton::clicked, this, [this]() {
        controller_->checkForUpdates(true);
    });
    connect(openLatestReleaseButton_, &QPushButton::clicked, controller_, &AppController::openLatestReleasePage);

    return page;
}

QWidget *MainWindow::buildVerificationPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    verificationStatusLabel_ = new QLabel(page);
    verificationDeviceIdLabel_ = new QLabel(page);
    verificationEmojiList_ = new QListWidget(page);
    verificationDecimalsLabel_ = new QLabel(page);

    auto *buttonRow = new QHBoxLayout();
    auto *requestButton = new QPushButton(QStringLiteral("Request"), page);
    auto *startButton = new QPushButton(QStringLiteral("Start SAS"), page);
    auto *approveButton = new QPushButton(QStringLiteral("Approve"), page);
    auto *declineButton = new QPushButton(QStringLiteral("Reject"), page);
    buttonRow->addWidget(requestButton);
    buttonRow->addWidget(startButton);
    buttonRow->addWidget(approveButton);
    buttonRow->addWidget(declineButton);
    buttonRow->addStretch();

    layout->addWidget(verificationStatusLabel_);
    layout->addWidget(verificationDeviceIdLabel_);
    layout->addLayout(buttonRow);
    layout->addWidget(verificationEmojiList_, 1);
    layout->addWidget(verificationDecimalsLabel_);

    connect(requestButton, &QPushButton::clicked, controller_, &AppController::requestVerification);
    connect(startButton, &QPushButton::clicked, controller_, &AppController::startSasVerification);
    connect(approveButton, &QPushButton::clicked, controller_, &AppController::approveVerification);
    connect(declineButton, &QPushButton::clicked, controller_, &AppController::declineVerification);

    return page;
}

void MainWindow::populateRoomsPage()
{
    const QString selectedRoomId = roomsList_->currentItem() != nullptr
        ? roomsList_->currentItem()->data(Qt::UserRole).toString()
        : QString();

    roomsList_->clear();
    for (const RoomRecord &room : controller_->rooms()) {
        auto *item = new QListWidgetItem(roomDisplayTitle(room), roomsList_);
        item->setData(Qt::UserRole, room.roomId);
        if (room.isSpace) {
            item->setText(item->text() + QStringLiteral(" [Space]"));
        }
        if (room.roomId == selectedRoomId) {
            roomsList_->setCurrentItem(item);
        }
    }

    const QListWidgetItem *currentItem = roomsList_->currentItem();
    if (currentItem == nullptr) {
        roomDetailLabel_->setText(QStringLiteral("Join a room or space to begin browsing shared media."));
        leaveRoomButton_->setEnabled(false);
        return;
    }

    leaveRoomButton_->setEnabled(true);
    const QString roomId = currentItem->data(Qt::UserRole).toString();
    const auto rooms = controller_->rooms();
    for (const RoomRecord &room : rooms) {
        if (room.roomId == roomId) {
            roomDetailLabel_->setText(QStringLiteral("ID: %1\nAlias: %2\nFolder: %3\nMembership: %4")
                                          .arg(room.roomId, room.currentCanonicalAlias, room.activeFolderLabel, room.membership));
            break;
        }
    }
}

void MainWindow::populateBrowserPage()
{
    const BotRuntimeSnapshot &runtime = controller_->runtime();
    powerToggle_->blockSignals(true);
    powerToggle_->setChecked(controller_->settings().desiredPowerState);
    powerToggle_->blockSignals(false);
    connectionLabel_->setText(QStringLiteral("Matrix: %1").arg(controller_->connectionStatusText()));
    ipfsStatusLabel_->setText(QStringLiteral("IPFS: %1").arg(ipfsRuntimeStateTitle(runtime.ipfs.state)));
    uploadLimitLabel_->setText(QStringLiteral("Upload Limit: %1").arg(uploadLimitText(runtime.uploadSizeLimitBytes)));
    if (controller_->updateAvailable() || controller_->isUpdateCheckInProgress()) {
        updateBannerLabel_->setText(QStringLiteral("Updates: %1").arg(controller_->updateStatusText()));
    } else {
        updateBannerLabel_->clear();
    }

    const QString previousRoomId = shareRoomCombo_->currentData().toString();
    shareRoomCombo_->blockSignals(true);
    shareRoomCombo_->clear();
    for (const RoomRecord &room : controller_->joinedRooms()) {
        shareRoomCombo_->addItem(roomDisplayTitle(room), room.roomId);
    }
    const int restoredIndex = previousRoomId.isEmpty() ? 0 : shareRoomCombo_->findData(previousRoomId);
    if (restoredIndex >= 0) {
        shareRoomCombo_->setCurrentIndex(restoredIndex);
    }
    shareRoomCombo_->blockSignals(false);
    const QString selectedRoomId = shareRoomCombo_->currentData().toString();

    discoveriesList_->clear();
    for (const AttachmentDiscovery &discovery : controller_->discoveries()) {
        if (!selectedRoomId.isEmpty() && discovery.roomId != selectedRoomId) {
            continue;
        }
        QString title = discovery.originalFilename.isEmpty() ? discovery.eventId : discovery.originalFilename;
        title += QStringLiteral("  [%1 | %2]")
                     .arg(mediaCategoryTitle(discovery.category), mediaSourceKindTitle(discovery.sourceKind));
        auto *item = new QListWidgetItem(title, discoveriesList_);
        const QString sourceUrl = discovery.directUrl.isEmpty() ? discovery.mxcUrl : discovery.directUrl;
        item->setToolTip(QStringLiteral("%1\n%2").arg(discovery.roomId, sourceUrl));
        item->setData(Qt::UserRole, discovery.roomId);
        item->setData(Qt::UserRole + 1, discovery.eventId);
    }
    openDiscoveryButton_->setEnabled(discoveriesList_->currentItem() != nullptr);
    downloadDiscoveryButton_->setEnabled(discoveriesList_->currentItem() != nullptr);
}

void MainWindow::populateLibraryPage()
{
    libraryList_->clear();
    for (const DownloadJobRecord &job : controller_->jobs()) {
        if (!isSavedState(job.state) || job.savedRelativePath.isEmpty()) {
            continue;
        }
        auto *item = new QListWidgetItem(jobTitle(job), libraryList_);
        item->setData(Qt::UserRole, job.savedRelativePath);
        item->setToolTip(job.savedRelativePath);
    }
    openLibraryButton_->setEnabled(libraryList_->currentItem() != nullptr);
}

void MainWindow::populateTransfersPage()
{
    queueStatsLabel_->setText(
        QStringLiteral("Waiting: %1   Active: %2")
            .arg(controller_->waitingQueueCount())
            .arg(controller_->runtime().activeDownloads.size()));

    activeDownloadsList_->clear();
    for (const ActiveDownloadSnapshot &download : controller_->runtime().activeDownloads) {
        activeDownloadsList_->addItem(QStringLiteral("Worker %1: %2 (%3 bytes)")
                                          .arg(download.workerId)
                                          .arg(download.filename)
                                          .arg(download.receivedBytes));
    }

    QVector<DownloadJobRecord> waitingJobs;
    QVector<DownloadJobRecord> failedJobs;
    for (const DownloadJobRecord &job : controller_->jobs()) {
        if (job.state == DownloadJobState::FailedPermanent) {
            failedJobs.append(job);
        } else if (!isSavedState(job.state)) {
            waitingJobs.append(job);
        }
    }

    waitingJobsTable_->setRowCount(waitingJobs.size());
    for (int row = 0; row < waitingJobs.size(); ++row) {
        const DownloadJobRecord &job = waitingJobs.at(row);
        waitingJobsTable_->setItem(row, 0, new QTableWidgetItem(jobTitle(job)));
        waitingJobsTable_->setItem(row, 1, new QTableWidgetItem(job.roomId));
        waitingJobsTable_->setItem(row, 2, new QTableWidgetItem(downloadJobStateTitle(job.state)));
        waitingJobsTable_->setItem(row, 3, new QTableWidgetItem(job.lastError));
    }

    failedJobsTable_->setRowCount(failedJobs.size());
    for (int row = 0; row < failedJobs.size(); ++row) {
        const DownloadJobRecord &job = failedJobs.at(row);
        failedJobsTable_->setItem(row, 0, new QTableWidgetItem(QString::number(job.id)));
        failedJobsTable_->setItem(row, 1, new QTableWidgetItem(jobTitle(job)));
        failedJobsTable_->setItem(row, 2, new QTableWidgetItem(job.roomId));
        failedJobsTable_->setItem(row, 3, new QTableWidgetItem(job.lastError));
    }
}

void MainWindow::populateSettingsPage()
{
    const AppSettings &settings = controller_->settings();
    homeserverEdit_->setText(settings.homeserverUrl);
    usernameEdit_->setText(settings.username);
    passwordEdit_->setText(controller_->password());
    destinationEdit_->setText(settings.destinationRootPath);
    libraryEdit_->setText(settings.libraryRootPath);
    archiveEdit_->setText(settings.archiveRootPath);
    archiveScanEnabledCheck_->setChecked(settings.archiveScanEnabled);
    archiveHighPriorityCheck_->setChecked(settings.archiveScanHighPriority);
    manualDownloadsEdit_->setText(settings.manualDownloadRootPath);
    primaryGatewayEdit_->setText(settings.primaryGatewayUrl);
    preferredGatewaysEdit_->setPlainText(settings.preferredGatewayUrls.join(QStringLiteral("\n")));
    messageLimitSpin_->setValue(settings.messageLimit);
    retryCooldownSpin_->setValue(settings.retryCooldownMinutes);
    retryLimitSpin_->setValue(settings.retryLimit);
    downloadWorkersSpin_->setValue(settings.downloadWorkerCount);
    bandwidthSpin_->setValue(settings.bandwidthLimitKiBPerSec);
    previewWorkersSpin_->setValue(settings.previewWorkerCount);
    autostartCheck_->setChecked(settings.autostartEnabled);
    minimizeToTrayCheck_->setChecked(settings.minimizeToTray);
    startHiddenCheck_->setChecked(settings.startHidden);
    autoJoinSpacesCheck_->setChecked(settings.autoJoinSpaceRooms);
    autoDownloadCheck_->setChecked(settings.autoDownloadNewMedia);
    currentVersionLabel_->setText(controller_->currentVersion());
    updateStatusLabel_->setText(controller_->updateStatusText());
    latestReleaseLabel_->setText(controller_->latestReleaseSummaryText());
    lastCheckedLabel_->setText(displayDateTime(controller_->updateCheckState().lastCheckedAt));
    checkUpdatesButton_->setEnabled(!controller_->isUpdateCheckInProgress());
    openLatestReleaseButton_->setEnabled(!controller_->latestReleasePageUrl().trimmed().isEmpty());
    settingsPageInitialized_ = true;
}

void MainWindow::populateVerificationPage()
{
    const VerificationSnapshot &verification = controller_->runtime().verification;
    verificationStatusLabel_->setText(QStringLiteral("Status: %1").arg(verificationStatusTitle(verification.state)));
    verificationDeviceIdLabel_->setText(QStringLiteral("Device: %1").arg(verification.deviceId));
    verificationEmojiList_->clear();
    for (const VerificationEmoji &emoji : verification.emojis) {
        verificationEmojiList_->addItem(QStringLiteral("%1  %2").arg(emoji.symbol, emoji.description));
    }

    QStringList decimals;
    for (const quint16 value : verification.decimals) {
        decimals.append(QString::number(value));
    }
    verificationDecimalsLabel_->setText(QStringLiteral("Decimals: %1").arg(decimals.join(QStringLiteral(", "))));
}

AppSettings MainWindow::gatherSettingsFromUi() const
{
    AppSettings settings = controller_->settings();
    settings.homeserverUrl = homeserverEdit_->text().trimmed();
    settings.username = usernameEdit_->text().trimmed();
    settings.ownerUserId.clear();
    settings.destinationRootPath = destinationEdit_->text().trimmed();
    settings.libraryRootPath = libraryEdit_->text().trimmed();
    settings.archiveRootPath = archiveEdit_->text().trimmed();
    settings.archiveScanEnabled = archiveScanEnabledCheck_->isChecked();
    settings.archiveScanHighPriority = archiveHighPriorityCheck_->isChecked();
    settings.manualDownloadRootPath = manualDownloadsEdit_->text().trimmed();
    settings.primaryGatewayUrl = primaryGatewayEdit_->text().trimmed();
    settings.preferredGatewayUrls = preferredGatewaysEdit_->toPlainText().split('\n', Qt::SkipEmptyParts);
    settings.messageLimit = messageLimitSpin_->value();
    settings.retryCooldownMinutes = retryCooldownSpin_->value();
    settings.retryLimit = retryLimitSpin_->value();
    settings.downloadWorkerCount = downloadWorkersSpin_->value();
    settings.bandwidthLimitKiBPerSec = bandwidthSpin_->value();
    settings.previewWorkerCount = previewWorkersSpin_->value();
    settings.autostartEnabled = autostartCheck_->isChecked();
    settings.minimizeToTray = minimizeToTrayCheck_->isChecked();
    settings.startHidden = startHiddenCheck_->isChecked();
    settings.autoJoinSpaceRooms = autoJoinSpacesCheck_->isChecked();
    settings.autoDownloadNewMedia = autoDownloadCheck_->isChecked();
    return settings;
}

QString MainWindow::roomDisplayTitle(const RoomRecord &room) const
{
    if (!room.currentDisplayName.isEmpty()) {
        return room.currentDisplayName;
    }
    if (!room.currentCanonicalAlias.isEmpty()) {
        return room.currentCanonicalAlias;
    }
    return room.roomId;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event != nullptr
        && stack_ != nullptr
        && stack_->currentIndex() == 1
        && event->mimeData() != nullptr
        && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QMainWindow::dragEnterEvent(event);
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (event != nullptr
        && stack_ != nullptr
        && stack_->currentIndex() == 1
        && event->mimeData() != nullptr
        && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QMainWindow::dragMoveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (event == nullptr || stack_ == nullptr || stack_->currentIndex() != 1 || event->mimeData() == nullptr) {
        QMainWindow::dropEvent(event);
        return;
    }

    const QString roomId = shareRoomCombo_ != nullptr ? shareRoomCombo_->currentData().toString() : QString {};
    if (roomId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Matrix Media Share Client"), QStringLiteral("Pick a destination room before dropping files."));
        event->ignore();
        return;
    }

    QStringList filePaths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            const QString filePath = url.toLocalFile().trimmed();
            if (!filePath.isEmpty() && !filePaths.contains(filePath)) {
                filePaths.append(filePath);
            }
        }
    }

    if (filePaths.isEmpty()) {
        event->ignore();
        return;
    }

    controller_->shareLocalFiles(roomId, filePaths);
    event->acceptProposedAction();
}
