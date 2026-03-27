#include "MainWindow.h"

#include "AppController.h"
#include "VideoFrameWidget.h"
#include "VlcPlayerWidget.h"
#include "WebVideoWidget.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDesktopServices>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QLocale>
#include <QMimeData>
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QProgressBar>
#include <QResizeEvent>
#include <QScrollBar>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWindow>

class QMoveEvent;

namespace {

constexpr int kDiscoveryTileWidth = 236;
constexpr int kDiscoveryTileHeight = 188;
constexpr int kDiscoveryGridWidth = 248;
constexpr int kDiscoveryGridHeight = 236;
constexpr int kBrowserPageSize = 60;
constexpr int kBrowserBufferedPages = 2;
constexpr int kBrowserMaxWindowPages = 5;
constexpr int kBrowserThumbnailCacheLimit = 160;
constexpr int kBrowserBackgroundThumbnailBatchSize = 10;
constexpr int kBrowserThumbnailConcurrentRequests = 6;
constexpr int kLogsPageSize = 200;
constexpr int kLogsBufferedPages = 2;
constexpr int kLogsMaxWindowPages = 5;

struct ThemeBaseline {
    bool captured = false;
    QString styleName;
    QPalette palette;
};

ThemeBaseline &themeBaseline()
{
    static ThemeBaseline baseline;
    return baseline;
}

void captureThemeBaseline(QApplication *app)
{
    if (app == nullptr) {
        return;
    }
    ThemeBaseline &baseline = themeBaseline();
    if (baseline.captured) {
        return;
    }
    if (app->style() != nullptr) {
        baseline.styleName = app->style()->objectName();
    }
    baseline.palette = app->palette();
    baseline.captured = true;
}

QPalette darkPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#1b1f24")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#edf2f7")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#12161b")));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#222831")));
    palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#111827")));
    palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#edf2f7")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#edf2f7")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#2b3138")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#edf2f7")));
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#8c96a3")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#2563eb")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#60a5fa")));
    palette.setColor(QPalette::LinkVisited, QColor(QStringLiteral("#93c5fd")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#7d8590")));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#7d8590")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#7d8590")));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(QStringLiteral("#334155")));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(QStringLiteral("#cbd5e1")));
    return palette;
}

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

bool looksLikeIpfsUrl(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed.startsWith(QStringLiteral("ipfs://"), Qt::CaseInsensitive)
        || trimmed.contains(QStringLiteral("/ipfs/"), Qt::CaseInsensitive);
}

QString discoverySourceKindTitle(const AttachmentDiscovery &discovery)
{
    if (discovery.sourceKind == MediaSourceKind::Ipfs
        || looksLikeIpfsUrl(discovery.directUrl)
        || looksLikeIpfsUrl(discovery.mxcUrl)) {
        return QStringLiteral("ipfs");
    }
    return mediaSourceKindTitle(discovery.sourceKind);
}

bool shouldUseWebVideoFallback(const ViewerSnapshot &viewer)
{
#ifdef Q_OS_MACOS
    const QString mime = viewer.mimeType.trimmed().toLower();
    const QString path = viewer.localPath.trimmed().toLower();
    return mime == QStringLiteral("video/webm")
        || mime == QStringLiteral("application/webm")
        || path.endsWith(QStringLiteral(".webm"))
        || path.endsWith(QStringLiteral(".ogv"));
#else
    Q_UNUSED(viewer);
    return false;
#endif
}

QColor categoryAccent(const MediaCategory category)
{
    switch (category) {
    case MediaCategory::Images:
        return QColor(QStringLiteral("#0ea5e9"));
    case MediaCategory::Videos:
        return QColor(QStringLiteral("#f97316"));
    case MediaCategory::Audio:
        return QColor(QStringLiteral("#10b981"));
    case MediaCategory::Documents:
        return QColor(QStringLiteral("#6366f1"));
    case MediaCategory::Archives:
        return QColor(QStringLiteral("#eab308"));
    case MediaCategory::Programs:
        return QColor(QStringLiteral("#ef4444"));
    case MediaCategory::Other:
        break;
    }
    return QColor(QStringLiteral("#64748b"));
}

QString elidedTileText(const QString &value, const int maxLength)
{
    const QString trimmed = value.trimmed();
    if (trimmed.size() <= maxLength) {
        return trimmed;
    }
    return trimmed.left(maxLength - 1) + QChar(0x2026);
}

QImage loadAutoTransformedImageFromBytes(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }

    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    return reader.read();
}

QImage loadAutoTransformedImageFromFile(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    return reader.read();
}

QRect centeredAspectRect(const QSize &sourceSize, const QRect &bounds)
{
    if (!sourceSize.isValid() || !bounds.isValid()) {
        return bounds;
    }

    const QSize fitted = sourceSize.scaled(bounds.size(), Qt::KeepAspectRatio);
    QRect target(QPoint(0, 0), fitted);
    target.moveCenter(bounds.center());
    return target;
}

QString dataSizeText(const qint64 bytes)
{
    if (bytes < 0) {
        return QStringLiteral("Unknown");
    }
    return QLocale().formattedDataSize(bytes);
}

QString transferProgressText(const qint64 receivedBytes, const qint64 totalBytes)
{
    if (totalBytes > 0) {
        const double fraction = qBound(
            0.0,
            static_cast<double>(receivedBytes) / static_cast<double>(totalBytes),
            1.0);
        return QStringLiteral("%1 of %2 (%3%)")
            .arg(dataSizeText(receivedBytes),
                 dataSizeText(totalBytes),
                 QString::number(fraction * 100.0, 'f', 0));
    }
    if (receivedBytes > 0) {
        return dataSizeText(receivedBytes);
    }
    return QString();
}

int activePendingUploadCount(const QVector<PendingUploadSnapshot> &uploads)
{
    int count = 0;
    for (const PendingUploadSnapshot &upload : uploads) {
        const QString state = upload.state.trimmed().toLower();
        if (!state.isEmpty() && state != QStringLiteral("queued")) {
            count += 1;
        }
    }
    return count;
}

int previewGeneratingUploadCount(const QVector<PendingUploadSnapshot> &uploads)
{
    int count = 0;
    for (const PendingUploadSnapshot &upload : uploads) {
        if (upload.state == QStringLiteral("previewing")) {
            count += 1;
        }
    }
    return count;
}

QString browserUploadSummaryText(const QVector<PendingUploadSnapshot> &uploads)
{
    if (uploads.isEmpty()) {
        return QStringLiteral("Uploads: none");
    }
    return QStringLiteral("Uploads: %1").arg(uploads.size());
}

QString browserPreviewSummaryText(const QVector<PendingUploadSnapshot> &uploads)
{
    if (previewGeneratingUploadCount(uploads) > 0) {
        return QStringLiteral("Preview: generating");
    }
    return QStringLiteral("Preview: none");
}

QPixmap renderDiscoveryTile(
    const QSize &size,
    const QString &headline,
    const QString &subline,
    const QColor &accent,
    const QPixmap *preview,
    const bool isVideo,
    const double progressFraction = -1.0,
    const QString &progressText = QString())
{
    QPixmap canvas(size);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect bounds = canvas.rect().adjusted(3, 3, -3, -3);
    QPainterPath clipPath;
    clipPath.addRoundedRect(bounds, 20, 20);
    painter.setClipPath(clipPath);

    if (preview != nullptr && !preview->isNull()) {
        painter.fillRect(bounds, QColor(QStringLiteral("#020617")));
        const QPixmap scaled = preview->scaled(bounds.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QRect target = centeredAspectRect(scaled.size(), bounds);
        painter.drawPixmap(target.topLeft(), scaled);
        painter.fillRect(bounds, QColor(0, 0, 0, isVideo ? 28 : 18));
    } else {
        QLinearGradient gradient(bounds.topLeft(), bounds.bottomRight());
        gradient.setColorAt(0.0, accent.lighter(145));
        gradient.setColorAt(1.0, QColor(QStringLiteral("#111827")));
        painter.fillRect(bounds, gradient);
    }

    QLinearGradient topShade(bounds.topLeft(), QPoint(bounds.left(), bounds.top() + qMin(bounds.height() / 3, 72)));
    topShade.setColorAt(0.0, QColor(0, 0, 0, 120));
    topShade.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.fillRect(bounds, topShade);

    if (isVideo) {
        const int playSize = qMin(bounds.width(), bounds.height()) / 4;
        const QPoint center = bounds.center();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(15, 23, 42, 120));
        painter.drawEllipse(center, playSize / 2 + 10, playSize / 2 + 10);
        QPainterPath triangle;
        triangle.moveTo(center.x() - playSize / 3, center.y() - playSize / 2);
        triangle.lineTo(center.x() - playSize / 3, center.y() + playSize / 2);
        triangle.lineTo(center.x() + playSize / 2, center.y());
        triangle.closeSubpath();
        painter.setBrush(QColor(255, 255, 255, 210));
        painter.drawPath(triangle);
    }

    if (progressFraction >= 0.0) {
        const QRect progressBounds(bounds.left() + 16, bounds.bottom() - 24, bounds.width() - 32, 10);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(15, 23, 42, 180));
        painter.drawRoundedRect(progressBounds, 5, 5);
        QRect fillBounds = progressBounds;
        fillBounds.setWidth(qMax(8, static_cast<int>(progressBounds.width() * qBound(0.0, progressFraction, 1.0))));
        painter.setBrush(accent);
        painter.drawRoundedRect(fillBounds, 5, 5);

        if (!progressText.trimmed().isEmpty()) {
            QFont progressFont = painter.font();
            progressFont.setBold(true);
            progressFont.setPointSizeF(progressFont.pointSizeF() - 1.0);
            painter.setFont(progressFont);
            painter.setPen(QColor(QStringLiteral("#f8fafc")));
            painter.drawText(bounds.adjusted(16, 16, -16, -36), Qt::AlignLeft | Qt::AlignBottom, progressText);
        }
    }

    painter.setClipping(false);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 48), 1.0));
    painter.drawRoundedRect(bounds, 20, 20);

    if (preview == nullptr || preview->isNull()) {
        QFont titleFont = painter.font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(titleFont.pointSizeF() + 1.0);
        painter.setFont(titleFont);
        painter.setPen(QColor(QStringLiteral("#f8fafc")));
        painter.drawText(
            bounds.adjusted(16, 24, -16, -24),
            Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap,
            elidedTileText(headline, 40));
    }

    return canvas;
}

QIcon overlayDiscoveryStatusIcon(
    const QIcon &baseIcon,
    const QSize &size,
    const QColor &accent,
    const double progressFraction,
    const QString &statusText)
{
    if (baseIcon.isNull()) {
        return baseIcon;
    }

    QPixmap canvas = baseIcon.pixmap(size);
    if (canvas.isNull()) {
        return baseIcon;
    }

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRect bounds = canvas.rect().adjusted(3, 3, -3, -3);

    if (progressFraction >= 0.0) {
        const QRect progressBounds(bounds.left() + 16, bounds.bottom() - 24, bounds.width() - 32, 10);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(15, 23, 42, 180));
        painter.drawRoundedRect(progressBounds, 5, 5);
        QRect fillBounds = progressBounds;
        fillBounds.setWidth(qMax(8, static_cast<int>(progressBounds.width() * qBound(0.0, progressFraction, 1.0))));
        painter.setBrush(accent);
        painter.drawRoundedRect(fillBounds, 5, 5);
    }

    if (!statusText.trimmed().isEmpty()) {
        QFont statusFont = painter.font();
        statusFont.setBold(true);
        statusFont.setPointSizeF(statusFont.pointSizeF() - 1.0);
        painter.setFont(statusFont);

        const QString status = statusText.trimmed();
        const QRect statusRect = bounds.adjusted(14, bounds.height() - 52, -14, -30);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(15, 23, 42, 180));
        painter.drawRoundedRect(statusRect, 10, 10);
        painter.setPen(QColor(QStringLiteral("#f8fafc")));
        painter.drawText(statusRect.adjusted(10, 0, -10, 0), Qt::AlignLeft | Qt::AlignVCenter, status);
    }

    return QIcon(canvas);
}

bool parseMxcUrl(const QString &value, QString &serverName, QString &mediaId)
{
    if (!value.startsWith(QStringLiteral("mxc://"))) {
        return false;
    }
    const QString trimmed = value.mid(6);
    const int slashIndex = trimmed.indexOf(QLatin1Char('/'));
    if (slashIndex <= 0 || slashIndex == trimmed.size() - 1) {
        return false;
    }
    serverName = trimmed.left(slashIndex);
    mediaId = trimmed.mid(slashIndex + 1);
    return !serverName.isEmpty() && !mediaId.isEmpty();
}

QString matrixThumbnailUrl(const QString &homeserverUrl, const QString &mxcUrl, const int size)
{
    QString serverName;
    QString mediaId;
    if (!parseMxcUrl(mxcUrl, serverName, mediaId)) {
        return {};
    }

    QUrl base(homeserverUrl);
    if (!base.isValid()) {
        return {};
    }

    QString path = base.path();
    if (!path.endsWith(QLatin1Char('/'))) {
        path.append(QLatin1Char('/'));
    }
    path += QStringLiteral("_matrix/media/v3/thumbnail/%1/%2")
                .arg(QString::fromUtf8(QUrl::toPercentEncoding(serverName)),
                     QString::fromUtf8(QUrl::toPercentEncoding(mediaId)));
    base.setPath(path);

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("width"), QString::number(size));
    query.addQueryItem(QStringLiteral("height"), QString::number(size));
    query.addQueryItem(QStringLiteral("method"), QStringLiteral("scale"));
    base.setQuery(query);
    return base.toString();
}

bool sectionUsesRoomSidebar(const AppSection section)
{
    return section == AppSection::Browser || section == AppSection::Rooms;
}

}

