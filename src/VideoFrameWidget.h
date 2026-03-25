#pragma once

#include <QVideoFrame>
#include <QWidget>

class QVideoSink;

class VideoFrameWidget : public QWidget
{
public:
    explicit VideoFrameWidget(QWidget *parent = nullptr);

    QVideoSink *videoSink() const;
    void clearFrame();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVideoSink *videoSink_ = nullptr;
    QVideoFrame currentFrame_;
};
