#include "VideoFrameWidget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QVideoSink>

VideoFrameWidget::VideoFrameWidget(QWidget *parent)
    : QWidget(parent)
    , videoSink_(new QVideoSink(this))
{
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumSize(240, 180);

    connect(videoSink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        currentFrame_ = frame;
        updateGeometry();
        update();
    });
}

QVideoSink *VideoFrameWidget::videoSink() const
{
    return videoSink_;
}

void VideoFrameWidget::clearFrame()
{
    currentFrame_ = QVideoFrame();
    updateGeometry();
    update();
}

QSize VideoFrameWidget::sizeHint() const
{
    if (currentFrame_.isValid()) {
        const QSize frameSize = currentFrame_.surfaceFormat().viewport().size();
        if (frameSize.isValid()) {
            return frameSize;
        }
        if (currentFrame_.size().isValid()) {
            return currentFrame_.size();
        }
    }
    return QWidget::sizeHint().expandedTo(QSize(320, 240));
}

void VideoFrameWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(rect(), QColor(QStringLiteral("#020617")));

    if (!currentFrame_.isValid()) {
        return;
    }

    QVideoFrame frame = currentFrame_;
    QVideoFrame::PaintOptions options;
    options.backgroundColor = QColor(QStringLiteral("#020617"));
    options.aspectRatioMode = Qt::KeepAspectRatio;
    frame.paint(&painter, rect(), options);
}