MainWindow::MainWindow(AppController *controller, QWidget *parent)
    : QMainWindow(parent)
    , controller_(controller)
{
    applyTheme(controller_->settings().darkModeEnabled);
    setWindowTitle(QStringLiteral("Matrix Media Share Client"));
    resize(1380, 860);
    setAcceptDrops(true);
    thumbnailNetworkManager_ = new QNetworkAccessManager(this);
    windowConstraintTimer_ = new QTimer(this);
    windowConstraintTimer_->setSingleShot(true);
    connect(windowConstraintTimer_, &QTimer::timeout, this, [this]() {
        constrainToAvailableGeometry();
    });

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);

    roomSidebarContainer_ = new QWidget(central);
    roomSidebarContainer_->setFixedWidth(280);
    auto *roomSidebarLayout = new QVBoxLayout(roomSidebarContainer_);
    roomSidebarTitleLabel_ = new QLabel(QStringLiteral("Rooms"), roomSidebarContainer_);
    roomsList_ = new QListWidget(roomSidebarContainer_);
    roomsList_->setSelectionMode(QAbstractItemView::SingleSelection);
    roomSidebarLayout->addWidget(roomSidebarTitleLabel_);
    roomSidebarLayout->addWidget(roomsList_, 1);

    auto *contentContainer = new QWidget(central);
    auto *contentLayout = new QVBoxLayout(contentContainer);
    auto *navigationRow = new QHBoxLayout();
    auto *navigationLabel = new QLabel(QStringLiteral("Page"), contentContainer);
    sectionCombo_ = new QComboBox(contentContainer);
    sectionCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    sectionCombo_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    sectionCombo_->setMinimumContentsLength(0);
    pendingUploadsLabel_ = new QLabel(contentContainer);
    previewGenerationLabel_ = new QLabel(contentContainer);
    const QFontMetrics navMetrics(font());
    pendingUploadsLabel_->setMinimumWidth(navMetrics.horizontalAdvance(QStringLiteral("Uploads: none")) + 20);
    previewGenerationLabel_->setMinimumWidth(navMetrics.horizontalAdvance(QStringLiteral("Preview: generating")) + 20);
    pendingUploadsLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    previewGenerationLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    pendingUploadsLabel_->setStyleSheet(QStringLiteral("font-weight: 600;"));
    previewGenerationLabel_->setStyleSheet(QStringLiteral("font-weight: 600;"));
    pendingUploadsLabel_->setWordWrap(false);
    previewGenerationLabel_->setWordWrap(false);
    pendingUploadsLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    previewGenerationLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    navigationLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    navigationRow->addStretch();
    navigationRow->addWidget(pendingUploadsLabel_);
    navigationRow->addWidget(previewGenerationLabel_);
    navigationRow->addWidget(navigationLabel);
    navigationRow->addWidget(sectionCombo_);

    stack_ = new QStackedWidget(contentContainer);

    populateSectionNavigation();
    stack_->addWidget(buildBrowserPage());
    stack_->addWidget(buildRoomsPage());
    stack_->addWidget(buildLibraryPage());
    stack_->addWidget(buildTransfersPage());
    stack_->addWidget(buildLogsPage());
    stack_->addWidget(buildSettingsPage());
    stack_->addWidget(buildVerificationPage());

    contentLayout->addLayout(navigationRow);
    contentLayout->addWidget(stack_, 1);

    layout->addWidget(roomSidebarContainer_);
    layout->addWidget(contentContainer, 1);
    setCentralWidget(central);

    connect(sectionCombo_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        const AppSection previousSection = currentSection();
        if (stack_ != nullptr) {
            stack_->setCurrentIndex(index);
        }
        const AppSection nextSection = currentSection();
        if (previousSection == AppSection::Browser && nextSection != AppSection::Browser) {
            resetBrowserPageState();
        }
        if (previousSection == AppSection::Logs && nextSection != AppSection::Logs) {
            resetLogsPageState();
        }
        updateRoomSidebarVisibility();
        refreshView();
        if (currentSectionUsesRoomSidebar()) {
            const QString roomId = selectedRoomId();
            if (!roomId.isEmpty()) {
                controller_->focusRoom(roomId);
            }
        }
    });
    connect(roomsList_, &QListWidget::currentRowChanged, this, [this]() {
        const QString roomId = selectedRoomId();
        if (currentSection() == AppSection::Browser) {
            browserSelectedRoomId_ = roomId;
        } else if (currentSection() == AppSection::Rooms) {
            roomsPageSelectedRoomId_ = roomId;
        }
        populateRoomsPage();
        populateBrowserPage();
        if (!roomId.isEmpty()) {
            controller_->focusRoom(roomId);
        }
    });
    connect(controller_, &AppController::stateChanged, this, &MainWindow::refreshView);
    connect(controller_, &AppController::userNoticeRequested, this, [this](const QString &title, const QString &message) {
        QMessageBox::warning(this, title, message);
    });

    constrainToAvailableGeometry();
    sectionCombo_->setCurrentIndex(sectionIndex(AppSection::Browser));
    updateRoomSidebarVisibility();
    refreshView();
    if (currentSectionUsesRoomSidebar()) {
        const QString roomId = selectedRoomId();
        if (!roomId.isEmpty()) {
            controller_->focusRoom(roomId);
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (event != nullptr && settingsPageInitialized_ && settingsDirty_ && !saveSettingsFromUi(true)) {
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    scheduleWindowConstraint();
}

void MainWindow::moveEvent(QMoveEvent *event)
{
    QMainWindow::moveEvent(event);
    scheduleWindowConstraint();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    scheduleWindowConstraint();
}

void MainWindow::scheduleWindowConstraint()
{
    if (windowConstraintTimer_ == nullptr) {
        constrainToAvailableGeometry();
        return;
    }

#ifdef Q_OS_WIN
    windowConstraintTimer_->start(120);
#else
    windowConstraintTimer_->start(0);
#endif
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
    refreshViewerDialog();

    if (currentSectionUsesRoomSidebar()) {
        populateRoomSidebar();
    }

    switch (currentSection()) {
    case AppSection::Browser:
        populateBrowserPage();
        break;
    case AppSection::Rooms:
        populateRoomsPage();
        break;
    case AppSection::Library:
        populateLibraryPage();
        break;
    case AppSection::Transfers:
        populateTransfersPage();
        break;
    case AppSection::Logs:
        populateLogsPage();
        break;
    case AppSection::Settings:
        populateSettingsPage();
        break;
    case AppSection::Verification:
        populateVerificationPage();
        break;
    }

    if (!controller_->lastErrorMessage().isEmpty()) {
        controller_->dismissError();
    }
}

void MainWindow::populateSectionNavigation()
{
    sectionCombo_->blockSignals(true);
    sectionCombo_->clear();
    for (const AppSection section : allSections()) {
        sectionCombo_->addItem(sectionTitle(section), static_cast<int>(section));
    }
    sectionCombo_->blockSignals(false);
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

    roomDetailLabel_ = new QLabel(page);
    roomDetailLabel_->setWordWrap(true);
    leaveRoomButton_ = new QPushButton(QStringLiteral("Leave Selected"), page);
    auto *roomHelpLabel = new QLabel(
        QStringLiteral("Use the room list in the left pane to inspect memberships, aliases, and folders."),
        page);
    roomHelpLabel->setWordWrap(true);
    layout->addWidget(roomHelpLabel);
    layout->addWidget(roomDetailLabel_);
    layout->addWidget(leaveRoomButton_);
    layout->addStretch();

    connect(joinButton, &QPushButton::clicked, this, [this]() {
        controller_->joinRoom(joinRoomEdit_->text());
    });
    connect(leaveRoomButton_, &QPushButton::clicked, this, [this]() {
        const QListWidgetItem *item = roomsList_->currentItem();
        if (item != nullptr) {
            controller_->leaveRoom(item->data(Qt::UserRole).toString());
        }
    });

    return page;
}

QWidget *MainWindow::buildBrowserPage()
{
    browserPage_ = new QWidget(this);
    browserPage_->setAcceptDrops(true);
    browserPage_->installEventFilter(this);
    auto *layout = new QVBoxLayout(browserPage_);

    auto *statusRow = new QHBoxLayout();
    powerToggle_ = new QCheckBox(QStringLiteral("Power"), browserPage_);
    connectionLabel_ = new QLabel(browserPage_);
    ipfsStatusLabel_ = new QLabel(browserPage_);
    uploadLimitLabel_ = new QLabel(browserPage_);
    updateBannerLabel_ = new QLabel(browserPage_);
    updateBannerLabel_->setWordWrap(true);
    statusRow->addWidget(powerToggle_);
    statusRow->addWidget(connectionLabel_);
    statusRow->addWidget(ipfsStatusLabel_);
    statusRow->addWidget(uploadLimitLabel_);
    statusRow->addWidget(updateBannerLabel_);
    statusRow->addStretch();
    layout->addLayout(statusRow);

    auto *shareRow = new QHBoxLayout();
    browserSelectedRoomLabel_ = new QLabel(browserPage_);
    browserSelectedRoomLabel_->setWordWrap(true);
    shareFilesButton_ = new QPushButton(QStringLiteral("Upload Files"), browserPage_);
    auto *importButton = new QPushButton(QStringLiteral("Import IPFS Link"), browserPage_);
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"), browserPage_);
    shareRow->addWidget(new QLabel(QStringLiteral("Selected Room"), browserPage_));
    shareRow->addWidget(browserSelectedRoomLabel_, 1);
    shareRow->addWidget(shareFilesButton_);
    shareRow->addWidget(importButton);
    shareRow->addWidget(refreshButton);
    layout->addLayout(shareRow);

    browserDropHintLabel_ = new QLabel(
        QStringLiteral("Drop one or more files anywhere on this window while Browser is open to queue uploads into the selected room."),
        browserPage_);
    browserDropHintLabel_->setWordWrap(true);
    layout->addWidget(browserDropHintLabel_);

    discoveriesList_ = new QListWidget(browserPage_);
    discoveriesList_->setAcceptDrops(true);
    discoveriesList_->installEventFilter(this);
    discoveriesList_->viewport()->setAcceptDrops(true);
    discoveriesList_->viewport()->installEventFilter(this);
    discoveriesList_->setSelectionMode(QAbstractItemView::SingleSelection);
    discoveriesList_->setViewMode(QListView::IconMode);
    discoveriesList_->setResizeMode(QListView::Adjust);
    discoveriesList_->setMovement(QListView::Static);
    discoveriesList_->setWordWrap(true);
    discoveriesList_->setSpacing(12);
    discoveriesList_->setIconSize(QSize(kDiscoveryTileWidth, kDiscoveryTileHeight));
    discoveriesList_->setGridSize(QSize(kDiscoveryGridWidth, kDiscoveryGridHeight));
    discoveriesList_->setUniformItemSizes(false);
    discoveriesList_->setTextElideMode(Qt::ElideNone);
    layout->addWidget(discoveriesList_, 1);

    auto *browserButtonRow = new QHBoxLayout();
    openDiscoveryButton_ = new QPushButton(QStringLiteral("Open Selected"), browserPage_);
    downloadDiscoveryButton_ = new QPushButton(QStringLiteral("Download Selected"), browserPage_);
    browserButtonRow->addWidget(openDiscoveryButton_);
    browserButtonRow->addWidget(downloadDiscoveryButton_);
    browserButtonRow->addStretch();
    layout->addLayout(browserButtonRow);

    connect(powerToggle_, &QCheckBox::toggled, this, [this](const bool enabled) {
        if (enabled && settingsPageInitialized_ && settingsDirty_ && !saveSettingsFromUi(false)) {
            powerToggle_->blockSignals(true);
            powerToggle_->setChecked(false);
            powerToggle_->blockSignals(false);
            return;
        }
        controller_->togglePower(enabled);
    });
    connect(refreshButton, &QPushButton::clicked, controller_, &AppController::refreshCatalog);
    connect(shareFilesButton_, &QPushButton::clicked, this, [this]() {
        const QString roomId = selectedBrowserRoomId();
        const QStringList filePaths = QFileDialog::getOpenFileNames(this, QStringLiteral("Choose Files To Share"));
        if (!isUploadableBrowserRoom(roomId)) {
            controller_->recordWarning(
                QStringLiteral("share"),
                QStringLiteral("Pick a joined room from the Browser room list before sharing files."));
            return;
        }
        if (!filePaths.isEmpty()) {
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
    connect(discoveriesList_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        Q_UNUSED(value);
        requestVisibleBrowserThumbnails();
        maybeLoadMoreBrowserDiscoveries();
    });

    return browserPage_;
}

QWidget *MainWindow::buildLibraryPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    libraryList_ = new QListWidget(page);
    libraryList_->setViewMode(QListView::IconMode);
    libraryList_->setResizeMode(QListView::Adjust);
    libraryList_->setMovement(QListView::Static);
    libraryList_->setIconSize(QSize(kDiscoveryTileWidth, kDiscoveryTileHeight));
    libraryList_->setGridSize(QSize(kDiscoveryGridWidth, kDiscoveryGridHeight));
    libraryList_->setSpacing(12);
    libraryList_->setWordWrap(true);
    libraryList_->setTextElideMode(Qt::ElideRight);
    libraryList_->setSelectionMode(QAbstractItemView::SingleSelection);
    libraryList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    openLibraryButton_ = new QPushButton(QStringLiteral("Open Selected"), page);
    deleteLibraryButton_ = new QPushButton(QStringLiteral("Delete Selected"), page);
    auto *buttonRow = new QHBoxLayout();
    buttonRow->addWidget(openLibraryButton_);
    buttonRow->addWidget(deleteLibraryButton_);
    buttonRow->addStretch();
    layout->addWidget(libraryList_, 1);
    layout->addLayout(buttonRow);

    connect(openLibraryButton_, &QPushButton::clicked, this, &MainWindow::openSelectedSharedItem);
    connect(deleteLibraryButton_, &QPushButton::clicked, this, &MainWindow::deleteSelectedSharedItem);
    connect(libraryList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item != nullptr) {
            openSelectedSharedItem();
        }
    });
    connect(libraryList_, &QListWidget::currentRowChanged, this, [this]() {
        const bool hasSelection = libraryList_->currentItem() != nullptr;
        openLibraryButton_->setEnabled(hasSelection);
        deleteLibraryButton_->setEnabled(hasSelection);
    });

    return page;
}

QWidget *MainWindow::buildTransfersPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    queueStatsLabel_ = new QLabel(page);
    layout->addWidget(queueStatsLabel_);

    pendingUploadsList_ = new QListWidget(page);
    layout->addWidget(pendingUploadsList_);

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

QWidget *MainWindow::buildLogsPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    logsSummaryLabel_ = new QLabel(page);
    logsSummaryLabel_->setWordWrap(true);
    layout->addWidget(logsSummaryLabel_);

    logProblemsOnlyCheck_ = new QCheckBox(QStringLiteral("Show only warnings and errors"), page);
    layout->addWidget(logProblemsOnlyCheck_);

    logsTable_ = new QTableWidget(page);
    logsTable_->setColumnCount(4);
    logsTable_->setHorizontalHeaderLabels({
        QStringLiteral("Time"),
        QStringLiteral("Level"),
        QStringLiteral("Subsystem"),
        QStringLiteral("Message"),
    });
    logsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    logsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    logsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    logsTable_->verticalHeader()->setVisible(false);
    logsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    logsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    logsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    logsTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(logsTable_, 1);

    connect(logProblemsOnlyCheck_, &QCheckBox::toggled, this, [this]() {
        populateLogsPage();
    });
    connect(logsTable_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        Q_UNUSED(value);
        maybeLoadMoreLogs();
    });

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
    darkModeCheck_ = new QCheckBox(content);
    archiveScanEnabledCheck_ = new QCheckBox(content);
    archiveHighPriorityCheck_ = new QCheckBox(content);
    selfHealCheck_ = new QCheckBox(content);
    flatFolderLayoutCheck_ = new QCheckBox(content);
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

    addPathPickerRow(QStringLiteral("Downloads Root"), destinationEdit_, QStringLiteral("Choose Downloads Root"));
    addPathPickerRow(QStringLiteral("Shared Files Root (Managed)"), libraryEdit_, QStringLiteral("Choose Shared Files Root"));
    addPathPickerRow(QStringLiteral("Archive Root"), archiveEdit_, QStringLiteral("Choose Archive Root"));

    form->addRow(QStringLiteral("Homeserver"), homeserverEdit_);
    form->addRow(QStringLiteral("Username"), usernameEdit_);
    form->addRow(QStringLiteral("Password"), passwordEdit_);
    form->addRow(QStringLiteral("Current Version"), currentVersionLabel_);
    form->addRow(QStringLiteral("Update Status"), updateStatusLabel_);
    form->addRow(QStringLiteral("Latest Release"), latestReleaseLabel_);
    form->addRow(QStringLiteral("Last Checked"), lastCheckedLabel_);
    form->addRow(QStringLiteral("Archive Scan Enabled"), archiveScanEnabledCheck_);
    form->addRow(QStringLiteral("Archive Scan High Priority"), archiveHighPriorityCheck_);
    form->addRow(QStringLiteral("Self-Heal Shared Files"), selfHealCheck_);
    form->addRow(QStringLiteral("Flat Folder Layout"), flatFolderLayoutCheck_);
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
    form->addRow(QStringLiteral("Dark Mode"), darkModeCheck_);
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
        saveSettingsFromUi(true);
    });
    connect(resetButton, &QPushButton::clicked, controller_, &AppController::resetHistoryScans);
    connect(checkUpdatesButton_, &QPushButton::clicked, this, [this]() {
        controller_->checkForUpdates(true);
    });
    connect(openLatestReleaseButton_, &QPushButton::clicked, controller_, &AppController::openLatestReleasePage);

    settingsDatabasePathLabel_ = new QLabel(content);
    secretStorePathLabel_ = new QLabel(content);
    for (QLabel *label : {settingsDatabasePathLabel_, secretStorePathLabel_}) {
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    form->insertRow(7, QStringLiteral("Settings Database"), settingsDatabasePathLabel_);
    form->insertRow(8, QStringLiteral("Secret Store"), secretStorePathLabel_);

    for (QLineEdit *edit : {homeserverEdit_, usernameEdit_, passwordEdit_, destinationEdit_, libraryEdit_, archiveEdit_, primaryGatewayEdit_}) {
        connect(edit, &QLineEdit::textChanged, this, [this]() {
            markSettingsDirty();
        });
    }
    connect(preferredGatewaysEdit_, &QTextEdit::textChanged, this, [this]() {
        markSettingsDirty();
    });
    for (QSpinBox *spin : {messageLimitSpin_, retryCooldownSpin_, retryLimitSpin_, downloadWorkersSpin_, bandwidthSpin_, previewWorkersSpin_}) {
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
            markSettingsDirty();
        });
    }
    for (QCheckBox *check : {archiveScanEnabledCheck_, archiveHighPriorityCheck_, selfHealCheck_, flatFolderLayoutCheck_, autostartCheck_, minimizeToTrayCheck_, startHiddenCheck_, darkModeCheck_, autoJoinSpacesCheck_, autoDownloadCheck_}) {
        connect(check, &QCheckBox::toggled, this, [this](bool) {
            markSettingsDirty();
        });
    }

    return page;
}

