#import <AppKit/AppKit.h>
#include <cstdio>

@interface RhineDesktopFixture : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSWindow *foregroundWindow;
@end

@implementation RhineDesktopFixture
/** 使用独立 bundle 身份模拟外部应用；同一个音乐面板的多进程不能验证调度组切换。 */
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    const auto arguments = NSProcessInfo.processInfo.arguments;
    const auto mode = arguments.count > 1 ? arguments[1] : @"normal";
    const auto style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 640, 360)
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"Rhine desktop verification";
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    if ([mode isEqualToString:@"maximized"] || [mode isEqualToString:@"maximized-with-normal"]) {
        [self.window setFrame:self.window.screen.visibleFrame display:YES];
    }
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if ([mode isEqualToString:@"fullscreen"]) {
        [self.window toggleFullScreen:nil];
    } else if ([mode isEqualToString:@"maximized-with-normal"]) {
        self.foregroundWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(300, 180, 640, 360)
                                                            styleMask:style
                                                              backing:NSBackingStoreBuffered
                                                                defer:NO];
        [self.window addChildWindow:self.foregroundWindow ordered:NSWindowAbove];
        [self.foregroundWindow makeKeyAndOrderFront:nil];
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1600 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
      std::puts("COVERAGE_FIXTURE_READY");
      std::fflush(stdout);
    });
}
@end

/** 初始化独立的原生应用事件循环；夹具只提供外部窗口，不读取音乐或账号数据。 */
int main(int argc, char **argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        auto delegate = [RhineDesktopFixture new];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
