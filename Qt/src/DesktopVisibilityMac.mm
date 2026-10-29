#include "DesktopVisibility.h"
#include "DesktopPresence.h"
#import <AppKit/AppKit.h>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QtGui/qscreen_platform.h>

namespace {
QRectF windowRectangle(NSDictionary *window) {
    CGRect bounds;
    if (!CGRectMakeWithDictionaryRepresentation(
            (__bridge CFDictionaryRef)window[(__bridge id)kCGWindowBounds], &bounds)) {
        return {};
    }
    return QRectF(bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height);
}

NSScreen *desktopScreen(const QString &name, const QRect &geometry) {
    for (auto screen : QGuiApplication::screens()) {
        if (screen->name() == name && (!geometry.isValid() || screen->geometry() == geometry)) {
            if (auto native = screen->nativeInterface<QNativeInterface::QCocoaScreen>()) {
                return native->nativeScreen();
            }
        }
    }
    return [NSScreen screens].firstObject;
}

QRectF screenRectangle(NSRect frame) {
    // Core Graphics 以主屏左上角为原点，AppKit 以主屏左下角为原点，单位均为逻辑点。
    const auto primaryHeight = [NSScreen screens].firstObject.frame.size.height;
    return QRectF(frame.origin.x, primaryHeight - NSMaxY(frame), frame.size.width, frame.size.height);
}

bool hasStageManagerStrip(NSArray *windows, const QRectF &screen) {
    const auto manager =
        [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.WindowManager"].firstObject;
    if (!manager) {
        return false;
    }
    for (NSDictionary *window in windows) {
        if ([window[(__bridge id)kCGWindowOwnerPID] intValue] == manager.processIdentifier) {
            const auto bounds = windowRectangle(window);
            if (bounds.intersects(screen) && bounds.width() < screen.width() / 4) {
                return true;
            }
        }
    }
    return false;
}

bool isStageManagerEnabled(NSArray *windows, const QRectF &screen) {
    // 系统没有公开的调度组恢复接口；开关仅作为保守恢复门槛，不修改系统偏好。
    const auto domain = CFSTR("com.apple.WindowManager");
    CFPreferencesAppSynchronize(domain);
    Boolean valid = false;
    const auto enabled = CFPreferencesGetAppBooleanValue(CFSTR("GloballyEnabled"), domain, &valid);
    return valid ? bool(enabled) : hasStageManagerStrip(windows, screen);
}

class MacDesktopPresence final : public DesktopPresenceMonitor {
  public:
    ~MacDesktopPresence() override {
        [m_anchor close];
    }

    void setScreen(const QString &name, const QRect &geometry) override {
        if (name != m_screen || geometry != m_geometry) {
            m_screen = name;
            m_geometry = geometry;
            resetWorkspace();
        }
    }

    /** 保留不可见的原生桌面归属标记；不创建 Qt 图形设备、场景或 Metal 表面。 */
    void resetWorkspace() override {
        if (QGuiApplication::platformName() != "cocoa") {
            return;
        }
        [m_anchor close];
        auto screen = desktopScreen(m_screen, m_geometry);
        if (!screen) {
            m_anchor = nil;
            return;
        }
        const auto origin = screen.visibleFrame.origin;
        m_anchor = [[NSPanel alloc]
            initWithContentRect:NSMakeRect(origin.x, origin.y, 1, 1)
                      styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                        backing:NSBackingStoreBuffered
                          defer:YES];
        m_anchor.releasedWhenClosed = NO;
        m_anchor.opaque = NO;
        m_anchor.alphaValue = 0;
        m_anchor.hasShadow = NO;
        m_anchor.hidesOnDeactivate = NO;
        m_anchor.ignoresMouseEvents = YES;
        m_anchor.excludedFromWindowsMenu = YES;
        m_anchor.animationBehavior = NSWindowAnimationBehaviorNone;
        auto behavior = NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle |
                        NSWindowCollectionBehaviorFullScreenPrimary;
        if (@available(macOS 13.0, *)) {
            behavior |= NSWindowCollectionBehaviorCanJoinAllApplications;
        }
        m_anchor.collectionBehavior = behavior;
        // 首次排序只确定所属 Space；透明标记不显示内容，随后保持隐藏，仍可查询桌面归属。
        [m_anchor orderBack:nil];
        [m_anchor orderOut:nil];
    }

    DesktopPresence sample(qint64 interfacePid, bool fullscreen) const override {
        DesktopPresence result;
        if (QGuiApplication::platformName() != "cocoa") {
            return result;
        }
        result.workspaceActive = !m_anchor || m_anchor.onActiveSpace;
        auto target = desktopScreen(m_screen, m_geometry);
        NSArray *windows =
            CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID));
        result.automaticRestoreAllowed =
            !fullscreen && !isStageManagerEnabled(windows, screenRectangle(target.frame));
        result.covered = platformDesktopCovered(m_screen, interfacePid, m_geometry);
        const auto foreground = [NSWorkspace sharedWorkspace].frontmostApplication;
        if (interfacePid <= 0) {
            return result;
        }
        bool foundWindow = false;
        bool hasPresentedWindow = false;
        for (NSDictionary *window in windows) {
            if ([window[(__bridge id)kCGWindowOwnerPID] longLongValue] != interfacePid ||
                [window[(__bridge id)kCGWindowLayer] intValue] != 0) {
                continue;
            }
            const auto bounds = windowRectangle(window);
            if (bounds.width() < 100 || bounds.height() < 100) {
                continue;
            }
            foundWindow = true;
            const bool isOnScreen = [window[(__bridge id)kCGWindowIsOnscreen] boolValue];
            // 面板最小尺寸为 960 × 540；系统调度缩略图不能视为仍在显示音乐面板。
            if (isOnScreen && bounds.width() >= 480 && bounds.height() >= 270) {
                hasPresentedWindow = true;
            }
        }
        // 面板自己进入全屏时拥有独立 Space；可见的真实窗口比原桌面锚点更优先。
        result.workspaceActive = result.workspaceActive || hasPresentedWindow;
        result.stagedAway = foundWindow && !hasPresentedWindow && result.workspaceActive &&
                            foreground.processIdentifier != interfacePid;
        return result;
    }