QWidget *MainWindow::buildVerificationPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    verificationStatusLabel_ = new QLabel(page);
    verificationDeviceIdLabel_ = new QLabel(page);
    verificationMessageLabel_ = new QLabel(page);
    verificationMessageLabel_->setWordWrap(true);
    verificationEmojiList_ = new QListWidget(page);
    verificationDecimalsLabel_ = new QLabel(page);

    auto *buttonRow = new QHBoxLayout();
    requestVerificationButton_ = new QPushButton(QStringLiteral("Set Up / Request"), page);
    startVerificationButton_ = new QPushButton(QStringLiteral("Start SAS"), page);
    approveVerificationButton_ = new QPushButton(QStringLiteral("Approve"), page);
    declineVerificationButton_ = new QPushButton(QStringLiteral("Reject"), page);
    buttonRow->addWidget(requestVerificationButton_);
    buttonRow->addWidget(startVerificationButton_);
    buttonRow->addWidget(approveVerificationButton_);
    buttonRow->addWidget(declineVerificationButton_);
    buttonRow->addStretch();

    layout->addWidget(verificationStatusLabel_);
    layout->addWidget(verificationDeviceIdLabel_);
    layout->addWidget(verificationMessageLabel_);
    layout->addLayout(buttonRow);
    layout->addWidget(verificationEmojiList_, 1);
    layout->addWidget(verificationDecimalsLabel_);

    connect(requestVerificationButton_, &QPushButton::clicked, controller_, &AppController::requestVerification);
    connect(startVerificationButton_, &QPushButton::clicked, controller_, &AppController::startSasVerification);
    connect(approveVerificationButton_, &QPushButton::clicked, controller_, &AppController::approveVerification);
    connect(declineVerificationButton_, &QPushButton::clicked, controller_, &AppController::declineVerification);

    return page;
}

