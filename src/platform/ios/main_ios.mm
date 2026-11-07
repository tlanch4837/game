// iOS shell: a UIKit scene with a CAMetalLayer view, driving the game without the editor (src/game/game_host.h).
// The display link runs the frames while the scene is active; touches become the game's pointer input:
// one finger is the left button (tools, GUI), two fingers pan the camera (the middle button) and pinch zooms
// (wheel notches at the pinch centre).
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <cmath>
#include <memory>
#include <string>
#include <utility>

#include "../../game/game_host.h"
#include "../../platform/platform.h"
#include "../../render/renderer.h"

@interface TbdMetalView : UIView
@end
@implementation TbdMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

@interface TbdViewController : UIViewController
- (void)resumeFrames;
- (void)pauseFrames;
@end

@implementation TbdViewController {
    CADisplayLink* _link;
    BOOL _started;
    BOOL _failed;
    // touches
    UITouch* _first;      // the finger that is the left button
    BOOL _leftDown;
    BOOL _twoFinger;      // two fingers: camera pan (middle button) and pinch
    CGPoint _panCentre;   // device pixels
    CGFloat _pinchDistance;
}

- (void)loadView {
    TbdMetalView* view = [[TbdMetalView alloc] initWithFrame:CGRectZero];
    view.multipleTouchEnabled = YES;
    self.view = view;
}

- (void)resumeFrames {
    if (_link || _failed) return;
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame:)];
    _link.preferredFrameRateRange = CAFrameRateRangeMake(30, 60, 60);
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)pauseFrames {
    [_link invalidate];
    _link = nil;
}

- (CGFloat)pixelScale { return self.view.contentScaleFactor; }

- (CGPoint)pixels:(CGPoint)p {
    const CGFloat s = [self pixelScale];
    return CGPointMake(p.x * s, p.y * s);
}

// First frame with a window and a size: the game, its Metal renderer and a new city.
- (BOOL)startGame {
    CAMetalLayer* layer = (CAMetalLayer*)self.view.layer;
    const CGFloat scale = [self pixelScale];
    const int w = (int)(self.view.bounds.size.width * scale), h = (int)(self.view.bounds.size.height * scale);
    if (!self.view.window || w <= 0 || h <= 0) return NO;
    layer.contentsScale = scale;

    NSString* resources = [[NSBundle mainBundle] resourcePath];
    const std::wstring assets = tbd::platform::Utf8ToWide(std::string(resources.fileSystemRepresentation) + "/assets");
    tbd::host::Init(assets);

    auto renderer = tbd::CreateMetalRenderer();
    if (!renderer || !renderer->Init((__bridge void*)layer, w, h, false)) {
        tbd::Log("iOS: the Metal renderer failed to start");
        _failed = YES;
        [self pauseFrames];
        return NO;
    }
    tbd::host::AttachRenderer(std::move(renderer), w, h);
    tbd::host::Resize(w, h, (float)scale);
    tbd::host::StartCity();
    tbd::Log("iOS: city started (%d x %d, scale %.1f)", w, h, (double)scale);
    return YES;
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (!_started) return;
    const CGFloat scale = [self pixelScale];
    tbd::host::Resize((int)(self.view.bounds.size.width * scale), (int)(self.view.bounds.size.height * scale), (float)scale);
}

- (void)frame:(CADisplayLink*)link {
    if (!_started) {
        if (![self startGame]) return;
        _started = YES;
    }
    tbd::host::Frame();
}

// ---- touches
- (NSArray<UITouch*>*)activeTouches:(UIEvent*)event {
    NSMutableArray<UITouch*>* out = [NSMutableArray array];
    for (UITouch* t in [event touchesForView:self.view])
        if (t.phase != UITouchPhaseEnded && t.phase != UITouchPhaseCancelled) [out addObject:t];
    return out;
}