  private:
    QString m_screen;
    QRect m_geometry;
    NSPanel *m_anchor = nil;
};
}

std::unique_ptr<DesktopPresenceMonitor> createDesktopPresenceMonitor() {
    return std::make_unique<MacDesktopPresence>();
}

/** 只读取进程、叠放顺序和矩形，不读取标题、内容或请求录屏权限。 */
bool platformDesktopCovered(const QString &screenName, qint64 interfacePid, const QRect &screenGeometry) {
    const auto foreground = [NSWorkspace sharedWorkspace].frontmostApplication;
    if (!foreground || (interfacePid > 0 && foreground.processIdentifier == interfacePid)) {
        return false;
    }
    auto target = desktopScreen(screenName, screenGeometry);
    if (!target) {
        return false;
    }
    const auto screen = screenRectangle(target.frame);
    const auto work = screenRectangle(target.visibleFrame);
    NSArray *windows = CFBridgingRelease(CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
    const bool hasStrip = hasStageManagerStrip(windows, screen);
    for (NSDictionary *window in windows) {
        if ([window[(__bridge id)kCGWindowLayer] intValue] != 0) {
            continue;
        }
        const auto ownerPid = [window[(__bridge id)kCGWindowOwnerPID] intValue];
        // 只判断面板之前的窗口；面板销毁后继续观察仍留在屏幕上的遮挡窗口。
        if (ownerPid == interfacePid && interfacePid > 0) {
            return false;
        }
        if (ownerPid == QCoreApplication::applicationPid()) {
            continue;
        }
        const auto bounds = windowRectangle(window);
        if (desktopWindowCoversScreen(bounds, screen, work) ||
            desktopWindowFillsManagedArea(bounds, work, hasStrip)) {
            return true;
        }
    }
    return false;
}

qint64 platformDesktopForegroundPid() {
    return [NSWorkspace sharedWorkspace].frontmostApplication.processIdentifier;
}
