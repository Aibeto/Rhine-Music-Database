#pragma once
#include <functional>

class QWindow;

/** 业务窗口属于自己的桌面和调度组，不能继承 agent/浮动窗的跨桌面行为。 */
void bindInterfaceToDesktop(QWindow *window);

/** 无窗口的测试控制器只保留辅助身份，不占据 Dock 或参与被测的调度组。 */
void configureTestControllerAsAccessory();

class QByteArray;
bool showInterfaceStaticImage(QWindow *window, const QByteArray &image);
void clearInterfaceStaticImage(QWindow *window);

void drainInterfaceResources(const std::function<void()> &release);
