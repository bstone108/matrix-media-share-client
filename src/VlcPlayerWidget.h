#pragma once

#include <QWidget>

class VlcPlayerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VlcPlayerWidget(QWidget *parent = nullptr);
    ~VlcPlayerWidget() override;

    bool isAvailable() const;
    QString lastError() const;
    bool playFile(const QString &filePath);
    void stopPlayback();

    QSize sizeHint() const override;

signals:
    void playbackStarted();
    void playbackFailed(const QString &error);

protected:
    void showEvent(QShowEvent *event) override;

private:
    bool ensureInitialized();
    bool startPlaybackNow(const QString &filePath);
    void schedulePendingPlayback();

    struct Private;
    Private *d_ = nullptr;
};