void MainWindow::populateRoomsPage()
{
    const QListWidgetItem *currentItem = roomsList_->currentItem();
    if (currentItem == nullptr) {
        roomDetailLabel_->setText(QStringLiteral("Join a room or space to begin browsing shared media."));
        leaveRoomButton_->setEnabled(false);
        return;
    }

    leaveRoomButton_->setEnabled(currentSection() == AppSection::Rooms);
    const QString roomId = currentItem->data(Qt::UserRole).toString();
    const auto rooms = controller_->rooms();
    for (const RoomRecord &room : rooms) {
        if (room.roomId == roomId) {
            roomDetailLabel_->setText(QStringLiteral("ID: %1\nAlias: %2\nFolder: %3\nMembership: %4\nCached Media: %5")
                                          .arg(
                                              room.roomId,
                                              room.currentCanonicalAlias,
                                              room.activeFolderLabel,
                                              room.membership,
                                              QString::number(room.discoveredMediaCount)));
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
    pendingUploadsLabel_->setText(browserUploadSummaryText(runtime.pendingUploads));
    previewGenerationLabel_->setText(browserPreviewSummaryText(runtime.pendingUploads));
    if (controller_->updateAvailable() || controller_->isUpdateCheckInProgress()) {
        updateBannerLabel_->setText(QStringLiteral("Updates: %1").arg(controller_->updateStatusText()));
    } else {
        updateBannerLabel_->clear();
    }

    const QString currentRoomId = selectedBrowserRoomId();
    QString selectedRoomTitle = QStringLiteral("None");
    QString selectedMembership;
    bool selectedIsSpace = false;
    for (const RoomRecord &room : controller_->rooms()) {
        if (room.roomId == currentRoomId) {
            selectedRoomTitle = roomDisplayTitle(room);
            selectedMembership = room.membership;
            selectedIsSpace = room.isSpace;
            if (room.isSpace) {
                selectedRoomTitle += QStringLiteral(" [Space]");
            }
            break;
        }
    }
    browserSelectedRoomLabel_->setText(selectedRoomTitle);
    const bool uploadable = !currentRoomId.isEmpty() && !selectedIsSpace && selectedMembership == QStringLiteral("joined");
    if (shareFilesButton_ != nullptr) {
        shareFilesButton_->setEnabled(uploadable);
    }
    browserDropHintLabel_->setText(currentRoomId.isEmpty()
            ? QStringLiteral("Pick a room from the left pane before dropping files or uploading.")
            : uploadable
                ? QStringLiteral("Drop one or more files anywhere on this window while Browser is open to queue uploads into %1.")
                      .arg(selectedRoomTitle)
                : QStringLiteral("%1 is cached for browsing, but you need to join it from the Rooms page before uploading.")
                      .arg(selectedRoomTitle));

    const int totalDiscoveries = currentRoomId.isEmpty() ? 0 : controller_->discoveryCount(currentRoomId);
    const bool roomChanged = browserLoadedRoomId_ != currentRoomId;
    if (roomChanged || totalDiscoveries != browserTotalDiscoveryCount_) {
        resetBrowserPageState();
        browserLoadedRoomId_ = currentRoomId;
        browserTotalDiscoveryCount_ = totalDiscoveries;
    }

    if (currentRoomId.isEmpty() || browserTotalDiscoveryCount_ <= 0) {
        discoveriesList_->setEnabled(false);
        openDiscoveryButton_->setEnabled(false);
        downloadDiscoveryButton_->setEnabled(false);
        return;
    }

    discoveriesList_->setEnabled(true);
    if (browserLoadedDiscoveries_.isEmpty()) {
        loadMoreBrowserDiscoveries(true);
    } else {
        syncBrowserLoadedDiscoveries();
        requestVisibleBrowserThumbnails();
        maybeLoadMoreBrowserDiscoveries();
        openDiscoveryButton_->setEnabled(discoveriesList_->currentItem() != nullptr);
        downloadDiscoveryButton_->setEnabled(discoveriesList_->currentItem() != nullptr);
    }
}

void MainWindow::populateLibraryPage()
{
    if (libraryList_ == nullptr) {
        return;
    }

    const QVector<SharedItemRecord> items = controller_->sharedItems();
    const QString signature = sharedItemSignature(items);
    const QString selectedSha = libraryList_->currentItem() != nullptr
        ? libraryList_->currentItem()->data(Qt::UserRole).toString()
        : QString();

    if (signature == sharedItemsSignature_) {
        const bool hasSelection = libraryList_->currentItem() != nullptr;
        openLibraryButton_->setEnabled(hasSelection);
        if (deleteLibraryButton_ != nullptr) {
            deleteLibraryButton_->setEnabled(hasSelection);
        }
        return;
    }

    sharedItemsSignature_ = signature;
    sharedItemIconCache_.clear();

    QSignalBlocker blocker(libraryList_);
    libraryList_->setUpdatesEnabled(false);
    libraryList_->clear();

    int selectedRow = -1;
    for (int index = 0; index < items.size(); ++index) {
        const SharedItemRecord &item = items.at(index);
        auto *listItem = new QListWidgetItem(sharedItemIcon(item), QStringLiteral("%1 | %2")
            .arg(mediaCategoryTitle(item.category).toUpper(), sharedItemOriginLabel(item).toUpper()));
        listItem->setData(Qt::UserRole, item.sha256);
        listItem->setData(Qt::UserRole + 1, sharedItemLocalPath(item));
        listItem->setData(Qt::UserRole + 2, item.originalFilename);
        listItem->setData(Qt::UserRole + 3, item.mimeType);
        listItem->setData(Qt::UserRole + 4, static_cast<int>(item.category));
        listItem->setToolTip(
            QStringLiteral("%1\nHash: %2\nSource: %3\nUpdated: %4")
                .arg(
                    item.originalFilename.isEmpty() ? item.sha256.left(16) : item.originalFilename,
                    item.sha256,
                    QDir::toNativeSeparators(sharedItemLocalPath(item)),
                    displayDateTime(item.updatedAt)));
        libraryList_->addItem(listItem);
        if (!selectedSha.isEmpty() && item.sha256 == selectedSha) {
            selectedRow = index;
        }
    }

    if (selectedRow >= 0) {
        libraryList_->setCurrentRow(selectedRow);
    }

    libraryList_->setUpdatesEnabled(true);
    const bool hasSelection = libraryList_->currentItem() != nullptr;
    openLibraryButton_->setEnabled(hasSelection);
    if (deleteLibraryButton_ != nullptr) {
        deleteLibraryButton_->setEnabled(hasSelection);
    }
}

void MainWindow::populateRoomSidebar()
{
    if (roomsList_ == nullptr) {
        return;
    }

    const AppSection section = currentSection();
    const QString previousRoomId = section == AppSection::Browser
        ? browserSelectedRoomId_
        : roomsPageSelectedRoomId_;
    const int previousScrollValue = roomsList_->verticalScrollBar() != nullptr
        ? roomsList_->verticalScrollBar()->value()
        : 0;
    roomsList_->blockSignals(true);
    roomsList_->clear();
    roomSidebarTitleLabel_->setText(section == AppSection::Browser
            ? QStringLiteral("Browser Rooms")
            : QStringLiteral("Rooms"));

    int restoredRow = -1;
    const auto rooms = roomSidebarRoomsForCurrentSection();
    for (int index = 0; index < rooms.size(); ++index) {
        const RoomRecord &room = rooms.at(index);
        auto *item = new QListWidgetItem(roomDisplayTitle(room), roomsList_);
        item->setData(Qt::UserRole, room.roomId);
        if (room.isSpace) {
            item->setText(item->text() + QStringLiteral(" [Space]"));
        } else if (section == AppSection::Browser && room.membership != QStringLiteral("joined")) {
            item->setText(item->text() + QStringLiteral(" [Cached]"));
        }
        if (room.roomId == previousRoomId) {
            restoredRow = index;
        }
    }

    if (restoredRow < 0 && section == AppSection::Browser) {
        for (int index = 0; index < rooms.size(); ++index) {
            if (!rooms.at(index).isSpace && rooms.at(index).membership == QStringLiteral("joined")) {
                restoredRow = index;
                break;
            }
        }
    }

    if (roomsList_->count() > 0) {
        roomsList_->setCurrentRow(restoredRow >= 0 ? restoredRow : 0);
        if (section == AppSection::Browser) {
            browserSelectedRoomId_ = selectedRoomId();
        } else if (section == AppSection::Rooms) {
            roomsPageSelectedRoomId_ = selectedRoomId();
        }
    } else if (section == AppSection::Browser) {
        browserSelectedRoomId_.clear();
    } else if (section == AppSection::Rooms) {
        roomsPageSelectedRoomId_.clear();
    }
    roomsList_->blockSignals(false);
    if (roomsList_->verticalScrollBar() != nullptr) {
        roomsList_->verticalScrollBar()->setValue(previousScrollValue);
    }
}

void MainWindow::populateTransfersPage()
{
    const QVector<PendingUploadSnapshot> &pendingUploads = controller_->runtime().pendingUploads;
    const int activeUploadCount = activePendingUploadCount(pendingUploads);
    const int queuedUploadCount = qMax(0, pendingUploads.size() - activeUploadCount);
    queueStatsLabel_->setText(
        QStringLiteral("Downloads waiting: %1   Downloads active: %2   Uploads queued: %3   Uploads active: %4")
            .arg(controller_->waitingQueueCount())
            .arg(controller_->runtime().activeDownloads.size())
            .arg(queuedUploadCount)
            .arg(activeUploadCount));

    if (pendingUploadsList_ != nullptr) {
        pendingUploadsList_->clear();
        for (const PendingUploadSnapshot &upload : pendingUploads) {
            const QString displayName = upload.fileName.trimmed().isEmpty()
                ? QFileInfo(upload.filePath).fileName()
                : upload.fileName;
            pendingUploadsList_->addItem(
                QStringLiteral("%1 [%2] -> %3")
                    .arg(displayName,
                         upload.state.trimmed().isEmpty() ? QStringLiteral("queued") : upload.state,
                         upload.roomId));
        }
    }

    activeDownloadsList_->clear();
    for (const ActiveDownloadSnapshot &download : controller_->runtime().activeDownloads) {
        const QString progress = transferProgressText(download.receivedBytes, download.totalBytes);
        activeDownloadsList_->addItem(
            progress.isEmpty()
                ? QStringLiteral("Worker %1: %2").arg(download.workerId).arg(download.filename)
                : QStringLiteral("Worker %1: %2 (%3)")
                      .arg(download.workerId)
                      .arg(download.filename, progress));
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
        QString stateText = downloadJobStateTitle(job.state);
        const QString progress = transferProgressText(job.receivedBytes, job.totalBytes);
        if (!progress.isEmpty()) {
            stateText += QStringLiteral(" (%1)").arg(progress);
        }
        waitingJobsTable_->setItem(row, 0, new QTableWidgetItem(jobTitle(job)));
        waitingJobsTable_->setItem(row, 1, new QTableWidgetItem(job.roomId));
        waitingJobsTable_->setItem(row, 2, new QTableWidgetItem(stateText));
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

void MainWindow::populateLogsPage()
{
    const bool problemsOnly = logProblemsOnlyCheck_ != nullptr && logProblemsOnlyCheck_->isChecked();
    const int totalLogs = controller_->logCount(problemsOnly);
    if (problemsOnly != logsProblemsOnly_ || totalLogs != logTotalCount_) {
        resetLogsPageState();
        logsProblemsOnly_ = problemsOnly;
        logTotalCount_ = totalLogs;
    }

    if (logsSummaryLabel_ != nullptr) {
        const QString filterText = problemsOnly
            ? QStringLiteral("warnings/errors")
            : QStringLiteral("all entries");
        logsSummaryLabel_->setText(
            QStringLiteral("Showing %1 of %2 %3 from the local activity log. Newest entries appear first.")
                .arg(loadedLogEntries_.size())
                .arg(logTotalCount_)
                .arg(filterText));
    }

    if (logsTable_ == nullptr || logTotalCount_ <= 0) {
        if (logsTable_ != nullptr) {
            logsTable_->setRowCount(0);
        }
        return;
    }

    if (loadedLogEntries_.isEmpty()) {
        loadMoreLogs(true);
    } else {
        maybeLoadMoreLogs();
    }
}

void MainWindow::populateSettingsPage()
{
    currentVersionLabel_->setText(controller_->currentVersion());
    updateStatusLabel_->setText(controller_->updateStatusText());
    latestReleaseLabel_->setText(controller_->latestReleaseSummaryText());
    lastCheckedLabel_->setText(displayDateTime(controller_->updateCheckState().lastCheckedAt));
    settingsDatabasePathLabel_->setText(QDir::toNativeSeparators(controller_->settingsDatabasePath()));
    secretStorePathLabel_->setText(QDir::toNativeSeparators(controller_->secretStorePath()));
    checkUpdatesButton_->setEnabled(!controller_->isUpdateCheckInProgress());
    openLatestReleaseButton_->setEnabled(!controller_->latestReleasePageUrl().trimmed().isEmpty());

    if (settingsDirty_) {
        settingsPageInitialized_ = true;
        return;
    }

    const AppSettings &settings = controller_->settings();
    populatingSettingsUi_ = true;
    homeserverEdit_->setText(settings.homeserverUrl);
    usernameEdit_->setText(settings.username);
    passwordEdit_->setText(controller_->password());
    destinationEdit_->setText(settings.destinationRootPath);
    libraryEdit_->setText(settings.libraryRootPath);
    archiveEdit_->setText(settings.archiveRootPath);
    archiveScanEnabledCheck_->setChecked(settings.archiveScanEnabled);
    archiveHighPriorityCheck_->setChecked(settings.archiveScanHighPriority);
    selfHealCheck_->setChecked(settings.selfHealEnabled);
    flatFolderLayoutCheck_->setChecked(settings.flatFolderLayout);
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
    darkModeCheck_->setChecked(settings.darkModeEnabled);
    autoJoinSpacesCheck_->setChecked(settings.autoJoinSpaceRooms);
    autoDownloadCheck_->setChecked(settings.autoDownloadNewMedia);
    populatingSettingsUi_ = false;
    settingsDirty_ = false;
    settingsPageInitialized_ = true;
}

void MainWindow::populateVerificationPage()
{
    const VerificationSnapshot &verification = controller_->runtime().verification;
    verificationStatusLabel_->setText(QStringLiteral("Status: %1").arg(verificationStatusTitle(verification.state)));
    verificationDeviceIdLabel_->setText(
        QStringLiteral("Device: %1").arg(verification.deviceId.isEmpty() ? QStringLiteral("Unknown") : verification.deviceId));
    verificationMessageLabel_->setText(verification.message);
    verificationEmojiList_->clear();
    for (const VerificationEmoji &emoji : verification.emojis) {
        verificationEmojiList_->addItem(QStringLiteral("%1  %2").arg(emoji.symbol, emoji.description));
    }

    QStringList decimals;
    for (const quint16 value : verification.decimals) {
        decimals.append(QString::number(value));
    }
    verificationDecimalsLabel_->setText(QStringLiteral("Decimals: %1").arg(decimals.join(QStringLiteral(", "))));

    if (verification.canBootstrapCrossSigning) {
        requestVerificationButton_->setText(QStringLiteral("Set Up Verification"));
    } else if (verification.otherDeviceCount > 0) {
        requestVerificationButton_->setText(QStringLiteral("Request Other Device"));
    } else {
        requestVerificationButton_->setText(QStringLiteral("Repair Verification"));
    }

    if (verification.requestCanAccept) {
        startVerificationButton_->setText(QStringLiteral("Accept Request"));
    } else if (verification.sasCanAccept) {
        startVerificationButton_->setText(QStringLiteral("Accept SAS"));
    } else {
        startVerificationButton_->setText(QStringLiteral("Start SAS"));
    }

    const bool connected = controller_->runtime().connectionState == ConnectionState::Running;
    requestVerificationButton_->setEnabled(connected);
    startVerificationButton_->setEnabled(
        connected && (verification.requestReady || verification.requestCanAccept || verification.sasCanAccept));
    approveVerificationButton_->setEnabled(
        connected && verification.hasActiveSas && (!verification.emojis.isEmpty() || !verification.decimals.isEmpty()));
    declineVerificationButton_->setEnabled(connected && (verification.hasActiveRequest || verification.hasActiveSas));
}

void MainWindow::refreshViewerDialog()
{
    const ViewerSnapshot runtimeViewer = controller_->runtime().viewer;
    const bool runtimeViewerActive = runtimeViewer.state != ViewerState::Idle && runtimeViewer.sessionId != 0;
    const bool localViewerActive = localViewerActive_ && localViewerSnapshot_.state != ViewerState::Idle
        && localViewerSnapshot_.sessionId != 0;
    const ViewerSnapshot viewer = runtimeViewerActive ? runtimeViewer : localViewerSnapshot_;

    if ((!runtimeViewerActive && !localViewerActive) || viewer.state == ViewerState::Idle || viewer.sessionId == 0) {
        if (viewerDialog_ != nullptr) {
            viewerDialog_->hide();
        }
        if (viewerMediaPlayer_ != nullptr) {
            viewerMediaPlayer_->stop();
        }
        if (viewerVideoWidget_ != nullptr) {
            viewerVideoWidget_->clearFrame();
        }
        if (viewerVlcWidget_ != nullptr) {
            viewerVlcWidget_->stopPlayback();
        }
        if (viewerWebVideoWidget_ != nullptr) {
            viewerWebVideoWidget_->clearMedia();
        }
        viewerLoadedSessionId_ = 0;
        viewerLoadedLocalPath_.clear();
        viewerLoadedState_ = ViewerState::Idle;
        return;
    }

    if (runtimeViewerActive && viewer.sessionId == viewerDismissedSessionId_) {
        return;
    }

    ensureViewerDialog();
    if (viewerDialog_ == nullptr) {
        return;
    }

    if (!viewerDialog_->isVisible()) {
        viewerDialog_->show();
        viewerDialog_->raise();
        viewerDialog_->activateWindow();
    }

    viewerTitleLabel_->setText(viewer.fileName.isEmpty() ? QStringLiteral("Viewer") : viewer.fileName);
    switch (viewer.state) {
    case ViewerState::Downloading:
        if (viewer.totalBytes > 0) {
            viewerProgressBar_->setRange(0, 1000);
            viewerProgressBar_->setValue(static_cast<int>(
                qBound(0.0, static_cast<double>(viewer.receivedBytes) / static_cast<double>(viewer.totalBytes), 1.0)
                * 1000.0));
            viewerStatusLabel_->setText(QStringLiteral("Downloading %1 of %2")
                                            .arg(dataSizeText(viewer.receivedBytes), dataSizeText(viewer.totalBytes)));
        } else {
            viewerProgressBar_->setRange(0, 0);
            viewerStatusLabel_->setText(QStringLiteral("Downloading media for viewing..."));
        }
        break;
    case ViewerState::Ready:
        viewerProgressBar_->setRange(0, 1000);
        viewerProgressBar_->setValue(1000);
        viewerStatusLabel_->setText(QStringLiteral("Ready"));
        break;
    case ViewerState::Error:
        viewerProgressBar_->setRange(0, 1000);
        viewerProgressBar_->setValue(0);
        viewerStatusLabel_->setText(viewer.error.isEmpty() ? QStringLiteral("Viewer error") : viewer.error);
        break;
    case ViewerState::Idle:
        break;
    }

    if (viewer.sessionId != viewerLoadedSessionId_
        || viewer.localPath != viewerLoadedLocalPath_
        || viewer.state != viewerLoadedState_) {
        loadViewerMedia(viewer);
    }
}

void MainWindow::ensureViewerDialog()
{
    if (viewerDialog_ != nullptr) {
        return;
    }

    viewerDialog_ = new QDialog(this, Qt::Window);
    viewerDialog_->setAttribute(Qt::WA_DeleteOnClose, false);
    viewerDialog_->setWindowTitle(QStringLiteral("Media Viewer"));
    viewerDialog_->setMinimumSize(420, 320);

    auto *layout = new QVBoxLayout(viewerDialog_);
    viewerTitleLabel_ = new QLabel(viewerDialog_);
    viewerTitleLabel_->setWordWrap(true);
    viewerStatusLabel_ = new QLabel(viewerDialog_);
    viewerStatusLabel_->setWordWrap(true);
    viewerProgressBar_ = new QProgressBar(viewerDialog_);
    viewerProgressBar_->setTextVisible(true);

    viewerContentStack_ = new QStackedWidget(viewerDialog_);

    viewerImageScrollArea_ = new QScrollArea(viewerDialog_);
    viewerImageScrollArea_->setWidgetResizable(true);
    viewerImageLabel_ = new QLabel(viewerImageScrollArea_);
    viewerImageLabel_->setAlignment(Qt::AlignCenter);
    viewerImageScrollArea_->setWidget(viewerImageLabel_);

    viewerVideoWidget_ = new VideoFrameWidget(viewerDialog_);
    viewerVlcWidget_ = new VlcPlayerWidget(viewerDialog_);
    viewerWebVideoWidget_ = new WebVideoWidget(viewerDialog_);
    viewerFallbackLabel_ = new QLabel(viewerDialog_);
    viewerFallbackLabel_->setAlignment(Qt::AlignCenter);
    viewerFallbackLabel_->setWordWrap(true);

    viewerContentStack_->addWidget(viewerImageScrollArea_);
    viewerContentStack_->addWidget(viewerVideoWidget_);
    viewerContentStack_->addWidget(viewerVlcWidget_);
    viewerContentStack_->addWidget(viewerWebVideoWidget_);
    viewerContentStack_->addWidget(viewerFallbackLabel_);

    auto *closeButton = new QPushButton(QStringLiteral("Close"), viewerDialog_);
    auto *buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    buttonRow->addWidget(closeButton);

    layout->addWidget(viewerTitleLabel_);
    layout->addWidget(viewerStatusLabel_);
    layout->addWidget(viewerProgressBar_);
    layout->addWidget(viewerContentStack_, 1);
    layout->addLayout(buttonRow);

    viewerMediaPlayer_ = new QMediaPlayer(viewerDialog_);
    viewerAudioOutput_ = new QAudioOutput(viewerDialog_);
    viewerMediaPlayer_->setAudioOutput(viewerAudioOutput_);
    viewerMediaPlayer_->setVideoSink(viewerVideoWidget_->videoSink());
    connect(viewerMediaPlayer_, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &errorString) {
        if (viewerFallbackLabel_ != nullptr && !errorString.trimmed().isEmpty()) {
            if (controller_ != nullptr) {
                controller_->recordWarning(QStringLiteral("viewer"),
                                           QStringLiteral("Qt media playback failed: %1").arg(errorString.trimmed()));
            }
            viewerFallbackLabel_->setText(errorString);
            if (viewerContentStack_ != nullptr) {
                viewerContentStack_->setCurrentWidget(viewerFallbackLabel_);
            }
        }
    });
    connect(viewerVlcWidget_, &VlcPlayerWidget::playbackStarted, this, [this]() {
        if (controller_ != nullptr) {
            controller_->recordInfo(QStringLiteral("viewer"),
                                    QStringLiteral("Playing media with the built-in VLC viewer."));
        }
        if (viewerStatusLabel_ != nullptr) {
            viewerStatusLabel_->setText(QStringLiteral("Playing in the built-in viewer..."));
        }
    });
    connect(viewerVlcWidget_, &VlcPlayerWidget::playbackFailed, this, [this](const QString &error) {
        const QString trimmed = error.trimmed().isEmpty()
            ? QStringLiteral("The built-in VLC viewer could not play this media.")
            : error.trimmed();
        if (controller_ != nullptr) {
            controller_->recordWarning(QStringLiteral("viewer"),
                                       QStringLiteral("VLC playback failed: %1").arg(trimmed));
        }

        const ViewerSnapshot viewer = localViewerActive_
            ? localViewerSnapshot_
            : (controller_ != nullptr ? controller_->runtime().viewer : ViewerSnapshot {});
        const bool usedWebFallback = viewerWebVideoWidget_ != nullptr
            && viewerWebVideoWidget_->isAvailable()
            && shouldUseWebVideoFallback(viewer)
            && viewerWebVideoWidget_->loadMediaFile(viewer.localPath, viewer.mimeType);
        if (usedWebFallback) {
            if (viewerContentStack_ != nullptr) {
                viewerContentStack_->setCurrentWidget(viewerWebVideoWidget_);
            }
            if (viewerStatusLabel_ != nullptr) {
                viewerStatusLabel_->setText(QStringLiteral("Playing in the built-in viewer..."));
            }
            return;
        }

        if (viewerMediaPlayer_ != nullptr && !viewer.localPath.trimmed().isEmpty()) {
            viewerMediaPlayer_->setSource(QUrl::fromLocalFile(viewer.localPath));
            viewerMediaPlayer_->play();
            if (viewerContentStack_ != nullptr) {
                viewerContentStack_->setCurrentWidget(viewerVideoWidget_);
            }
            if (viewerStatusLabel_ != nullptr) {
                viewerStatusLabel_->setText(QStringLiteral("Playing in the built-in viewer..."));
            }
            return;
        }

        if (viewerFallbackLabel_ != nullptr) {
            viewerFallbackLabel_->setText(trimmed);
        }
        if (viewerContentStack_ != nullptr) {
            viewerContentStack_->setCurrentWidget(viewerFallbackLabel_);
        }
        if (viewerStatusLabel_ != nullptr) {
            viewerStatusLabel_->setText(QStringLiteral("Viewer error"));
        }
    });

    connect(closeButton, &QPushButton::clicked, this, [this]() {
        const ViewerSnapshot runtimeViewer = controller_->runtime().viewer;
        if (runtimeViewer.state != ViewerState::Idle && runtimeViewer.sessionId != 0) {
            viewerDismissedSessionId_ = runtimeViewer.sessionId;
        } else {
            localViewerActive_ = false;
            localViewerSnapshot_ = ViewerSnapshot {};
        }
        if (viewerMediaPlayer_ != nullptr) {
            viewerMediaPlayer_->stop();
        }
        if (viewerVideoWidget_ != nullptr) {
            viewerVideoWidget_->clearFrame();
        }
        if (viewerVlcWidget_ != nullptr) {
            viewerVlcWidget_->stopPlayback();
        }
        if (viewerWebVideoWidget_ != nullptr) {
            viewerWebVideoWidget_->clearMedia();
        }
        if (viewerDialog_ != nullptr) {
            viewerDialog_->hide();
        }
    });
    connect(viewerDialog_, &QDialog::finished, this, [this](int result) {
        Q_UNUSED(result);
        const ViewerSnapshot runtimeViewer = controller_->runtime().viewer;
        if (runtimeViewer.state != ViewerState::Idle && runtimeViewer.sessionId != 0) {
            viewerDismissedSessionId_ = runtimeViewer.sessionId;
        } else {
            localViewerActive_ = false;
            localViewerSnapshot_ = ViewerSnapshot {};
        }
        if (viewerMediaPlayer_ != nullptr) {
            viewerMediaPlayer_->stop();
        }
        if (viewerVideoWidget_ != nullptr) {
            viewerVideoWidget_->clearFrame();
        }
        if (viewerVlcWidget_ != nullptr) {
            viewerVlcWidget_->stopPlayback();
        }
        if (viewerWebVideoWidget_ != nullptr) {
            viewerWebVideoWidget_->clearMedia();
        }
    });
}

void MainWindow::loadViewerMedia(const ViewerSnapshot &viewer)
{
    if (viewerDialog_ == nullptr || viewerContentStack_ == nullptr) {
        return;
    }

    viewerLoadedSessionId_ = viewer.sessionId;
    viewerLoadedLocalPath_ = viewer.localPath;
    viewerLoadedState_ = viewer.state;
    if (viewerMediaPlayer_ != nullptr) {
        viewerMediaPlayer_->stop();
        viewerMediaPlayer_->setSource(QUrl());
    }
    if (viewerVideoWidget_ != nullptr) {
        viewerVideoWidget_->clearFrame();
    }
    if (viewerVlcWidget_ != nullptr) {
        viewerVlcWidget_->stopPlayback();
    }
    if (viewerWebVideoWidget_ != nullptr) {
        viewerWebVideoWidget_->clearMedia();
    }

    QScreen *targetScreen = windowHandle() != nullptr ? windowHandle()->screen() : QGuiApplication::primaryScreen();
    const QRect available = targetScreen != nullptr
        ? targetScreen->availableGeometry()
        : QRect(80, 80, 1200, 800);
    const QSize maxContentSize(qMax(320, available.width() - 140), qMax(220, available.height() - 220));
    QSize dialogSize(qMin(available.width() - 80, 1100), qMin(available.height() - 80, 860));

    if (viewer.state != ViewerState::Ready || viewer.localPath.isEmpty()) {
        viewerFallbackLabel_->setText(
            viewer.state == ViewerState::Error
                ? (viewer.error.isEmpty() ? QStringLiteral("Unable to open this media item.") : viewer.error)
                : QStringLiteral("Preparing media for the built-in viewer..."));
        viewerContentStack_->setCurrentWidget(viewerFallbackLabel_);
    } else if (viewer.category == MediaCategory::Images || viewer.mimeType.startsWith(QStringLiteral("image/"))) {
        const QImage image = loadAutoTransformedImageFromFile(viewer.localPath);
        const QPixmap pixmap = QPixmap::fromImage(image);
        if (pixmap.isNull()) {
            viewerFallbackLabel_->setText(QStringLiteral("The image could not be loaded."));
            viewerContentStack_->setCurrentWidget(viewerFallbackLabel_);
        } else {
            const QSize fitted = pixmap.size().boundedTo(maxContentSize);
            const QPixmap scaled = pixmap.size().width() > fitted.width() || pixmap.size().height() > fitted.height()
                ? pixmap.scaled(fitted, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                : pixmap;
            viewerImageLabel_->setPixmap(scaled);
            viewerImageLabel_->setMinimumSize(scaled.size());
            viewerImageLabel_->resize(scaled.size());
            viewerContentStack_->setCurrentWidget(viewerImageScrollArea_);
            dialogSize = QSize(
                qMin(available.width() - 80, scaled.width() + 80),
                qMin(available.height() - 80, scaled.height() + 170));
        }
    } else if (viewer.category == MediaCategory::Videos
               || viewer.category == MediaCategory::Audio
               || viewer.mimeType.startsWith(QStringLiteral("video/"))
               || viewer.mimeType.startsWith(QStringLiteral("audio/"))) {
        const bool isVideo = viewer.category == MediaCategory::Videos || viewer.mimeType.startsWith(QStringLiteral("video/"));
        const bool usingVlc = isVideo
            && viewerVlcWidget_ != nullptr
            && viewerVlcWidget_->isAvailable()
            && viewerVlcWidget_->playFile(viewer.localPath);
        const bool usingWebFallback = isVideo
            && !usingVlc
            && viewerWebVideoWidget_ != nullptr
            && viewerWebVideoWidget_->isAvailable()
            && shouldUseWebVideoFallback(viewer)
            && viewerWebVideoWidget_->loadMediaFile(viewer.localPath, viewer.mimeType);
        viewerContentStack_->setCurrentWidget(
            isVideo
                ? (usingVlc
                    ? static_cast<QWidget *>(viewerVlcWidget_)
                    : usingWebFallback
                    ? static_cast<QWidget *>(viewerWebVideoWidget_)
                    : static_cast<QWidget *>(viewerVideoWidget_))
                : static_cast<QWidget *>(viewerFallbackLabel_));
        if (viewer.category == MediaCategory::Audio || viewer.mimeType.startsWith(QStringLiteral("audio/"))) {
            viewerFallbackLabel_->setText(QStringLiteral("Playing audio in the built-in viewer..."));
        }
        if (!usingVlc && !usingWebFallback && viewerMediaPlayer_ != nullptr) {
            viewerMediaPlayer_->setSource(QUrl::fromLocalFile(viewer.localPath));
            viewerMediaPlayer_->play();
        } else if (isVideo && !usingVlc && !usingWebFallback && viewerVlcWidget_ != nullptr && !viewerVlcWidget_->lastError().trimmed().isEmpty()) {
            viewerFallbackLabel_->setText(viewerVlcWidget_->lastError());
        }
    } else {
        viewerFallbackLabel_->setText(
            QStringLiteral("This file type is downloaded, but the built-in viewer does not support displaying it yet.\n\n%1")
                .arg(QDir::toNativeSeparators(viewer.localPath)));
        viewerContentStack_->setCurrentWidget(viewerFallbackLabel_);
    }

    dialogSize.setWidth(qBound(420, dialogSize.width(), available.width()));
    dialogSize.setHeight(qBound(320, dialogSize.height(), available.height()));
    viewerDialog_->resize(dialogSize);
    QRect geometry = viewerDialog_->frameGeometry();
    geometry.setSize(dialogSize);
    geometry.moveCenter(available.center());
    if (geometry.left() < available.left()) {
        geometry.moveLeft(available.left());
    }
    if (geometry.top() < available.top()) {
        geometry.moveTop(available.top());
    }
    if (geometry.right() > available.right()) {
        geometry.moveRight(available.right());
    }
    if (geometry.bottom() > available.bottom()) {
        geometry.moveBottom(available.bottom());
    }
    viewerDialog_->setGeometry(geometry);
}

void MainWindow::openSelectedSharedItem()
{
    if (libraryList_ == nullptr || libraryList_->currentItem() == nullptr) {
        return;
    }

    const QListWidgetItem *item = libraryList_->currentItem();
    const QString filePath = item->data(Qt::UserRole + 1).toString();
    if (filePath.trimmed().isEmpty()) {
        controller_->recordWarning(QStringLiteral("shared-files"), QStringLiteral("The selected shared item has no available local file."));
        return;
    }

    openLocalViewerFile(
        filePath,
        item->data(Qt::UserRole + 2).toString(),
        item->data(Qt::UserRole + 3).toString(),
        static_cast<MediaCategory>(item->data(Qt::UserRole + 4).toInt()));
}

void MainWindow::deleteSelectedSharedItem()
{
    if (libraryList_ == nullptr || libraryList_->currentItem() == nullptr) {
        return;
    }

    const QString sha256 = libraryList_->currentItem()->data(Qt::UserRole).toString().trimmed();
    if (sha256.isEmpty()) {
        controller_->recordWarning(QStringLiteral("shared-files"), QStringLiteral("The selected shared item could not be identified for cleanup."));
        return;
    }

    controller_->deleteSharedItem(sha256);
}

QString MainWindow::sharedItemLocalPath(const SharedItemRecord &item) const
{
    const QStringList candidates = {
        item.sourcePath,
        item.libraryPath,
        item.archivePath,
    };

    for (const QString &candidate : candidates) {
        if (!candidate.trimmed().isEmpty() && QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    if (!item.bundlePath.trimmed().isEmpty()) {
        const QDir bundleDir(item.bundlePath);
        const QFileInfoList entries = bundleDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &entry : entries) {
            const QString lowerName = entry.fileName().toLower();
            if (lowerName == QStringLiteral("index.html") || lowerName == QStringLiteral("thumbnail.jpg")) {
                continue;
            }
            return entry.absoluteFilePath();
        }
    }

    return {};
}

QString MainWindow::sharedItemThumbnailPath(const SharedItemRecord &item) const
{
    if (!item.bundlePath.trimmed().isEmpty()) {
        const QString bundleThumbnail = QDir(item.bundlePath).filePath(QStringLiteral("thumbnail.jpg"));
        if (QFileInfo::exists(bundleThumbnail)) {
            return bundleThumbnail;
        }
    }

    const QString localPath = sharedItemLocalPath(item);
    if (!localPath.isEmpty()
        && (item.category == MediaCategory::Images || item.mimeType.startsWith(QStringLiteral("image/")))) {
        return localPath;
    }
    return {};
}

QString MainWindow::sharedItemOriginLabel(const SharedItemRecord &item) const
{
    const QString sourcePath = item.sourcePath.trimmed();
    const QString archivePath = item.archivePath.trimmed();
    const QString libraryPath = item.libraryPath.trimmed();
    const QString downloadsRoot = controller_->settings().destinationRootPath.trimmed();
    const QString sharedRoot = controller_->settings().libraryRootPath.trimmed();

    if (!archivePath.isEmpty() && sourcePath == archivePath) {
        return QStringLiteral("Archive");
    }
    if (!libraryPath.isEmpty() && sourcePath == libraryPath) {
        return QStringLiteral("Shared");
    }
    if (!downloadsRoot.isEmpty() && sourcePath.startsWith(downloadsRoot, Qt::CaseInsensitive)) {
        return QStringLiteral("Downloads");
    }
    if (!sharedRoot.isEmpty() && sourcePath.startsWith(sharedRoot, Qt::CaseInsensitive)) {
        return QStringLiteral("Shared");
    }
    if (!archivePath.isEmpty()) {
        return QStringLiteral("Archive");
    }
    return QStringLiteral("Shared");
}

QString MainWindow::sharedItemSignature(const QVector<SharedItemRecord> &items) const
{
    QStringList tokens;
    tokens.reserve(items.size());
    for (const SharedItemRecord &item : items) {
        tokens.append(QStringLiteral("%1|%2|%3")
                          .arg(item.sha256,
                               item.updatedAt.toString(Qt::ISODateWithMs),
                               item.sourcePath));
    }
    return tokens.join(QChar(0x1f));
}

QIcon MainWindow::sharedItemIcon(const SharedItemRecord &item)
{
    const QString cacheKey = QStringLiteral("%1|%2|%3")
        .arg(item.sha256, item.updatedAt.toString(Qt::ISODateWithMs), sharedItemLocalPath(item));
    const auto cached = sharedItemIconCache_.constFind(cacheKey);
    if (cached != sharedItemIconCache_.cend()) {
        return cached.value();
    }

    QIcon icon;
    const QString previewPath = sharedItemThumbnailPath(item);
    if (!previewPath.isEmpty()) {
        const QImage image = loadAutoTransformedImageFromFile(previewPath);
        const QPixmap preview = QPixmap::fromImage(image);
        if (!preview.isNull()) {
            icon = QIcon(renderDiscoveryTile(
                QSize(kDiscoveryTileWidth, kDiscoveryTileHeight),
                item.originalFilename.isEmpty() ? item.sha256.left(16) : item.originalFilename,
                QString(),
                categoryAccent(item.category),
                &preview,
                item.category == MediaCategory::Videos || item.mimeType.startsWith(QStringLiteral("video/"))));
        }
    }

    if (icon.isNull()) {
        icon = QIcon(renderDiscoveryTile(
            QSize(kDiscoveryTileWidth, kDiscoveryTileHeight),
            item.originalFilename.isEmpty() ? item.sha256.left(16) : item.originalFilename,
            QString(),
            categoryAccent(item.category),
            nullptr,
            item.category == MediaCategory::Videos || item.mimeType.startsWith(QStringLiteral("video/"))));
    }

    sharedItemIconCache_.insert(cacheKey, icon);
    return icon;
}

void MainWindow::openLocalViewerFile(
    const QString &filePath,
    const QString &displayName,
    const QString &mimeType,
    const MediaCategory category)
{
    ViewerSnapshot snapshot;
    snapshot.sessionId = nextLocalViewerSessionId_++;
    snapshot.fileName = displayName.isEmpty() ? QFileInfo(filePath).fileName() : displayName;
    snapshot.mimeType = mimeType;
    snapshot.category = category;
    snapshot.localPath = filePath;

    if (QFileInfo::exists(filePath)) {
        snapshot.state = ViewerState::Ready;
        snapshot.totalBytes = QFileInfo(filePath).size();
        snapshot.receivedBytes = snapshot.totalBytes;
    } else {
        snapshot.state = ViewerState::Error;
        snapshot.error = QStringLiteral("The file is no longer available at %1").arg(QDir::toNativeSeparators(filePath));
    }

    localViewerSnapshot_ = snapshot;
    localViewerActive_ = true;
    refreshViewerDialog();
}

AppSettings MainWindow::gatherSettingsFromUi() const
{
    AppSettings settings = controller_->settings();
    settings.homeserverUrl = homeserverEdit_->text().trimmed();
    settings.username = usernameEdit_->text().trimmed();
    settings.ownerUserId.clear();
    settings.destinationRootPath = destinationEdit_->text().trimmed();
    settings.libraryRootPath = libraryEdit_->text().trimmed();
    settings.flatFolderLayout = flatFolderLayoutCheck_->isChecked();
    settings.archiveRootPath = archiveEdit_->text().trimmed();
    settings.archiveScanEnabled = archiveScanEnabledCheck_->isChecked();
    settings.archiveScanHighPriority = archiveHighPriorityCheck_->isChecked();
    settings.selfHealEnabled = selfHealCheck_->isChecked();
    settings.manualDownloadRootPath = settings.destinationRootPath;
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
    settings.darkModeEnabled = darkModeCheck_->isChecked();
    settings.autoJoinSpaceRooms = autoJoinSpacesCheck_->isChecked();
    settings.autoDownloadNewMedia = autoDownloadCheck_->isChecked();
    return settings;
}

const ActiveDownloadSnapshot *MainWindow::activeDownloadForDiscovery(const AttachmentDiscovery &discovery) const
{
    const QVector<ActiveDownloadSnapshot> &activeDownloads = controller_->runtime().activeDownloads;
    for (const ActiveDownloadSnapshot &active : activeDownloads) {
        if (active.roomId == discovery.roomId && active.eventId == discovery.eventId) {
            return &active;
        }
    }
    return nullptr;
}

const DownloadJobRecord *MainWindow::jobForDiscovery(const AttachmentDiscovery &discovery) const
{
    const QVector<DownloadJobRecord> &jobs = controller_->jobs();
    for (const DownloadJobRecord &job : jobs) {
        if (job.roomId == discovery.roomId && job.eventId == discovery.eventId) {
            return &job;
        }
    }
    return nullptr;
}

void MainWindow::applyDiscoveryPresentation(QListWidgetItem *item, const AttachmentDiscovery &discovery)
{
    if (item == nullptr) {
        return;
    }

    const QString title = discovery.originalFilename.isEmpty() ? discovery.eventId : discovery.originalFilename;
    const QString tileLabel = QStringLiteral("%1 | %2")
                                  .arg(mediaCategoryTitle(discovery.category).toUpper(),
                                       discoverySourceKindTitle(discovery).toUpper());
    const QString sourceUrl = discovery.directUrl.isEmpty() ? discovery.mxcUrl : discovery.directUrl;
    const QString cacheKey = browserThumbnailKey(discovery);
    const ActiveDownloadSnapshot *activeDownload = activeDownloadForDiscovery(discovery);
    const DownloadJobRecord *job = jobForDiscovery(discovery);
    QString secondaryText;
    double progressFraction = -1.0;
    QString progressText;

    if (activeDownload != nullptr) {
        if (activeDownload->totalBytes > 0) {
            progressFraction = qBound(
                0.0,
                static_cast<double>(activeDownload->receivedBytes) / static_cast<double>(activeDownload->totalBytes),
                1.0);
            progressText = transferProgressText(activeDownload->receivedBytes, activeDownload->totalBytes);
        } else {
            progressFraction = 0.2;
            progressText = transferProgressText(activeDownload->receivedBytes, activeDownload->totalBytes);
        }
        secondaryText = activeDownload->workerId == 0
            ? QStringLiteral("Opening %1").arg(progressText.isEmpty() ? QStringLiteral("...") : progressText)
            : QStringLiteral("Downloading %1").arg(progressText.isEmpty() ? QStringLiteral("...") : progressText);
    } else if (job != nullptr) {
        switch (job->state) {
        case DownloadJobState::Queued:
            secondaryText = QStringLiteral("Queued");
            progressFraction = 0.0;
            break;
        case DownloadJobState::CoolingDown:
            secondaryText = QStringLiteral("Retrying Soon");
            progressFraction = 0.0;
            break;
        case DownloadJobState::UndecryptablePending:
            secondaryText = QStringLiteral("Pending");
            progressFraction = 0.0;
            break;
        case DownloadJobState::FailedPermanent:
            secondaryText = QStringLiteral("Failed");
            break;
        case DownloadJobState::Completed:
        case DownloadJobState::DuplicateCompleted:
            secondaryText = QStringLiteral("Downloaded");
            break;
        case DownloadJobState::Downloading:
            progressText = transferProgressText(job->receivedBytes, job->totalBytes);
            secondaryText = progressText.isEmpty()
                ? QStringLiteral("Downloading")
                : QStringLiteral("Downloading %1").arg(progressText);
            progressFraction = job->totalBytes > 0
                ? qBound(
                      0.0,
                      static_cast<double>(job->receivedBytes) / static_cast<double>(job->totalBytes),
                      1.0)
                : 0.2;
            break;
        }
    }

    item->setText(secondaryText.isEmpty() ? tileLabel : QStringLiteral("%1\n%2").arg(tileLabel, secondaryText));
    item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
    item->setToolTip(QStringLiteral("%1\n%2\n%3").arg(title, discovery.roomId, sourceUrl));
    item->setSizeHint(QSize(kDiscoveryGridWidth, kDiscoveryGridHeight));
    item->setData(Qt::UserRole + 2, cacheKey);

    QIcon baseIcon;
    const auto cachedIcon = browserThumbnailIconCache_.constFind(cacheKey);
    if (cachedIcon != browserThumbnailIconCache_.cend()) {
        baseIcon = *cachedIcon;
    } else {
        baseIcon = placeholderDiscoveryIcon(discovery);
        if (cacheKey.startsWith(QStringLiteral("placeholder:"))) {
            cacheBrowserThumbnailIcon(cacheKey, baseIcon);
        }
    }

    item->setIcon(
        (progressFraction >= 0.0 || !secondaryText.isEmpty())
            ? overlayDiscoveryStatusIcon(
                  baseIcon,
                  QSize(kDiscoveryTileWidth, kDiscoveryTileHeight),
                  categoryAccent(discovery.category),
                  progressFraction,
                  secondaryText)
            : baseIcon);
}

void MainWindow::resetBrowserPageState()
{
    browserLoadedRoomId_.clear();
    browserLoadedOffset_ = 0;
    browserLoadedDiscoveries_.clear();
    browserTotalDiscoveryCount_ = 0;
    browserLoadingPage_ = false;
    browserBackgroundThumbnailPrefetchScheduled_ = false;
    clearBrowserThumbnailRequests();
    if (discoveriesList_ == nullptr) {
        return;
    }

    const QSignalBlocker blocker(discoveriesList_);
    discoveriesList_->clear();
    discoveriesList_->verticalScrollBar()->setValue(0);
}

void MainWindow::loadMoreBrowserDiscoveries(const bool reset)
{
    if (discoveriesList_ == nullptr || browserLoadingPage_) {
        return;
    }

    const QString roomId = selectedBrowserRoomId();
    if (roomId.isEmpty()) {
        return;
    }

    browserLoadingPage_ = true;

    const QString selectedEventId = discoveriesList_->currentItem() != nullptr
        ? discoveriesList_->currentItem()->data(Qt::UserRole + 1).toString()
        : QString();

    if (reset) {
        resetBrowserPageState();
        browserLoadedRoomId_ = roomId;
        browserTotalDiscoveryCount_ = controller_->discoveryCount(roomId);
        browserLoadingPage_ = true;
    }

    const QVector<AttachmentDiscovery> page = controller_->fetchDiscoveriesPage(
        roomId,
        browserLoadedOffset_ + browserLoadedDiscoveries_.size(),
        kBrowserPageSize);

    QListWidgetItem *restoredSelection = nullptr;
    discoveriesList_->setUpdatesEnabled(false);
    for (const AttachmentDiscovery &discovery : page) {
        browserLoadedDiscoveries_.append(discovery);
        auto *item = new QListWidgetItem(discoveriesList_);
        applyDiscoveryPresentation(item, discovery);
        item->setData(Qt::UserRole, discovery.roomId);
        item->setData(Qt::UserRole + 1, discovery.eventId);
        if (!selectedEventId.isEmpty() && discovery.eventId == selectedEventId) {
            restoredSelection = item;
        }
    }
    discoveriesList_->setUpdatesEnabled(true);

    if (restoredSelection != nullptr) {
        discoveriesList_->setCurrentItem(restoredSelection);
    }

    browserLoadingPage_ = false;
    requestVisibleBrowserThumbnails();
    scheduleBackgroundBrowserThumbnailPrefetch();
    openDiscoveryButton_->setEnabled(discoveriesList_->currentItem() != nullptr);
    downloadDiscoveryButton_->setEnabled(discoveriesList_->currentItem() != nullptr);

    if (!page.isEmpty() && browserLoadedDiscoveries_.size() < browserTotalDiscoveryCount_) {
        QTimer::singleShot(0, this, [this]() {
            maybeLoadMoreBrowserDiscoveries();
        });
    }
}

void MainWindow::maybeLoadMoreBrowserDiscoveries()
{
    if (discoveriesList_ == nullptr
        || browserLoadingPage_
        || browserTotalDiscoveryCount_ <= 0
        || browserLoadedDiscoveries_.isEmpty()) {
        return;
    }

    QScrollBar *scrollBar = discoveriesList_->verticalScrollBar();
    if (scrollBar == nullptr) {
        return;
    }

    int firstVisibleIndex = -1;
    int lastVisibleIndex = -1;
    const QRect viewportRect = discoveriesList_->viewport()->rect();
    for (int index = 0; index < discoveriesList_->count(); ++index) {
        QListWidgetItem *item = discoveriesList_->item(index);
        if (item != nullptr && discoveriesList_->visualItemRect(item).intersects(viewportRect)) {
            if (firstVisibleIndex < 0) {
                firstVisibleIndex = index;
            }
            lastVisibleIndex = index;
        }
    }
    if (firstVisibleIndex < 0) {
        firstVisibleIndex = 0;
    }
    if (lastVisibleIndex < 0) {
        lastVisibleIndex = qMin(discoveriesList_->count() - 1, kBrowserPageSize - 1);
    }

    const int visibleColumns = qMax(1, discoveriesList_->viewport()->width() / qMax(1, discoveriesList_->gridSize().width()));
    const int gridHeight = qMax(1, discoveriesList_->gridSize().height());

    if (browserLoadedOffset_ > 0 && firstVisibleIndex <= (kBrowserPageSize * kBrowserBufferedPages)) {
        const int prependCount = qMin(kBrowserPageSize, browserLoadedOffset_);
        const int newOffset = browserLoadedOffset_ - prependCount;
        const QVector<AttachmentDiscovery> page = controller_->fetchDiscoveriesPage(
            browserLoadedRoomId_,
            newOffset,
            prependCount);
        if (!page.isEmpty()) {
            discoveriesList_->setUpdatesEnabled(false);
            for (int index = page.size() - 1; index >= 0; --index) {
                const AttachmentDiscovery &discovery = page.at(index);
                browserLoadedDiscoveries_.prepend(discovery);
                auto *item = new QListWidgetItem();
                applyDiscoveryPresentation(item, discovery);
                item->setData(Qt::UserRole, discovery.roomId);
                item->setData(Qt::UserRole + 1, discovery.eventId);
                discoveriesList_->insertItem(0, item);
            }
            discoveriesList_->setUpdatesEnabled(true);
            browserLoadedOffset_ = newOffset;
            const int addedRows = (page.size() + visibleColumns - 1) / visibleColumns;
            scrollBar->setValue(scrollBar->value() + addedRows * gridHeight);
            requestVisibleBrowserThumbnails();
            scheduleBackgroundBrowserThumbnailPrefetch();
        }
    }

    const int bufferedAhead = browserLoadedDiscoveries_.size() - (lastVisibleIndex + 1);
    if ((browserLoadedOffset_ + browserLoadedDiscoveries_.size()) < browserTotalDiscoveryCount_
        && (scrollBar->maximum() <= 0 || bufferedAhead <= (kBrowserPageSize * kBrowserBufferedPages))) {
        loadMoreBrowserDiscoveries(false);
        return;
    }

    trimBrowserDiscoveryWindow();
}

void MainWindow::trimBrowserDiscoveryWindow()
{
    if (discoveriesList_ == nullptr) {
        return;
    }

    const int maxItems = kBrowserPageSize * kBrowserMaxWindowPages;
    if (browserLoadedDiscoveries_.size() <= maxItems) {
        return;
    }

    QScrollBar *scrollBar = discoveriesList_->verticalScrollBar();
    if (scrollBar == nullptr) {
        return;
    }

    int firstVisibleIndex = discoveriesList_->indexAt(QPoint(8, 8)).row();
    if (firstVisibleIndex < 0) {
        firstVisibleIndex = 0;
    }
    int lastVisibleIndex = firstVisibleIndex;
    const QRect viewportRect = discoveriesList_->viewport()->rect();
    for (int index = firstVisibleIndex; index < discoveriesList_->count(); ++index) {
        QListWidgetItem *item = discoveriesList_->item(index);
        if (item == nullptr || !discoveriesList_->visualItemRect(item).intersects(viewportRect)) {
            continue;
        }
        lastVisibleIndex = index;
    }

    const int visibleColumns = qMax(1, discoveriesList_->viewport()->width() / qMax(1, discoveriesList_->gridSize().width()));
    const int gridHeight = qMax(1, discoveriesList_->gridSize().height());
    while (browserLoadedDiscoveries_.size() > maxItems
           && firstVisibleIndex > (kBrowserPageSize * (kBrowserBufferedPages + 1))) {
        const int removeCount = qMin(kBrowserPageSize, browserLoadedDiscoveries_.size() - maxItems);
        browserLoadedDiscoveries_.remove(0, removeCount);
        for (int row = 0; row < removeCount; ++row) {
            delete discoveriesList_->takeItem(0);
        }
        browserLoadedOffset_ += removeCount;
        firstVisibleIndex -= removeCount;
        lastVisibleIndex -= removeCount;
        const int removedRows = (removeCount + visibleColumns - 1) / visibleColumns;
        scrollBar->setValue(qMax(0, scrollBar->value() - removedRows * gridHeight));
    }

    while (browserLoadedDiscoveries_.size() > maxItems
           && (browserLoadedDiscoveries_.size() - (lastVisibleIndex + 1))
               > (kBrowserPageSize * (kBrowserBufferedPages + 1))) {
        const int removeCount = qMin(kBrowserPageSize, browserLoadedDiscoveries_.size() - maxItems);
        const int startIndex = browserLoadedDiscoveries_.size() - removeCount;
        browserLoadedDiscoveries_.remove(startIndex, removeCount);
        for (int row = 0; row < removeCount; ++row) {
            delete discoveriesList_->takeItem(discoveriesList_->count() - 1);
        }
    }
}

QString MainWindow::currentBrowserSelectedEventId() const
{
    if (discoveriesList_ == nullptr || discoveriesList_->currentItem() == nullptr) {
        return QString();
    }
    return discoveriesList_->currentItem()->data(Qt::UserRole + 1).toString();
}

void MainWindow::restoreBrowserSelectionByEventId(const QString &eventId)
{
    if (discoveriesList_ == nullptr || eventId.isEmpty()) {
        return;
    }
    for (int index = 0; index < discoveriesList_->count(); ++index) {
        if (QListWidgetItem *item = discoveriesList_->item(index)) {
            if (item->data(Qt::UserRole + 1).toString() == eventId) {
                if (discoveriesList_->currentItem() != item) {
                    discoveriesList_->setCurrentItem(item);
                }
                return;
            }
        }
    }
}

void MainWindow::syncBrowserLoadedDiscoveries()
{
    if (discoveriesList_ == nullptr
        || browserLoadedDiscoveries_.isEmpty()
        || browserLoadedRoomId_.isEmpty()) {
        return;
    }

    const QVector<AttachmentDiscovery> refreshed = controller_->fetchDiscoveriesPage(
        browserLoadedRoomId_,
        browserLoadedOffset_,
        browserLoadedDiscoveries_.size());
    if (refreshed.size() != browserLoadedDiscoveries_.size()) {
        return;
    }

    auto discoveryVisualSignature = [this](const AttachmentDiscovery &discovery) {
        return QStringList {
            discovery.roomId,
            discovery.eventId,
            browserThumbnailKey(discovery),
            discovery.thumbnailSourceUrl,
            discovery.thumbnailCachedPath,
            discovery.directUrl,
            discovery.mxcUrl,
            discovery.originalFilename,
            discovery.mimeType,
            QString::number(static_cast<int>(discovery.sourceKind)),
            QString::number(static_cast<int>(discovery.category)),
        }.join(QChar(0x1f));
    };

    bool structureChanged = false;
    for (qsizetype index = 0; index < refreshed.size(); ++index) {
        if (refreshed.at(index).roomId != browserLoadedDiscoveries_.at(index).roomId
            || refreshed.at(index).eventId != browserLoadedDiscoveries_.at(index).eventId) {
            structureChanged = true;
            break;
        }
    }

    const QString selectedEventId = currentBrowserSelectedEventId();
    const int scrollValue = discoveriesList_->verticalScrollBar() != nullptr
        ? discoveriesList_->verticalScrollBar()->value()
        : 0;
    const QSignalBlocker blocker(discoveriesList_);

    if (structureChanged) {
        discoveriesList_->setUpdatesEnabled(false);
        discoveriesList_->clear();
        browserLoadedDiscoveries_ = refreshed;
        QListWidgetItem *restoredSelection = nullptr;
        for (const AttachmentDiscovery &discovery : refreshed) {
            primeBrowserThumbnailIconCache(discovery);
            auto *item = new QListWidgetItem(discoveriesList_);
            applyDiscoveryPresentation(item, discovery);
            item->setData(Qt::UserRole, discovery.roomId);
            item->setData(Qt::UserRole + 1, discovery.eventId);
            if (!selectedEventId.isEmpty() && discovery.eventId == selectedEventId) {
                restoredSelection = item;
            }
        }
        discoveriesList_->setUpdatesEnabled(true);
        if (restoredSelection != nullptr) {
            discoveriesList_->setCurrentItem(restoredSelection);
        } else {
            restoreBrowserSelectionByEventId(selectedEventId);
        }
        if (discoveriesList_->verticalScrollBar() != nullptr) {
            discoveriesList_->verticalScrollBar()->setValue(scrollValue);
        }
        return;
    }

    QListWidgetItem *restoredSelection = nullptr;
    for (qsizetype index = 0; index < refreshed.size() && index < discoveriesList_->count(); ++index) {
        const AttachmentDiscovery &updatedDiscovery = refreshed.at(index);
        if (discoveryVisualSignature(updatedDiscovery) == discoveryVisualSignature(browserLoadedDiscoveries_.at(index))) {
            browserLoadedDiscoveries_[index] = updatedDiscovery;
            if (!selectedEventId.isEmpty() && updatedDiscovery.eventId == selectedEventId) {
                restoredSelection = discoveriesList_->item(static_cast<int>(index));
            }
            continue;
        }

        browserLoadedDiscoveries_[index] = updatedDiscovery;
        primeBrowserThumbnailIconCache(updatedDiscovery);
        if (QListWidgetItem *item = discoveriesList_->item(static_cast<int>(index))) {
            applyDiscoveryPresentation(item, updatedDiscovery);
            if (!selectedEventId.isEmpty() && updatedDiscovery.eventId == selectedEventId) {
                restoredSelection = item;
            }
        }
    }
    if (restoredSelection != nullptr && discoveriesList_->currentItem() != restoredSelection) {
        discoveriesList_->setCurrentItem(restoredSelection);
    } else if (!selectedEventId.isEmpty()) {
        restoreBrowserSelectionByEventId(selectedEventId);
    }
}

void MainWindow::requestVisibleBrowserThumbnails()
{
    if (discoveriesList_ == nullptr || browserLoadedDiscoveries_.isEmpty()) {
        return;
    }

    const QSet<QString> visibleKeys = currentVisibleBrowserThumbnailKeys();
    const QRect viewportRect = discoveriesList_->viewport()->rect();
    for (int index = 0; index < browserLoadedDiscoveries_.size() && index < discoveriesList_->count(); ++index) {
        QListWidgetItem *item = discoveriesList_->item(index);
        if (item == nullptr || !discoveriesList_->visualItemRect(item).intersects(viewportRect)) {
            continue;
        }
        enqueueBrowserThumbnailRequest(browserLoadedDiscoveries_.at(index), true);
    }
    reprioritizeBrowserThumbnailRequests(visibleKeys);
    pumpBrowserThumbnailRequests();
}

void MainWindow::scheduleBackgroundBrowserThumbnailPrefetch()
{
    if (browserBackgroundThumbnailPrefetchScheduled_ || discoveriesList_ == nullptr) {
        return;
    }

    browserBackgroundThumbnailPrefetchScheduled_ = true;
    QTimer::singleShot(180, this, [this]() {
        browserBackgroundThumbnailPrefetchScheduled_ = false;
        if (discoveriesList_ == nullptr) {
            return;
        }
        if (!browserThumbnailForegroundOrder_.isEmpty() || !browserThumbnailForegroundRequestsInFlight_.isEmpty()) {
            scheduleBackgroundBrowserThumbnailPrefetch();
            return;
        }

        const QRect viewportRect = discoveriesList_->viewport()->rect();
        int firstVisibleIndex = -1;
        int lastVisibleIndex = -1;
        for (int index = 0; index < browserLoadedDiscoveries_.size() && index < discoveriesList_->count(); ++index) {
            QListWidgetItem *item = discoveriesList_->item(index);
            if (item == nullptr || !discoveriesList_->visualItemRect(item).intersects(viewportRect)) {
                continue;
            }
            if (firstVisibleIndex < 0) {
                firstVisibleIndex = index;
            }
            lastVisibleIndex = index;
        }

        if (firstVisibleIndex < 0 || lastVisibleIndex < 0) {
            return;
        }

        int queued = 0;
        const int maxDistance = qMax(firstVisibleIndex, browserLoadedDiscoveries_.size() - lastVisibleIndex - 1);
        for (int distance = 1; distance <= maxDistance && queued < kBrowserBackgroundThumbnailBatchSize; ++distance) {
            const int afterIndex = lastVisibleIndex + distance;
            if (afterIndex >= 0 && afterIndex < browserLoadedDiscoveries_.size()) {
                enqueueBrowserThumbnailRequest(browserLoadedDiscoveries_.at(afterIndex), false);
                ++queued;
                if (queued >= kBrowserBackgroundThumbnailBatchSize) {
                    break;
                }
            }

            const int beforeIndex = firstVisibleIndex - distance;
            if (beforeIndex >= 0 && beforeIndex < browserLoadedDiscoveries_.size()) {
                enqueueBrowserThumbnailRequest(browserLoadedDiscoveries_.at(beforeIndex), false);
                ++queued;
            }
        }

        pumpBrowserThumbnailRequests();

        if (!browserThumbnailForegroundOrder_.isEmpty()
            || !browserThumbnailForegroundRequestsInFlight_.isEmpty()
            || !browserThumbnailBackgroundOrder_.isEmpty()) {
            scheduleBackgroundBrowserThumbnailPrefetch();
        }
    });
}

QString MainWindow::browserThumbnailKey(const AttachmentDiscovery &discovery) const
{
    const QString imageUrl = browserThumbnailUrl(discovery);
    if (!imageUrl.isEmpty()) {
        return imageUrl;
    }
    return QStringLiteral("placeholder:%1:%2").arg(discovery.roomId, discovery.eventId);
}

QString MainWindow::browserThumbnailUrl(const AttachmentDiscovery &discovery) const
{
    const auto isHttpSource = [](const QString &value) {
        const QUrl url(value);
        return url.isValid() && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"));
    };
    const auto isLocalFile = [](const QString &value) {
        return !value.trimmed().isEmpty() && QFileInfo::exists(value);
    };

    if (isLocalFile(discovery.thumbnailCachedPath)) {
        return QUrl::fromLocalFile(discovery.thumbnailCachedPath).toString();
    }

    if (discovery.sourceKind == MediaSourceKind::Matrix
        && (discovery.category == MediaCategory::Images || discovery.category == MediaCategory::Videos)) {
        const QString matrixSource = discovery.thumbnailSourceUrl.isEmpty()
            ? discovery.mxcUrl
            : discovery.thumbnailSourceUrl;
        const QString url = matrixThumbnailUrl(controller_->settings().homeserverUrl, matrixSource, 384);
        if (!url.isEmpty()) {
            return url;
        }
    }

    if (discovery.category == MediaCategory::Images
        && isHttpSource(!discovery.thumbnailSourceUrl.isEmpty() ? discovery.thumbnailSourceUrl : discovery.directUrl)) {
        return !discovery.thumbnailSourceUrl.isEmpty() ? discovery.thumbnailSourceUrl : discovery.directUrl;
    }

    return {};
}

QIcon MainWindow::placeholderDiscoveryIcon(const AttachmentDiscovery &discovery) const
{
    const QString title = discovery.originalFilename.isEmpty() ? discovery.eventId : discovery.originalFilename;
    const QString subtitle = QStringLiteral("%1 | %2")
                                 .arg(mediaCategoryTitle(discovery.category), mediaSourceKindTitle(discovery.sourceKind));
    const QPixmap tile = renderDiscoveryTile(
        QSize(kDiscoveryTileWidth, kDiscoveryTileHeight),
        title,
        subtitle,
        categoryAccent(discovery.category),
        nullptr,
        discovery.category == MediaCategory::Videos);
    return QIcon(tile);
}

bool MainWindow::primeBrowserThumbnailIconCache(const AttachmentDiscovery &discovery)
{
    const QString cacheKey = browserThumbnailKey(discovery);
    if (cacheKey.isEmpty()
        || cacheKey.startsWith(QStringLiteral("placeholder:"))
        || browserThumbnailIconCache_.contains(cacheKey)
        || discovery.thumbnailCachedPath.trimmed().isEmpty()
        || !QFileInfo::exists(discovery.thumbnailCachedPath)) {
        return false;
    }

    const QImage image = loadAutoTransformedImageFromFile(discovery.thumbnailCachedPath);
    const QPixmap preview = QPixmap::fromImage(image);
    if (preview.isNull()) {
        return false;
    }

    const QString title = discovery.originalFilename.isEmpty() ? discovery.eventId : discovery.originalFilename;
    const QString subtitle = QStringLiteral("%1 | %2")
                                 .arg(mediaCategoryTitle(discovery.category), mediaSourceKindTitle(discovery.sourceKind));
    const QPixmap tile = renderDiscoveryTile(
        QSize(kDiscoveryTileWidth, kDiscoveryTileHeight),
        title,
        subtitle,
        categoryAccent(discovery.category),
        &preview,
        discovery.category == MediaCategory::Videos);
    cacheBrowserThumbnailIcon(cacheKey, QIcon(tile));
    return true;
}

QSet<QString> MainWindow::currentVisibleBrowserThumbnailKeys() const
{
    QSet<QString> visibleKeys;
    if (discoveriesList_ == nullptr) {
        return visibleKeys;
    }

    const QRect viewportRect = discoveriesList_->viewport()->rect();
    for (int index = 0; index < browserLoadedDiscoveries_.size() && index < discoveriesList_->count(); ++index) {
        QListWidgetItem *item = discoveriesList_->item(index);
        if (item == nullptr || !discoveriesList_->visualItemRect(item).intersects(viewportRect)) {
            continue;
        }
        visibleKeys.insert(browserThumbnailKey(browserLoadedDiscoveries_.at(index)));
    }

    return visibleKeys;
}

void MainWindow::clearBrowserThumbnailRequests()
{
    browserThumbnailForegroundPending_.clear();
    browserThumbnailForegroundOrder_.clear();
    browserThumbnailBackgroundPending_.clear();
    browserThumbnailBackgroundOrder_.clear();
    browserThumbnailRequestsInFlight_.clear();
    browserThumbnailForegroundRequestsInFlight_.clear();

    const auto replies = browserThumbnailReplies_;
    browserThumbnailReplies_.clear();
    for (auto it = replies.cbegin(); it != replies.cend(); ++it) {
        if (it.value() != nullptr) {
            it.value()->abort();
            it.value()->deleteLater();
        }
    }
}

void MainWindow::enqueueBrowserThumbnailRequest(const AttachmentDiscovery &discovery, const bool highPriority)
{
    const QString cacheKey = browserThumbnailKey(discovery);
    if (cacheKey.startsWith(QStringLiteral("placeholder:")) || browserThumbnailIconCache_.contains(cacheKey)) {
        return;
    }

    const bool inFlight = browserThumbnailRequestsInFlight_.contains(cacheKey);
    const bool foregroundInFlight = browserThumbnailForegroundRequestsInFlight_.contains(cacheKey);

    if (highPriority) {
        browserThumbnailBackgroundPending_.remove(cacheKey);
        browserThumbnailBackgroundOrder_.removeAll(cacheKey);

        if (!browserThumbnailForegroundPending_.contains(cacheKey)) {
            browserThumbnailForegroundPending_.insert(cacheKey, discovery);
        }
        browserThumbnailForegroundOrder_.removeAll(cacheKey);
        browserThumbnailForegroundOrder_.prepend(cacheKey);

        if (inFlight && !foregroundInFlight) {
            if (QNetworkReply *reply = browserThumbnailReplies_.value(cacheKey, nullptr)) {
                reply->abort();
            }
        }
        return;
    }

    if (inFlight || browserThumbnailForegroundPending_.contains(cacheKey)) {
        return;
    }

    if (browserThumbnailBackgroundPending_.contains(cacheKey)) {
        browserThumbnailBackgroundOrder_.removeAll(cacheKey);
        browserThumbnailBackgroundOrder_.append(cacheKey);
        return;
    }

    browserThumbnailBackgroundPending_.insert(cacheKey, discovery);
    browserThumbnailBackgroundOrder_.append(cacheKey);
}

void MainWindow::reprioritizeBrowserThumbnailRequests(const QSet<QString> &visibleKeys)
{
    Q_UNUSED(visibleKeys);
    if (browserThumbnailForegroundOrder_.isEmpty() || browserThumbnailReplies_.isEmpty()) {
        return;
    }

    for (auto it = browserThumbnailReplies_.cbegin(); it != browserThumbnailReplies_.cend(); ++it) {
        if (browserThumbnailForegroundRequestsInFlight_.contains(it.key()) || it.value() == nullptr) {
            continue;
        }
        it.value()->abort();
    }
}

void MainWindow::pumpBrowserThumbnailRequests()
{
    while (browserThumbnailReplies_.size() < kBrowserThumbnailConcurrentRequests) {
        if (!browserThumbnailForegroundOrder_.isEmpty()) {
            const QString cacheKey = browserThumbnailForegroundOrder_.takeFirst();
            const auto it = browserThumbnailForegroundPending_.find(cacheKey);
            if (it == browserThumbnailForegroundPending_.end()) {
                continue;
            }

            const AttachmentDiscovery discovery = it.value();
            browserThumbnailForegroundPending_.erase(it);
            requestBrowserThumbnail(discovery, true);
            continue;
        }

        if (!browserThumbnailForegroundRequestsInFlight_.isEmpty()) {
            return;
        }

        if (browserThumbnailBackgroundOrder_.isEmpty()) {
            return;
        }

        const QString cacheKey = browserThumbnailBackgroundOrder_.takeFirst();
        const auto it = browserThumbnailBackgroundPending_.find(cacheKey);
        if (it == browserThumbnailBackgroundPending_.end()) {
            continue;
        }

        const AttachmentDiscovery discovery = it.value();
        browserThumbnailBackgroundPending_.erase(it);
        requestBrowserThumbnail(discovery, false);
    }
}

void MainWindow::requestBrowserThumbnail(const AttachmentDiscovery &discovery, const bool foreground)
{
    if (thumbnailNetworkManager_ == nullptr) {
        return;
    }

    const QString cacheKey = browserThumbnailKey(discovery);
    if (cacheKey.startsWith(QStringLiteral("placeholder:"))
        || browserThumbnailRequestsInFlight_.contains(cacheKey)
        || browserThumbnailIconCache_.contains(cacheKey)) {
        return;
    }

    const QString url = browserThumbnailUrl(discovery);
    if (url.isEmpty()) {
        return;
    }

    QNetworkRequest request {QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    browserThumbnailRequestsInFlight_.insert(cacheKey);
    if (foreground) {
        browserThumbnailForegroundRequestsInFlight_.insert(cacheKey);
    }
    QNetworkReply *reply = thumbnailNetworkManager_->get(request);
    browserThumbnailReplies_.insert(cacheKey, reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, cacheKey, discovery]() {
        browserThumbnailRequestsInFlight_.remove(cacheKey);
        browserThumbnailForegroundRequestsInFlight_.remove(cacheKey);
        browserThumbnailReplies_.remove(cacheKey);

        if (reply == nullptr || reply->error() != QNetworkReply::NoError) {
            if (reply != nullptr) {
                reply->deleteLater();
            }
            pumpBrowserThumbnailRequests();
            return;
        }

        const QByteArray bytes = reply->readAll();
        reply->deleteLater();

        const QImage image = loadAutoTransformedImageFromBytes(bytes);
        const QPixmap preview = QPixmap::fromImage(image);
        if (preview.isNull()) {
            pumpBrowserThumbnailRequests();
            return;
        }

        browserThumbnailForegroundPending_.remove(cacheKey);
        browserThumbnailForegroundOrder_.removeAll(cacheKey);
        browserThumbnailBackgroundPending_.remove(cacheKey);
        browserThumbnailBackgroundOrder_.removeAll(cacheKey);

        const QString title = discovery.originalFilename.isEmpty() ? discovery.eventId : discovery.originalFilename;
        const QString subtitle = QStringLiteral("%1 | %2")
                                     .arg(mediaCategoryTitle(discovery.category), mediaSourceKindTitle(discovery.sourceKind));
        const QPixmap tile = renderDiscoveryTile(
            QSize(kDiscoveryTileWidth, kDiscoveryTileHeight),
            title,
            subtitle,
            categoryAccent(discovery.category),
            &preview,
            discovery.category == MediaCategory::Videos);
        const QIcon icon(tile);
        cacheBrowserThumbnailIcon(cacheKey, icon);
        updateBrowserThumbnailItems(cacheKey, icon);
        pumpBrowserThumbnailRequests();
    });
}

void MainWindow::cacheBrowserThumbnailIcon(const QString &cacheKey, const QIcon &icon)
{
    if (cacheKey.isEmpty()) {
        return;
    }

    if (!browserThumbnailIconCache_.contains(cacheKey)) {
        browserThumbnailCacheOrder_.append(cacheKey);
    }
    browserThumbnailIconCache_.insert(cacheKey, icon);

    while (browserThumbnailCacheOrder_.size() > kBrowserThumbnailCacheLimit) {
        const QString expiredKey = browserThumbnailCacheOrder_.takeFirst();
        if (!expiredKey.isEmpty()) {
            browserThumbnailIconCache_.remove(expiredKey);
        }
    }
}

void MainWindow::updateBrowserThumbnailItems(const QString &cacheKey, const QIcon &icon)
{
    if (discoveriesList_ == nullptr) {
        return;
    }

    Q_UNUSED(icon);
    const QString selectedEventId = currentBrowserSelectedEventId();
    const QSignalBlocker blocker(discoveriesList_);

    for (int index = 0; index < discoveriesList_->count(); ++index) {
        QListWidgetItem *item = discoveriesList_->item(index);
        if (item == nullptr) {
            continue;
        }
        if (item->data(Qt::UserRole + 2).toString() == cacheKey) {
            if (index >= 0 && index < browserLoadedDiscoveries_.size()) {
                applyDiscoveryPresentation(item, browserLoadedDiscoveries_.at(index));
            }
        }
    }

    restoreBrowserSelectionByEventId(selectedEventId);
}

void MainWindow::resetLogsPageState()
{
    loadedLogEntries_.clear();
    logsLoadedOffset_ = 0;
    logTotalCount_ = 0;
    logsLoadingPage_ = false;
    if (logsTable_ != nullptr) {
        logsTable_->setRowCount(0);
    }
}

void MainWindow::loadMoreLogs(const bool reset)
{
    if (logsTable_ == nullptr || logsLoadingPage_) {
        return;
    }

    if (reset) {
        resetLogsPageState();
        logsProblemsOnly_ = logProblemsOnlyCheck_ != nullptr && logProblemsOnlyCheck_->isChecked();
        logTotalCount_ = controller_->logCount(logsProblemsOnly_);
    }

    if (loadedLogEntries_.size() >= logTotalCount_) {
        return;
    }

    logsLoadingPage_ = true;
    const QVector<ActivityLogEntry> page = controller_->fetchLogsPage(
        logsLoadedOffset_ + loadedLogEntries_.size(),
        kLogsPageSize,
        logsProblemsOnly_);

    logsTable_->setUpdatesEnabled(false);
    for (const ActivityLogEntry &entry : page) {
        const int row = logsTable_->rowCount();
        logsTable_->insertRow(row);

        const QString levelText = appLogLevelTitle(entry.level);
        const QString timeText = entry.createdAt.isValid()
            ? QLocale().toString(entry.createdAt.toLocalTime(), QLocale::ShortFormat)
            : QStringLiteral("Unknown");

        auto *timeItem = new QTableWidgetItem(timeText);
        auto *levelItem = new QTableWidgetItem(levelText);
        auto *subsystemItem = new QTableWidgetItem(entry.subsystem);
        auto *messageItem = new QTableWidgetItem(entry.message);
        timeItem->setToolTip(entry.createdAt.toString(Qt::ISODateWithMs));
        messageItem->setToolTip(entry.message);

        logsTable_->setItem(row, 0, timeItem);
        logsTable_->setItem(row, 1, levelItem);
        logsTable_->setItem(row, 2, subsystemItem);
        logsTable_->setItem(row, 3, messageItem);
        loadedLogEntries_.append(entry);
    }
    logsTable_->setUpdatesEnabled(true);
    logsLoadingPage_ = false;

    if (logsSummaryLabel_ != nullptr) {
        const QString filterText = logsProblemsOnly_
            ? QStringLiteral("warnings/errors")
            : QStringLiteral("all entries");
        logsSummaryLabel_->setText(
            QStringLiteral("Showing %1 of %2 %3 from the local activity log. Newest entries appear first.")
                .arg(loadedLogEntries_.size())
                .arg(logTotalCount_)
                .arg(filterText));
    }

    if (!page.isEmpty() && loadedLogEntries_.size() < logTotalCount_) {
        QTimer::singleShot(0, this, [this]() {
            maybeLoadMoreLogs();
        });
    }
}

void MainWindow::maybeLoadMoreLogs()
{
    if (logsTable_ == nullptr
        || logsLoadingPage_
        || logTotalCount_ <= 0
        || loadedLogEntries_.isEmpty()) {
        return;
    }

    QScrollBar *scrollBar = logsTable_->verticalScrollBar();
    if (scrollBar == nullptr) {
        return;
    }

    int firstVisibleRow = logsTable_->rowAt(0);
    if (firstVisibleRow < 0) {
        firstVisibleRow = 0;
    }
    int lastVisibleRow = logsTable_->rowAt(logsTable_->viewport()->height() - 1);
    if (lastVisibleRow < 0) {
        lastVisibleRow = qMin(logsTable_->rowCount() - 1, kLogsPageSize - 1);
    }

    const int rowHeight = logsTable_->rowCount() > 0
        ? logsTable_->rowHeight(0)
        : logsTable_->verticalHeader()->defaultSectionSize();

    if (logsLoadedOffset_ > 0 && firstVisibleRow <= (kLogsPageSize * kLogsBufferedPages)) {
        const int prependCount = qMin(kLogsPageSize, logsLoadedOffset_);
        const int newOffset = logsLoadedOffset_ - prependCount;
        const QVector<ActivityLogEntry> page = controller_->fetchLogsPage(
            newOffset,
            prependCount,
            logsProblemsOnly_);
        if (!page.isEmpty()) {
            logsTable_->setUpdatesEnabled(false);
            for (int index = page.size() - 1; index >= 0; --index) {
                const ActivityLogEntry &entry = page.at(index);
                logsTable_->insertRow(0);

                const QString levelText = appLogLevelTitle(entry.level);
                const QString timeText = entry.createdAt.isValid()
                    ? QLocale().toString(entry.createdAt.toLocalTime(), QLocale::ShortFormat)
                    : QStringLiteral("Unknown");

                auto *timeItem = new QTableWidgetItem(timeText);
                auto *levelItem = new QTableWidgetItem(levelText);
                auto *subsystemItem = new QTableWidgetItem(entry.subsystem);
                auto *messageItem = new QTableWidgetItem(entry.message);
                timeItem->setToolTip(entry.createdAt.toString(Qt::ISODateWithMs));
                messageItem->setToolTip(entry.message);

                logsTable_->setItem(0, 0, timeItem);
                logsTable_->setItem(0, 1, levelItem);
                logsTable_->setItem(0, 2, subsystemItem);
                logsTable_->setItem(0, 3, messageItem);
                loadedLogEntries_.prepend(entry);
            }
            logsTable_->setUpdatesEnabled(true);
            logsLoadedOffset_ = newOffset;
            scrollBar->setValue(scrollBar->value() + (page.size() * rowHeight));
        }
    }

    const int bufferedAhead = loadedLogEntries_.size() - (lastVisibleRow + 1);
    if ((logsLoadedOffset_ + loadedLogEntries_.size()) < logTotalCount_
        && (scrollBar->maximum() <= 0 || bufferedAhead <= (kLogsPageSize * kLogsBufferedPages))) {
        loadMoreLogs(false);
        return;
    }

    trimLogWindow();
    if (logsSummaryLabel_ != nullptr) {
        const QString filterText = logsProblemsOnly_
            ? QStringLiteral("warnings/errors")
            : QStringLiteral("all entries");
        logsSummaryLabel_->setText(
            QStringLiteral("Showing %1 of %2 %3 from the local activity log. Newest entries appear first.")
                .arg(loadedLogEntries_.size())
                .arg(logTotalCount_)
                .arg(filterText));
    }
}

void MainWindow::trimLogWindow()
{
    if (logsTable_ == nullptr) {
        return;
    }

    const int maxRows = kLogsPageSize * kLogsMaxWindowPages;
    if (loadedLogEntries_.size() <= maxRows) {
        return;
    }

    QScrollBar *scrollBar = logsTable_->verticalScrollBar();
    if (scrollBar == nullptr) {
        return;
    }

    int firstVisibleRow = logsTable_->rowAt(0);
    if (firstVisibleRow < 0) {
        firstVisibleRow = 0;
    }
    int lastVisibleRow = logsTable_->rowAt(logsTable_->viewport()->height() - 1);
    if (lastVisibleRow < 0) {
        lastVisibleRow = qMin(logsTable_->rowCount() - 1, kLogsPageSize - 1);
    }

    const int rowHeight = logsTable_->rowCount() > 0
        ? logsTable_->rowHeight(0)
        : logsTable_->verticalHeader()->defaultSectionSize();

    while (loadedLogEntries_.size() > maxRows
           && firstVisibleRow > (kLogsPageSize * (kLogsBufferedPages + 1))) {
        const int removeCount = qMin(kLogsPageSize, loadedLogEntries_.size() - maxRows);
        loadedLogEntries_.remove(0, removeCount);
        logsLoadedOffset_ += removeCount;
        for (int row = 0; row < removeCount; ++row) {
            logsTable_->removeRow(0);
        }
        firstVisibleRow -= removeCount;
        lastVisibleRow -= removeCount;
        scrollBar->setValue(qMax(0, scrollBar->value() - (removeCount * rowHeight)));
    }

    while (loadedLogEntries_.size() > maxRows
           && (loadedLogEntries_.size() - (lastVisibleRow + 1))
               > (kLogsPageSize * (kLogsBufferedPages + 1))) {
        const int removeCount = qMin(kLogsPageSize, loadedLogEntries_.size() - maxRows);
        for (int row = 0; row < removeCount; ++row) {
            loadedLogEntries_.removeLast();
            logsTable_->removeRow(logsTable_->rowCount() - 1);
        }
    }
}

bool MainWindow::saveSettingsFromUi(const bool interactive)
{
    if (!settingsPageInitialized_) {
        return true;
    }

    if (!controller_->saveSettings(gatherSettingsFromUi(), passwordEdit_->text())) {
        if (interactive && controller_->lastErrorMessage().trimmed().isEmpty()) {
            controller_->recordError(QStringLiteral("settings"), QStringLiteral("Failed to save settings."));
        }
        return false;
    }

    applyTheme(controller_->settings().darkModeEnabled);
    settingsDirty_ = false;
    return true;
}

void MainWindow::markSettingsDirty()
{
    if (populatingSettingsUi_) {
        return;
    }
    settingsDirty_ = true;
}

void MainWindow::applyTheme(const bool darkModeEnabled)
{
    QApplication *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (app == nullptr) {
        return;
    }

    captureThemeBaseline(app);
    const ThemeBaseline &baseline = themeBaseline();
    if (darkModeEnabled) {
        app->setStyle(QStringLiteral("Fusion"));
        app->setPalette(darkPalette());
        app->setStyleSheet(QStringLiteral(
            "QToolTip {"
            " color: #edf2f7;"
            " background-color: #111827;"
            " border: 1px solid #4b5563;"
            " }"));
        return;
    }

    if (!baseline.styleName.trimmed().isEmpty()) {
        app->setStyle(baseline.styleName);
    }
    app->setPalette(baseline.palette);
    app->setStyleSheet(QString());
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

AppSection MainWindow::currentSection() const
{
    const QList<AppSection> sections = allSections();
    const int index = stack_ != nullptr ? stack_->currentIndex() : -1;
    if (index >= 0 && index < sections.size()) {
        return sections.at(index);
    }
    return AppSection::Browser;
}

QVector<RoomRecord> MainWindow::roomSidebarRoomsForCurrentSection() const
{
    if (currentSection() == AppSection::Browser) {
        QVector<RoomRecord> rooms;
        for (const RoomRecord &room : controller_->rooms()) {
            if (room.isSpace) {
                continue;
            }
            if (room.membership == QStringLiteral("joined") || room.discoveredMediaCount > 0) {
                rooms.append(room);
            }
        }
        return rooms;
    }

    return controller_->rooms();
}

QString MainWindow::selectedRoomId() const
{
    return roomsList_ != nullptr && roomsList_->currentItem() != nullptr
        ? roomsList_->currentItem()->data(Qt::UserRole).toString()
        : QString();
}

QString MainWindow::selectedBrowserRoomId() const
{
    if (currentSection() == AppSection::Browser) {
        return selectedRoomId();
    }
    return browserSelectedRoomId_;
}

bool MainWindow::isUploadableBrowserRoom(const QString &roomId) const
{
    for (const RoomRecord &room : controller_->rooms()) {
        if (room.roomId == roomId) {
            return !room.isSpace && room.membership == QStringLiteral("joined");
        }
    }
    return false;
}

bool MainWindow::currentSectionUsesRoomSidebar() const
{
    return sectionUsesRoomSidebar(currentSection());
}

void MainWindow::updateRoomSidebarVisibility()
{
    if (roomSidebarContainer_ == nullptr) {
        return;
    }
    roomSidebarContainer_->setVisible(currentSectionUsesRoomSidebar());
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (currentSection() == AppSection::Browser
        && (watched == browserPage_
            || watched == discoveriesList_
            || (discoveriesList_ != nullptr && watched == discoveriesList_->viewport()))
        && event != nullptr) {
        switch (event->type()) {
        case QEvent::DragEnter:
            dragEnterEvent(static_cast<QDragEnterEvent *>(event));
            return event->isAccepted();
        case QEvent::DragMove:
            dragMoveEvent(static_cast<QDragMoveEvent *>(event));
            return event->isAccepted();
        case QEvent::Drop:
            dropEvent(static_cast<QDropEvent *>(event));
            return event->isAccepted();
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event != nullptr
        && currentSection() == AppSection::Browser
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
        && currentSection() == AppSection::Browser
        && event->mimeData() != nullptr
        && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QMainWindow::dragMoveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (event == nullptr
        || currentSection() != AppSection::Browser
        || event->mimeData() == nullptr) {
        QMainWindow::dropEvent(event);
        return;
    }

    const QString roomId = selectedBrowserRoomId();
    if (!isUploadableBrowserRoom(roomId)) {
        controller_->recordWarning(
            QStringLiteral("share"),
            QStringLiteral("Pick a joined room before dropping files."));
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
