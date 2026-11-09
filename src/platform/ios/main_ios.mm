// iOS shell: a UIKit scene with a CAMetalLayer view, driving the game without the editor (src/game/game_host.h).
// The display link runs the frames while the scene is active; touches become the game's pointer input.
//
// Touch scheme (CS2 / Maps style; the city camera's limits are in src/city_camera.h):
//   one finger on the GUI, or with a tool that draws  the left button (press, drag, release): roads / rails, zoning, utility
//                                                    lines, transit lines, terrain, bulldoze's Zone area box, the rotate dial;
//   one finger on the city otherwise                 a drag pans (the ground follows the finger); only a TAP acts: it lifts
//                                                    within 0.45 s and never travels 10 pt (src/game/touch_gesture.h; a
//                                                    slow press that does not move does nothing). That covers no tool (a tap
//                                                    opens the info card of what is under it), bulldoze's Tap mode (the first
//                                                    tap marks the target red, a second tap on it or the check demolishes) and
//                                                    a BUILDING being placed (utilities, services, transport, industry, waste;
//                                                    roads and rails have the road tool's aiming): the ghost waits in the middle
//                                                    of the view, a tap puts it where the finger landed, a tap on the ghost or
//                                                    the placement bar's check builds it. A pan never builds anything.
//   (game_host.cpp TouchBuilds asks city_interact.inl TouchFingerDraws which of the two a tool is.)
//   one finger with the road / rail aim dot out      [road ux] a drag pans the map under the screen-centre dot and a tap
//                                                    does nothing; only the dot's tick places (TouchBuilds says camera);
//   two fingers, after 12 pt of movement, lock into ONE of two gestures until a finger lifts:
//     TILT   both fingers move the same way vertically (each mostly vertical, their spacing and angle nearly unchanged):
//            fingers up = tilt toward the horizon / street level, fingers down = toward straight down (top-down);
//     MOVE   anything else: pinch zooms (spread = closer, the whole range from street level to the whole map), twist turns
//            the camera, and the fingers' centre pans - so a pan with a tool out still works (two fingers, not straight up/down).
// Why a lock and not "centre vertical = tilt" at all times: a two-finger pan always has some vertical centre movement, so
// tilt and pan would fight on every drag. Locking on the first 12 pt decides once and is never ambiguous afterwards; a pure
// vertical two-finger drag is tilt (as in Apple / Google Maps), pans use one finger, or two fingers off the vertical.
// Turning a building while it is placed is NOT a twist: a twist always turns the camera (above). The placement bar's Rotate
// toggle does it: the ghost locks, a ring shows its old facing (green) and its new one (blue) with the degrees, and ONE finger
// dragging anywhere turns it by the change of its bearing around the ghost; the Snap switch (shown every time) picks 15 degree
// steps (90 on a road) or free; the check keeps it, the cross goes back (src/city_interact.inl). On the PC also R / Shift+R.
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

#include "../../game/game_host.h"
#include "../../game/touch_gesture.h"
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
    UITouch* _first;      // the finger that is the left button (or, _camera, the one-finger pan)
    BOOL _leftDown;
    BOOL _camera;         // the first finger landed on the city with no tool: it pans once it moves, else it was a tap
    BOOL _panning;        // ... and it has moved: the middle button is down
    BOOL _toolDrag;       // the left button is down on the city with a build tool (not on the GUI): edge pan applies to it too
    CGPoint _fingerNow;   // the one dragging finger, device pixels, for the edge pan each frame
    CGPoint _start;       // where it landed, device pixels
    NSTimeInterval _startTime; // when it landed (a tap lifts within tbd::touch::kTapMaxSeconds)
    CGFloat _travel;      // the farthest it got from _start, device pixels
    CGFloat _twistAngle;  // the two fingers' angle last move
    BOOL _twoFinger;      // two fingers on the camera
    int _twoMode;         // 0 undecided (under the slop), 1 TILT, 2 MOVE (pan + pinch + twist; the middle button is down)
    UITouch* _ta;         // the two fingers, by identity (a touch set has no stable order)
    UITouch* _tb;
    CGPoint _a0, _b0;     // where they were when the second landed, device pixels
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

