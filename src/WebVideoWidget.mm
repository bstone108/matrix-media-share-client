#include "WebVideoWidget.h"

#include <QFileInfo>
#include <QResizeEvent>
#include <QUrl>

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

namespace {

NSString *toNSString(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    return [[NSString alloc] initWithBytes:utf8.constData()
                                     length:static_cast<NSUInteger>(utf8.size())
                                   encoding:NSUTF8StringEncoding];
}

WKWebView *asWebView(void *value)
{
    return (__bridge WKWebView *)value;
}

NSView *hostViewForWId(WId wid)
{
    return (__bridge NSView *)(reinterpret_cast<void *>(static_cast<quintptr>(wid)));
}

QString relativeMediaHref(const QFileInfo &info)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(info.fileName(), QByteArray(), QByteArray(" ")))
        .replace(QStringLiteral(" "), QStringLiteral("%20"));
}

} // namespace

WebVideoWidget::WebVideoWidget(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setMinimumSize(320, 240);
    setStyleSheet(QStringLiteral("background:#020617;"));

    (void)winId();
    auto *hostView = hostViewForWId(winId());
    if (hostView == nullptr) {
        return;
    }

    WKWebViewConfiguration *configuration = [[WKWebViewConfiguration alloc] init];
    if (@available(macOS 11.0, *)) {
        configuration.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
    }

    WKWebView *webView = [[WKWebView alloc] initWithFrame:[hostView bounds] configuration:configuration];
    [webView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [hostView addSubview:webView];

    nativeView_ = (__bridge_retained void *)webView;
    available_ = true;
}

WebVideoWidget::~WebVideoWidget()
{
    clearMedia();
    if (nativeView_ != nullptr) {
        WKWebView *webView = (__bridge_transfer WKWebView *)nativeView_;
        [webView removeFromSuperview];
        nativeView_ = nullptr;
    }
}

bool WebVideoWidget::isAvailable() const
{
    return available_;
}

bool WebVideoWidget::loadMediaFile(const QString &filePath, const QString &mimeType)
{
    if (!available_ || nativeView_ == nullptr) {
        return false;
    }

    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        return false;
    }

    const QString sourceHref = relativeMediaHref(info);
    const QString escapedMime = mimeType.toHtmlEscaped();
    const QString html = QStringLiteral(
                             "<!doctype html>"
                             "<html><head><meta charset=\"utf-8\">"
                             "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                             "<style>"
                             "html,body{margin:0;width:100%%;height:100%%;background:#020617;overflow:hidden;}"
                             "body{display:flex;align-items:center;justify-content:center;}"
                             "video{width:100%%;height:100%%;object-fit:contain;background:#020617;}"
                             "</style>"
                             "</head><body>"
                             "<video controls autoplay playsinline preload=\"auto\">"
                             "<source src=\"%1\" type=\"%2\">"
                             "</video>"
                             "</body></html>")
                             .arg(sourceHref.toHtmlEscaped(), escapedMime);

    WKWebView *webView = asWebView(nativeView_);
    NSURL *baseUrl = [NSURL fileURLWithPath:toNSString(info.absolutePath()) isDirectory:YES];
    [webView loadHTMLString:toNSString(html) baseURL:baseUrl];
    return true;
}

void WebVideoWidget::clearMedia()
{
    if (nativeView_ == nullptr) {
        return;
    }

    WKWebView *webView = asWebView(nativeView_);
    [webView loadHTMLString:@"<html><body style='margin:0;background:#020617;'></body></html>" baseURL:nil];
}

QSize WebVideoWidget::sizeHint() const
{
    return QWidget::sizeHint().expandedTo(QSize(480, 320));
}

void WebVideoWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (nativeView_ == nullptr) {
        return;
    }

    if (auto *hostView = hostViewForWId(winId())) {
        WKWebView *webView = asWebView(nativeView_);
        [webView setFrame:[hostView bounds]];
    }
}
