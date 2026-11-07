// iOS smoke test: draw a procedural triangle through the native Metal renderer.
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <memory>
#include <utility>

#include "../../render/renderer.h"

float g_brightness = 1.4f;    // main.cpp's defaults; the renderer reads them
float g_sceneExposure = 0.0f;

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
    std::unique_ptr<tbd::Renderer> _renderer;
    tbd::MeshId _triangle;
    CADisplayLink* _link;
    double _start;
}

- (void)loadView {
    self.view = [[TbdMetalView alloc] initWithFrame:CGRectZero];
}

- (void)resumeFrames {
    if (_link) return;
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame:)];
    _link.preferredFrameRateRange = CAFrameRateRangeMake(30, 60, 60);
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)pauseFrames {
    [_link invalidate];
    _link = nil;
}

- (BOOL)startRenderer {
    CAMetalLayer* layer = (CAMetalLayer*)self.view.layer;
    const CGFloat scale = self.view.contentScaleFactor;
    const int w = (int)(self.view.bounds.size.width * scale), h = (int)(self.view.bounds.size.height * scale);
    if (!self.view.window || w <= 0 || h <= 0) return NO;

    auto renderer = tbd::CreateMetalRenderer();
    if (!renderer->Init((__bridge void*)layer, w, h, false)) {
        tbd::Log("iOS: the Metal renderer failed to start");
        [self pauseFrames];
        return NO;
    }
    const tbd::Vertex vertices[] = {
        {-0.42f, -0.55f, 0.5f, 0, 0, 0, 1.0f, 0.16f, 0.12f, 1},
        { 0.42f, -0.55f, 0.5f, 0, 0, 0, 0.10f, 0.92f, 0.36f, 1},
        { 0.00f,  0.62f, 0.5f, 0, 0, 0, 0.12f, 0.48f, 1.0f, 1},
    };
    _triangle = renderer->CreateMesh(vertices, 3, tbd::Topology::Triangles);
    if (_triangle == tbd::kInvalidMesh) {
        tbd::Log("iOS: could not upload the smoke-test triangle");
        [self pauseFrames];
        return NO;
    }
    _renderer = std::move(renderer);
    _start = CACurrentMediaTime();
    return YES;
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (!_renderer) return;
    const CGFloat scale = self.view.contentScaleFactor;
    _renderer->Resize((int)(self.view.bounds.size.width * scale), (int)(self.view.bounds.size.height * scale));
}

- (void)frame:(CADisplayLink*)link {
    if (!_renderer && ![self startRenderer]) return;
    tbd::FrameView view;
    view.viewProj = tbd::Mat4::Identity();
    view.invJittered = tbd::Mat4::Identity();
    view.time = (float)(CACurrentMediaTime() - _start);
    view.dt = (float)link.duration;
    view.sky = false; // this test uses clip-space vertices
    _renderer->SetFrameView(view);
    const float clear[4] = {0.38f, 0.47f, 0.61f, 1.0f};
    _renderer->BeginFrame(clear);
    _renderer->Draw(_triangle, view.viewProj, tbd::Mat4::Identity(), true, true);
    _renderer->ResolveScene();
    _renderer->EndFrame();
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
