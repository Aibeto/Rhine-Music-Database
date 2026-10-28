#include "InterfaceWindowMac.h"
#import <AppKit/AppKit.h>

/** 无窗口的测试控制器只保留辅助身份，不占据 Dock 或参与被测的调度组。 */
void configureTestControllerAsAccessory() {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}
