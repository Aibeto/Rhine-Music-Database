#include "InterfaceWindowMac.h"
#import <AppKit/AppKit.h>
#include <QWindow>
#include <QByteArray>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#include <QGuiApplication>

/** 业务窗口属于自己的桌面和调度组，不能继承 agent/浮动窗的跨桌面行为。 */
void bindInterfaceToDesktop(QWindow *window) {
    if (QGuiApplication::platformName() != "cocoa") {
        return;
    }
    NSView *view = (__bridge NSView *)(void *)window->winId();
    auto behavior = view.window.collectionBehavior;
    behavior &= ~(NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorMoveToActiveSpace |
                  NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorStationary);
    behavior |= NSWindowCollectionBehaviorManaged;
    if (@available(macOS 13.0, *)) {
        behavior &= ~(NSWindowCollectionBehaviorAuxiliary | NSWindowCollectionBehaviorCanJoinAllApplications);
        behavior |= NSWindowCollectionBehaviorPrimary;
    }
    view.window.collectionBehavior = behavior;
}

/** 无窗口的测试控制器只保留辅助身份，不占据 Dock 或参与被测的调度组。 */
void configureTestControllerAsAccessory() {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}

/** 原生图片子层不持有 Metal 设备；保留 Qt 原生窗口及其桌面归属。 */
bool showInterfaceStaticImage(QWindow *window, const QByteArray &source) {
    NSView *view = (__bridge NSView *)(void *)window->winId();
    clearInterfaceStaticImage(window);
    // 设备解绑不会清空 CAMetalLayer 的 drawable 池；更换背衬层才能归还旧 IOSurface。
    // 通过视图自身的工厂保留 Qt 的 layer 子类，NSView、NSWindow 和桌面归属不变。
    CALayer *backing = [view makeBackingLayer];
    backing.frame = view.bounds;
    backing.contentsScale = window->devicePixelRatio();
    view.layer = backing;
    // ImageIO 直接下采样，避免先解码完整的 5K 图片再创建缩小副本。
    CFDataRef bytes =
        CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(source.constData()), source.size());
    if (!bytes) {
        return false;
    }
    CGImageSourceRef decoder = CGImageSourceCreateWithData(bytes, nullptr);
    if (!decoder) {
        CFRelease(bytes);
        return false;
    }
    NSDictionary *options = @{
        (id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
        (id)kCGImageSourceThumbnailMaxPixelSize : @1280,
        (id)kCGImageSourceShouldCacheImmediately : @YES
    };
    CGImageRef picture = CGImageSourceCreateThumbnailAtIndex(decoder, 0, (__bridge CFDictionaryRef)options);
    if (!picture) {
        CFRelease(decoder);
        CFRelease(bytes);
        return false;
    }
    CALayer *layer = [CALayer layer];
    layer.name = @"RhineStaticImage";
    layer.frame = view.bounds;
    layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    layer.contentsGravity = kCAGravityResizeAspectFill;
    layer.masksToBounds = YES;
    layer.contents = (__bridge id)picture;
    [view.layer addSublayer:layer];
    CGImageRelease(picture);
    CFRelease(decoder);
    CFRelease(bytes);
    return true;
}

void clearInterfaceStaticImage(QWindow *window) {
    NSView *view = (__bridge NSView *)(void *)window->winId();
    for (CALayer *layer in [view.layer.sublayers copy]) {
        if ([layer.name isEqualToString:@"RhineStaticImage"]) {
            [layer removeFromSuperlayer];
        }
    }
}

/** 将驱动与 Cocoa 的临时引用限定在释放事务内，避免留到外层事件池才归还。 */
void drainInterfaceResources(const std::function<void()> &release) {
    @autoreleasepool {
        release();
    }
}