// The screen's native pixels per point (2 on iPad, 3 on recent iPhones). A UIView whose layer is a CAMetalLayer and that has
// no drawRect: keeps contentScaleFactor 1.0 unless told otherwise, so the drawable was sized in points and iOS stretched the
// whole frame, GUI text included, 2x: that was the blurry GUI. The view's scale is set from the screen here, every time.
- (CGFloat)pixelScale {
    CGFloat s = 0.0;
    if (UIWindowScene* scene = self.view.window.windowScene) s = scene.screen.nativeScale;
    if (s <= 0.0) s = self.traitCollection.displayScale;
    if (s <= 0.0) s = 1.0;
    if (self.view.contentScaleFactor != s) self.view.contentScaleFactor = s;
    CAMetalLayer* layer = (CAMetalLayer*)self.view.layer;
    if (layer.contentsScale != s) layer.contentsScale = s;
    return s;
}

- (CGPoint)pixels:(CGPoint)p {
    const CGFloat s = [self pixelScale];
    return CGPointMake(p.x * s, p.y * s);
}

// [safe area] The view's safe area (the Dynamic Island / notch, the home indicator; landscape iPad: left, right and top)
// to the GUI in device pixels, with the display's corner radius. UIKit has no public corner radius: a screen with a home
// indicator (any bottom or side inset) has rounded corners, about 18 pt on iPad and 47 pt on a notched / island iPhone.
- (void)pushSafeArea {
    const CGFloat s = [self pixelScale];
    const UIEdgeInsets e = self.view.safeAreaInsets;
    const BOOL rounded = e.bottom > 0 || e.left > 0 || e.right > 0;
    const BOOL pad = self.traitCollection.userInterfaceIdiom == UIUserInterfaceIdiomPad;
    const CGFloat corner = rounded ? (pad ? 18.0 : 47.0) : 0.0;
    tbd::host::SetSafeArea((float)(e.left * s), (float)(e.top * s), (float)(e.right * s), (float)(e.bottom * s), (float)(corner * s));
}

- (void)viewSafeAreaInsetsDidChange {
    [super viewSafeAreaInsetsDidChange];
    if (_started) [self pushSafeArea];
}

