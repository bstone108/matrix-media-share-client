#include "WebVideoWidget.h"

#include <QResizeEvent>

WebVideoWidget::WebVideoWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
}

WebVideoWidget::~WebVideoWidget() = default;

bool WebVideoWidget::isAvailable() const
{
    return false;
}

bool WebVideoWidget::loadMediaFile(const QString &filePath, const QString &mimeType)
{
    Q_UNUSED(filePath);
    Q_UNUSED(mimeType);
    return false;
}

void WebVideoWidget::clearMedia()
{
}

QSize WebVideoWidget::sizeHint() const
{
    return QWidget::sizeHint().expandedTo(QSize(480, 320));
}

void WebVideoWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
}
