#pragma once
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

/** 菜单栏/托盘共用的单色图标；后台服务与界面进程按各自平台各自绘制一次。 */
inline QIcon rhineTrayIcon() {
#ifdef Q_OS_MACOS
    QPixmap image(44, 44);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.drawRoundedRect(QRectF(23, 7, 4, 26), 1, 1);
    painter.drawEllipse(QRectF(11, 26, 16, 11));
    painter.drawRoundedRect(QRectF(23, 7, 12, 6), 1, 1);
    painter.end();
    image.setDevicePixelRatio(2);
    QIcon icon(image);
    icon.setIsMask(true);
    return icon;
#else
    return QGuiApplication::windowIcon();
#endif
}