// First frame with a window and a size: the game, its Metal renderer and a new city.
- (BOOL)startGame {
    CAMetalLayer* layer = (CAMetalLayer*)self.view.layer;
    const CGFloat scale = [self pixelScale];
    const int w = (int)(self.view.bounds.size.width * scale), h = (int)(self.view.bounds.size.height * scale);
    if (!self.view.window || w <= 0 || h <= 0) return NO;
    layer.contentsScale = scale;

    // The game's files live in Documents (loaded onto the device through the Files app, Finder / Apple Devices file sharing or
    // tools/ipad_sync: docs/ipad_loader.md), so new content needs no new build. Documents/assets is the assets root; Documents/bin
    // stands in for the Windows exe folder (ExeDirectory() + "\\..\\assets"). The few files built into the app (the GUI atlas, the UI
    // tick) are copied in when missing, so the app starts with its GUI before anything is loaded.
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* docs = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString* docAssets = [docs stringByAppendingPathComponent:@"assets"];
    for (NSString* sub in @[ @"bin", @"assets", @"shaders", @"saves" ])
        [fm createDirectoryAtPath:[docs stringByAppendingPathComponent:sub] withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* bundled = [[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:@"assets"];
    NSDirectoryEnumerator* walk = [fm enumeratorAtPath:bundled];
    for (NSString* rel in walk) {
        if (![walk.fileAttributes.fileType isEqualToString:NSFileTypeRegular]) continue;
        NSString* to = [docAssets stringByAppendingPathComponent:rel];
        if ([fm fileExistsAtPath:to]) continue; // what was loaded onto the device wins
        [fm createDirectoryAtPath:[to stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
        [fm copyItemAtPath:[bundled stringByAppendingPathComponent:rel] toPath:to error:nil];
    }
    const std::wstring assets = tbd::platform::Utf8ToWide(std::string(docAssets.fileSystemRepresentation));
    tbd::Log("iOS: assets from %s", docAssets.fileSystemRepresentation);
    // Sound: the Playback category, so the Ring / Silent switch does not mute the game (miniaudio asks for it too, src/audio.cpp).
    {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* err = nil;
        if (![session setCategory:AVAudioSessionCategoryPlayback error:&err]) tbd::Log("iOS: audio session category failed");
        [session setActive:YES error:nil];
        [[NSNotificationCenter defaultCenter] addObserverForName:AVAudioSessionInterruptionNotification object:session
            queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification* note) {
                const NSInteger type = [note.userInfo[AVAudioSessionInterruptionTypeKey] integerValue];
                if (type != AVAudioSessionInterruptionTypeEnded) return;
                [[AVAudioSession sharedInstance] setActive:YES error:nil];
                tbd::host::ResumeAudio();
            }];
    }
    tbd::host::Init(assets); // also starts the audio thread on <assets>/audio (the bundled UI sfx were copied there above)

    auto renderer = tbd::CreateMetalRenderer();
    if (!renderer || !renderer->Init((__bridge void*)layer, w, h, false)) {
        tbd::Log("iOS: the Metal renderer failed to start");
        _failed = YES;
        [self pauseFrames];
        return NO;
    }
    tbd::host::AttachRenderer(std::move(renderer), w, h);
    tbd::host::Resize(w, h, (float)scale);
    [self pushSafeArea]; // [safe area]
    tbd::host::StartCity();
    tbd::Log("iOS: city started (%d x %d, scale %.1f)", w, h, (double)scale);
    return YES;
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (!_started) return;
    const CGFloat scale = [self pixelScale];
    tbd::host::Resize((int)(self.view.bounds.size.width * scale), (int)(self.view.bounds.size.height * scale), (float)scale);
    [self pushSafeArea]; // [safe area] a rotation moves the island from one side to the other
}

- (void)frame:(CADisplayLink*)link {
    if (!_started) {
        if (![self startGame]) return;
        _started = YES;
    }
    // Edge pan: a finger left in the screen's edge band while it drags keeps the camera going, faster the nearer the edge, as
    // if it kept dragging that way (no touch events arrive once a finger stops). Only for the camera's one-finger pan and a
    // build tool's drag; never for the GUI or two fingers.
    if ((_panning || _toolDrag) && _first && !_twoFinger) {
        const CGFloat s = [self pixelScale], W = self.view.bounds.size.width * s, H = self.view.bounds.size.height * s;
        const CGFloat band = 44.0 * s;
        auto push = [&](CGFloat v, CGFloat extent) -> CGFloat { // -1 .. 1: how far into the near or far edge band (0 in the middle)
            if (v < band) return -(1.0 - std::max<CGFloat>(v, 0.0) / band);
            if (v > extent - band) return 1.0 - (extent - std::min<CGFloat>(v, extent)) / band;
            return 0.0;
        };
        const CGFloat px = push(_fingerNow.x, W), py = push(_fingerNow.y, H);
        if (px != 0.0 || py != 0.0) {
            const CGFloat dt = std::min<CGFloat>(link.targetTimestamp - link.timestamp, 0.05), speed = 900.0 * s; // device px/s at the very edge
            tbd::host::EdgePan((float)(px * speed * dt), (float)(py * speed * dt));
            if (_toolDrag) tbd::host::PointerMove((float)_fingerNow.x, (float)_fingerNow.y); // the tool's ground point follows the moving view
        }
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
        _leftDown = _toolDrag = NO;
    }
    if (_panning) { // the one-finger pan hands over to the two-finger one
        tbd::host::PointerUp(tbd::host::Button::Middle, (float)_panCentre.x, (float)_panCentre.y);
        _panning = NO;
    }
    _camera = NO;
    _ta = touches[0];
    _tb = touches[1];
    const CGPoint a = [self pixels:[_ta locationInView:self.view]], b = [self pixels:[_tb locationInView:self.view]];
    _a0 = a, _b0 = b;
    _panCentre = CGPointMake((a.x + b.x) * 0.5, (a.y + b.y) * 0.5);
    _pinchDistance = std::hypot(a.x - b.x, a.y - b.y);
    _twistAngle = std::atan2(b.y - a.y, b.x - a.x);
    _twoFinger = YES;
    _twoMode = 0; // the middle button goes down only once the gesture is known to be MOVE
}

- (void)endTwoFinger {
    if (!_twoFinger) return;
    if (_twoMode == 2) tbd::host::PointerUp(tbd::host::Button::Middle, (float)_panCentre.x, (float)_panCentre.y);
    _twoFinger = NO;
    _twoMode = 0;
    _ta = _tb = nil;
}

static CGFloat WrapAngle(CGFloat a) {
    while (a > M_PI) a -= 2 * M_PI;
    while (a < -M_PI) a += 2 * M_PI;
    return a;
}

// Two fingers at device pixels a, b: decide the gesture once they have moved enough, then drive it.
- (void)twoFingerMoveA:(CGPoint)a b:(CGPoint)b {
    const CGPoint centre = CGPointMake((a.x + b.x) * 0.5, (a.y + b.y) * 0.5);
    const CGFloat d = std::hypot(a.x - b.x, a.y - b.y), ang = std::atan2(b.y - a.y, b.x - a.x);
    if (_twoMode == 0) {
        const CGFloat s = [self pixelScale], slop = 12.0 * s;
        const CGPoint da = CGPointMake(a.x - _a0.x, a.y - _a0.y), db = CGPointMake(b.x - _b0.x, b.y - _b0.y);
        const CGPoint c0 = CGPointMake((_a0.x + _b0.x) * 0.5, (_a0.y + _b0.y) * 0.5);
        const CGFloat cdy = centre.y - c0.y, cmove = std::hypot(centre.x - c0.x, cdy);
        const CGFloat spread = std::fabs(d - _pinchDistance), arc = std::fabs(WrapAngle(ang - _twistAngle)) * _pinchDistance * 0.5;
        if (std::max(cmove, std::max(spread, arc)) < slop) return; // not yet: nothing moves
        const bool vertical = da.y * db.y > 0.0 && std::fabs(da.y) > 1.5 * std::fabs(da.x) && std::fabs(db.y) > 1.5 * std::fabs(db.x);
        if (vertical && spread < 0.5 * std::fabs(cdy) && arc < 0.5 * std::fabs(cdy)) {
            _twoMode = 1; // TILT, from where the fingers started (the slop's movement is not lost)
            _panCentre = c0;
        } else {
            _twoMode = 2; // MOVE: the middle button from the start centre, so the slop's pan, pinch and twist all apply now
            _panCentre = c0;
            tbd::host::PointerDown(tbd::host::Button::Middle, (float)c0.x, (float)c0.y);
        }
    }
    if (_twoMode == 1) {
        // about 1.65 rad (straight down to level) over 370 pt of finger travel
        const CGFloat perPx = 0.0045 / [self pixelScale];
        const CGFloat dy = centre.y - _panCentre.y;
        if (std::fabs(dy) > 0.0) tbd::host::Tilt((float)(dy * perPx));
        _panCentre = centre;
        return;
    }
    _panCentre = centre;
    tbd::host::PointerMove((float)centre.x, (float)centre.y);
    if (_pinchDistance > 1.0 && d > 1.0) {
        // The wheel zooms the camera distance by 0.9^(0.5 n); spreading the fingers by f should divide it by f.
        const float notches = (float)(-2.0 * std::log(d / _pinchDistance) / std::log(0.9));
        if (std::fabs(notches) > 0.01f) tbd::host::Zoom(notches, (float)centre.x, (float)centre.y);
    }
    _pinchDistance = d;
    const CGFloat da = WrapAngle(ang - _twistAngle);
    if (std::fabs(da) > 0.002) tbd::host::Twist((float)da);
    _twistAngle = ang;
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
    if (!tbd::host::TouchBuilds((float)p.x, (float)p.y)) { // the city, no tool: wait and see whether it is a tap or a pan
        _camera = YES;
        _panning = NO;
        _start = p;
        _startTime = t.timestamp;
        _travel = 0;
        return;
    }
    tbd::host::PointerDown(tbd::host::Button::Left, (float)p.x, (float)p.y);
    _leftDown = YES;
    _toolDrag = !tbd::host::TouchOnGui((float)p.x, (float)p.y); // a tool on the city: edge pan may carry its drag
    _fingerNow = p;
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_started) return;
    if (_twoFinger) {
        if (!_ta || !_tb) return;
        [self twoFingerMoveA:[self pixels:[_ta locationInView:self.view]] b:[self pixels:[_tb locationInView:self.view]]];
        return;
    }
    if (_camera && [touches containsObject:_first]) {
        const CGPoint p = [self pixels:[_first locationInView:self.view]];
        _travel = std::max(_travel, (CGFloat)std::hypot(p.x - _start.x, p.y - _start.y));
        const float travelPt = tbd::touch::TravelPt((float)_travel, 0.0f, (float)[self pixelScale]);
        if (!_panning && tbd::touch::Classify(travelPt, 0.0, false) == tbd::touch::Gesture::Drag) { // 10 pt: a pan, never a tap
            _panning = YES;
            _panCentre = _start;
            tbd::host::PointerDown(tbd::host::Button::Middle, (float)_start.x, (float)_start.y);
        }
        if (_panning) _panCentre = p, _fingerNow = p, tbd::host::PointerMove((float)p.x, (float)p.y);
        return;
    }
    if (_leftDown && [touches containsObject:_first]) {
        const CGPoint p = [self pixels:[_first locationInView:self.view]];
        _fingerNow = p;
        tbd::host::PointerMove((float)p.x, (float)p.y);
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_started) return;
    if (_twoFinger) {
        if ([touches containsObject:_ta] || [touches containsObject:_tb] || [self activeTouches:event].count < 2) [self endTwoFinger];
        return;
    }
    if (_camera && [touches containsObject:_first]) {
        const CGPoint p = [self pixels:[_first locationInView:self.view]];
        if (_panning) tbd::host::PointerUp(tbd::host::Button::Middle, (float)p.x, (float)p.y);
        else {
            _travel = std::max(_travel, (CGFloat)std::hypot(p.x - _start.x, p.y - _start.y));
            const float travelPt = tbd::touch::TravelPt((float)_travel, 0.0f, (float)[self pixelScale]);
            if (tbd::touch::Classify(travelPt, _first.timestamp - _startTime, true) == tbd::touch::Gesture::Tap) {
                // a tap: a click where it landed (info, bulldoze's mark, the placement ghost: src/city_interact.inl)
                tbd::host::PointerDown(tbd::host::Button::Left, (float)_start.x, (float)_start.y);
                tbd::host::PointerUp(tbd::host::Button::Left, (float)_start.x, (float)_start.y);
            } // else a hold: nothing
        }
        _camera = _panning = NO;
        _first = nil;
        return;
    }
    if (_leftDown && [touches containsObject:_first]) {
        const CGPoint p = [self pixels:[_first locationInView:self.view]];
        tbd::host::PointerUp(tbd::host::Button::Left, (float)p.x, (float)p.y);
        _leftDown = _toolDrag = NO;
        _first = nil;
    }
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_started) return;
    tbd::host::PointerCancel();
    _leftDown = _toolDrag = NO;
    _twoFinger = NO;
    _twoMode = 0;
    _ta = _tb = nil;
    _camera = _panning = NO;
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

- (void)sceneDidBecomeActive:(UIScene*)scene {
    [_controller resumeFrames];
    [[AVAudioSession sharedInstance] setActive:YES error:nil];
    tbd::host::ResumeAudio(); // no-op before the game started or while the device runs
}
- (void)sceneWillResignActive:(UIScene*)scene { [_controller pauseFrames]; }
- (void)sceneDidEnterBackground:(UIScene*)scene { tbd::host::EnterBackground(); } // the city is autosaved now

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
