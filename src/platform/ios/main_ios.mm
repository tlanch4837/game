// iOS app shell: STUB. It brings up a full-screen CAMetalLayer, creates the Metal renderer and drives it from a
// CADisplayLink with an empty scene (a sky-coloured clear through the whole post chain), so the backend can be brought
// up on a device before the game is ported. Still to come in its place (src/main.cpp is the Win32 shell it replaces):
// touch input mapped to the camera and tools, the in-game GUI, audio (AVAudioEngine instead of XAudio2/WASAPI), the
// game loop and world, asset loading from the bundle, and app lifecycle (pause in background, low-memory warnings).
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <memory>

#include "../../render/renderer.h"

float g_brightness = 1.4f;    // main.cpp's defaults; the renderer reads them
float g_sceneExposure = 0.0f;

@interface TbdMetalView : UIView
@end
@implementation TbdMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

@interface TbdViewController : UIViewController
@end

@implementation TbdViewController {
    std::unique_ptr<tbd::Renderer> _renderer;
    CADisplayLink* _link;
    double _start;
}

- (void)loadView {
    self.view = [[TbdMetalView alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.view.contentScaleFactor = [UIScreen mainScreen].nativeScale;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (_renderer) return;
    CAMetalLayer* layer = (CAMetalLayer*)self.view.layer;
    const CGFloat scale = self.view.contentScaleFactor;
    const int w = (int)(self.view.bounds.size.width * scale), h = (int)(self.view.bounds.size.height * scale);
    _renderer = tbd::CreateMetalRenderer();
    if (!_renderer->Init((__bridge void*)layer, w, h, false)) {
        tbd::Log("iOS: the Metal renderer failed to start");
        _renderer.reset();
        return;
    }
    _start = CACurrentMediaTime();
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame:)];
    _link.preferredFrameRateRange = CAFrameRateRangeMake(30, 120, 60);
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (!_renderer) return;
    const CGFloat scale = self.view.contentScaleFactor;
    _renderer->Resize((int)(self.view.bounds.size.width * scale), (int)(self.view.bounds.size.height * scale));
}

- (void)frame:(CADisplayLink*)link {
    if (!_renderer) return;
    tbd::FrameView view;
    view.viewProj = tbd::Mat4::Identity();
    view.invJittered = tbd::Mat4::Identity();
    view.time = (float)(CACurrentMediaTime() - _start);
    view.dt = (float)link.duration;
    view.sky = false; // no camera yet: the physical sky needs a real view
    _renderer->SetFrameView(view);
    const float clear[4] = {0.38f, 0.47f, 0.61f, 1.0f};
    _renderer->BeginFrame(clear);
    _renderer->ResolveScene();
    _renderer->EndFrame();
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
@end

@interface TbdAppDelegate : UIResponder <UIApplicationDelegate>
@property(strong, nonatomic) UIWindow* window;
@end

@implementation TbdAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.rootViewController = [[TbdViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([TbdAppDelegate class]));
    }
}