- (void)beginTwoFinger:(NSArray<UITouch*>*)touches {
    if (_leftDown) { // the first finger was a left press: give it up rather than click where the pan starts
        tbd::host::PointerCancel();
        _leftDown = NO;
    }
    const CGPoint a = [self pixels:[touches[0] locationInView:self.view]], b = [self pixels:[touches[1] locationInView:self.view]];
    _panCentre = CGPointMake((a.x + b.x) * 0.5, (a.y + b.y) * 0.5);
    _pinchDistance = std::hypot(a.x - b.x, a.y - b.y);
    _twoFinger = YES;
    tbd::host::PointerDown(tbd::host::Button::Middle, (float)_panCentre.x, (float)_panCentre.y);
}

- (void)endTwoFinger {
    if (!_twoFinger) return;
    tbd::host::PointerUp(tbd::host::Button::Middle, (float)_panCentre.x, (float)_panCentre.y);
    _twoFinger = NO;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_started) return;
    NSArray<UITouch*>* active = [self activeTouches:event];
    if (active.count >= 2) {
        if (!_twoFinger) [self beginTwoFinger:active];
        return;
    }
    UITouch* t = [touches anyObject];
    if (!t || _leftDown || _twoFinger) return;
    _first = t;
    const CGPoint p = [self pixels:[t locationInView:self.view]];
    tbd::host::PointerMove((float)p.x, (float)p.y); // hover first, as a mouse would arrive
    tbd::host::PointerDown(tbd::host::Button::Left, (float)p.x, (float)p.y);
    _leftDown = YES;
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_started) return;
    if (_twoFinger) {
        NSArray<UITouch*>* active = [self activeTouches:event];
        if (active.count < 2) return;
        const CGPoint a = [self pixels:[active[0] locationInView:self.view]], b = [self pixels:[active[1] locationInView:self.view]];
        _panCentre = CGPointMake((a.x + b.x) * 0.5, (a.y + b.y) * 0.5);
        tbd::host::PointerMove((float)_panCentre.x, (float)_panCentre.y);
        const CGFloat d = std::hypot(a.x - b.x, a.y - b.y);
        if (_pinchDistance > 1.0 && d > 1.0) {
            // The wheel zooms the camera distance by 0.9^(0.5 n); spreading the fingers by f should divide it by f.
            const float notches = (float)(-2.0 * std::log(d / _pinchDistance) / std::log(0.9));
            if (std::fabs(notches) > 0.01f) tbd::host::Zoom(notches, (float)_panCentre.x, (float)_panCentre.y);
        }
        _pinchDistance = d;
        return;
    }
    if (_leftDown && [touches containsObject:_first]) {
        const CGPoint p = [self pixels:[_first locationInView:self.view]];
        tbd::host::PointerMove((float)p.x, (float)p.y);
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_started) return;
    if (_twoFinger) {
        if ([self activeTouches:event].count < 2) [self endTwoFinger];
        return;
    }
    if (_leftDown && [touches containsObject:_first]) {
        const CGPoint p = [self pixels:[_first locationInView:self.view]];
        tbd::host::PointerUp(tbd::host::Button::Left, (float)p.x, (float)p.y);
        _leftDown = NO;
        _first = nil;
    }
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_started) return;
    tbd::host::PointerCancel();
    _leftDown = NO;
    _twoFinger = NO;
    _first = nil;
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
@end

@interface TbdSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow* window;
@end

@implementation TbdSceneDelegate {
    TbdViewController* _controller;
}
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session
    options:(UISceneConnectionOptions*)options {
    if (![scene isKindOfClass:[UIWindowScene class]]) return;
    _controller = [[TbdViewController alloc] init];
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = _controller;
    [self.window makeKeyAndVisible];
}

- (void)sceneDidBecomeActive:(UIScene*)scene { [_controller resumeFrames]; }
- (void)sceneWillResignActive:(UIScene*)scene { [_controller pauseFrames]; }

- (void)sceneDidDisconnect:(UIScene*)scene {
    [_controller pauseFrames];
    tbd::host::Shutdown();
    self.window = nil;
    _controller = nil;
}
@end

@interface TbdAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation TbdAppDelegate
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([TbdAppDelegate class]));
    }
}
