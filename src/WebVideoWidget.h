#pragma once

#include <QWidget>

class WebVideoWidget : public QWidget
{
public:
    explicit WebVideoWidget(QWidget *parent = nullptr);
    ~WebVideoWidget() override;

    bool isAvailable() const;
    bool loadMediaFile(const QString &filePath, const QString &mimeType = QString());
    void clearMedia();

    QSize sizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void *nativeView_ = nullptr;
    bool available_ = false;
};
