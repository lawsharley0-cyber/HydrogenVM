// HydrogenVM UIKit test app: launch sequence, view geometry and hierarchy,
// drawing, images, nibs (both formats), controls, gestures, touch delivery
// (driven by `hvm run --tap`), display links and animation callbacks.
#import <UIKit/UIKit.h>
#import <OpenGLES/EAGL.h>
#include <OpenGLES/ES2/gl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0, checks = 0;
#define CHECK(cond, what) do { ++checks; if (cond) printf("ok %s\n", what); else { ++failures; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)
static BOOL near(CGFloat a, CGFloat b) { return a - b < 0.01f && b - a < 0.01f; }
static BOOL rectEq(CGRect a, CGRect b) {
    return near(a.origin.x, b.origin.x) && near(a.origin.y, b.origin.y) && near(a.size.width, b.size.width) && near(a.size.height, b.size.height);
}

static int drawCount = 0, layoutCount = 0, touchesBegan = 0, touchesEnded = 0, tapGestures = 0, linkFires = 0;
static int buttonTaps = 0, nibTaps = 0, animDone = 0, animStop = 0, viewDidLoadCount = 0, viewWillAppearCount = 0, movedToWindow = 0;
static CGPoint lastTouch;

@interface DrawView : UIView
@end
@implementation DrawView
- (void)drawRect:(CGRect)rect {
    drawCount++;
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    CGContextSetRGBFillColor(ctx, 1, 0.5, 0, 1);
    CGContextFillRect(ctx, CGRectMake(10, 10, 60, 30));
    CGContextSetRGBStrokeColor(ctx, 0, 0, 1, 1);
    CGContextSetLineWidth(ctx, 3);
    CGContextBeginPath(ctx);
    CGContextAddArc(ctx, 120, 30, 20, 0, 6.2831853f, 0);
    CGContextStrokePath(ctx);
    [[UIColor whiteColor] set];
    [@"Hydrogen" drawAtPoint:CGPointMake(10, 50) withFont:[UIFont boldSystemFontOfSize:16]];
}
@end

@interface LayoutView : UIView
@end
@implementation LayoutView
- (void)layoutSubviews { [super layoutSubviews]; layoutCount++; }
- (void)didMoveToWindow { if (self.window) movedToWindow++; }
@end

@interface TapView : UIView
@end
@implementation TapView
- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event { touchesBegan++; lastTouch = [[touches anyObject] locationInView:self]; }
- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event { touchesEnded++; }
@end

@interface TestViewController : UIViewController
@property (nonatomic, strong) UILabel *panelLabel;
@property (nonatomic, strong) LayoutView *container;
@property (nonatomic, strong) UIView *child;
@property (nonatomic, strong) UIButton *button;
@property (nonatomic, strong) TapView *gestureView;
@end

@implementation TestViewController
- (void)loadView {
    UIView *root = [[UIView alloc] initWithFrame:[UIScreen mainScreen].applicationFrame];
    root.backgroundColor = [UIColor colorWithRed:0.1 green:0.12 blue:0.2 alpha:1];
    self.view = root;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    viewDidLoadCount++;
    self.container = [[LayoutView alloc] initWithFrame:CGRectMake(20, 20, 200, 100)];
    self.container.backgroundColor = [UIColor darkGrayColor];
    [self.view addSubview:self.container];
    self.child = [[UIView alloc] initWithFrame:CGRectMake(10, 10, 50, 50)];
    self.child.backgroundColor = [UIColor redColor];
    self.child.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin;
    [self.container addSubview:self.child];

    DrawView *dv = [[DrawView alloc] initWithFrame:CGRectMake(20, 140, 180, 80)];
    dv.backgroundColor = [UIColor colorWithWhite:0.25 alpha:1];
    [self.view addSubview:dv];

    UILabel *label = [[UILabel alloc] initWithFrame:CGRectMake(20, 230, 280, 24)];
    label.text = @"Hello from UIKit";
    label.textColor = [UIColor yellowColor];
    label.backgroundColor = [UIColor clearColor];
    [self.view addSubview:label];

    self.button = [UIButton buttonWithType:UIButtonTypeRoundedRect];
    self.button.frame = CGRectMake(200, 140, 100, 37);
    [self.button setTitle:@"Tap me" forState:UIControlStateNormal];
    [self.button addTarget:self action:@selector(buttonTapped:) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:self.button];

    UIImageView *iv = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"test"]];
    iv.frame = CGRectMake(240, 200, 40, 20);
    [self.view addSubview:iv];

    self.gestureView = [[TapView alloc] initWithFrame:CGRectMake(20, 300, 100, 60)];
    self.gestureView.backgroundColor = [UIColor blueColor];
    [self.view addSubview:self.gestureView];
    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapped:)];
    [self.gestureView addGestureRecognizer:tap];

    UIView *plain = [[TapView alloc] initWithFrame:CGRectMake(180, 300, 100, 60)];
    plain.backgroundColor = [UIColor greenColor];
    plain.tag = 99;
    [self.view addSubview:plain];
}
- (void)viewWillAppear:(BOOL)animated { [super viewWillAppear:animated]; viewWillAppearCount++; }
- (void)buttonTapped:(id)sender { buttonTaps++; }
- (void)nibButtonTapped:(id)sender { nibTaps++; }
- (void)tapped:(UITapGestureRecognizer *)g { if (g.state == UIGestureRecognizerStateEnded) tapGestures++; }
- (void)animationDidStop:(NSString *)animID finished:(NSNumber *)finished context:(void *)ctx { if ([animID isEqualToString:@"move"]) animStop++; }
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) TestViewController *vc;
@property (nonatomic, strong) CADisplayLink *link;
@end

@implementation AppDelegate
- (void)frame:(CADisplayLink *)link { linkFires++; }

static unsigned pixelAt(CGImageRef img, int x, int y) {
    CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(img));
    const uint8_t *p = CFDataGetBytePtr(data) + (y * CGImageGetWidth(img) + x) * 4;
    unsigned v = (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
    CFRelease(data);
    return v;
}

- (void)runSynchronousChecks {
    CGRect screen = [UIScreen mainScreen].bounds;
    CHECK(rectEq(screen, CGRectMake(0, 0, 320, 480)), "3.5-inch screen without a 568h launch image");
    CHECK(rectEq([UIScreen mainScreen].applicationFrame, CGRectMake(0, 20, 320, 460)), "applicationFrame below the status bar");
    CHECK([UIScreen mainScreen].scale == 2, "retina scale");
    CHECK([[UIApplication sharedApplication] keyWindow] == self.window, "keyWindow");
    CHECK(rectEq(self.vc.view.frame, CGRectMake(0, 20, 320, 460)), "root view controller view frame");
    CHECK(viewDidLoadCount == 1 && viewWillAppearCount == 1, "viewDidLoad / viewWillAppear");
    CHECK(movedToWindow == 1, "didMoveToWindow");
    CHECK([self.vc.view nextResponder] == self.vc && [self.vc nextResponder] == self.window, "responder chain");
    CHECK([[UIDevice currentDevice].systemVersion isEqualToString:@"6.1"], "UIDevice systemVersion");

    // geometry
    UIView *child = self.vc.child;
    CGPoint p = [child convertPoint:CGPointMake(5, 5) toView:nil];
    CHECK(near(p.x, 35) && near(p.y, 55), "convertPoint:toView:nil (window)");
    CGPoint q = [self.vc.view convertPoint:CGPointMake(35, 55) fromView:nil];
    CHECK(near(q.x, 35) && near(q.y, 35), "convertPoint:fromView:nil");
    CHECK([self.vc.view hitTest:CGPointMake(35, 35) withEvent:nil] == child, "hitTest finds the child");
    CHECK([self.vc.view hitTest:CGPointMake(310, 10) withEvent:nil] == self.vc.view, "hitTest falls back to the root");
    CHECK([self.vc.view viewWithTag:99] != nil && [child isDescendantOfView:self.vc.view], "viewWithTag / isDescendantOfView");
    child.transform = CGAffineTransformMakeScale(2, 2);
    CHECK(rectEq(child.frame, CGRectMake(-15, -15, 100, 100)), "transformed frame");
    child.transform = CGAffineTransformIdentity;
    CHECK(rectEq(child.frame, CGRectMake(10, 10, 50, 50)), "identity transform restores frame");
    self.vc.container.frame = CGRectMake(20, 20, 300, 150);
    CHECK(rectEq(child.frame, CGRectMake(10, 60, 150, 50)), "autoresizing (flexible width + top margin)");
    int before = layoutCount;
    [self.vc.container setNeedsLayout];
    [self.vc.container layoutIfNeeded];
    CHECK(layoutCount == before + 1, "setNeedsLayout + layoutIfNeeded");
    [self.vc.container bringSubviewToFront:child];
    CHECK([self.vc.container.subviews lastObject] == child, "bringSubviewToFront");

    // colours, fonts, text
    CGFloat r, g, b, a;
    CHECK([[UIColor orangeColor] getRed:&r green:&g blue:&b alpha:&a] && near(r, 1) && near(g, 0.5f) && near(b, 0), "UIColor components");
    const CGFloat *comps = CGColorGetComponents([UIColor blueColor].CGColor);
    CHECK(comps && near(comps[2], 1), "UIColor CGColor");
    UIFont *font = [UIFont systemFontOfSize:17];
    CHECK(font.pointSize == 17 && font.lineHeight > 10, "UIFont metrics");
    CGSize ts = [@"Hello" sizeWithFont:font];
    CGSize ts2 = [@"Hello Hello" sizeWithFont:font];
    CHECK(ts.width > 10 && ts.height > 10 && ts2.width > ts.width, "sizeWithFont:");
    CGSize wrapped = [@"one two three four five six seven" sizeWithFont:font constrainedToSize:CGSizeMake(60, 1000)];
    CHECK(wrapped.width <= 60 && wrapped.height > font.lineHeight * 2, "sizeWithFont:constrainedToSize: wraps");

    // images
    UIImage *img = [UIImage imageNamed:@"test"];
    CHECK(img && near(img.size.width, 40) && near(img.size.height, 20) && img.scale == 2, "imageNamed: picks @2x");
    CHECK(CGImageGetWidth(img.CGImage) == 80, "CGImage of @2x image");
    CHECK([UIImage imageNamed:@"does-not-exist"] == nil, "imageNamed: missing returns nil");
    UIImage *crushed = [UIImage imageNamed:@"crushed"];
    CHECK(crushed != nil, "CgBI (Xcode-crushed) PNG decodes");
    if (crushed) {
        CHECK(pixelAt(crushed.CGImage, 0, 0) == 0xFF0000FFu && pixelAt(crushed.CGImage, 1, 0) == 0x00FF00FFu && pixelAt(crushed.CGImage, 0, 1) == 0x0000FFFFu,
              "CgBI channel order");
    }
    UIGraphicsBeginImageContextWithOptions(CGSizeMake(10, 10), NO, 1);
    [[UIColor redColor] setFill];
    UIRectFill(CGRectMake(0, 0, 5, 10));
    [img drawInRect:CGRectMake(5, 0, 5, 5)];
    UIImage *drawn = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    CHECK(drawn && near(drawn.size.width, 10), "UIGraphicsGetImageFromCurrentImageContext");
    CHECK(pixelAt(drawn.CGImage, 1, 1) == 0xFF0000FFu, "UIRectFill pixels");
    CHECK(pixelAt(drawn.CGImage, 7, 2) == 0x00C800FFu, "drawInRect: pixels (top-left origin)");
    CHECK(pixelAt(drawn.CGImage, 7, 7) == 0, "untouched pixels stay clear");
    NSData *pngData = UIImagePNGRepresentation(drawn);
    UIImage *reloaded = [UIImage imageWithData:pngData];
    CHECK(reloaded && near(reloaded.size.width, 10) && pixelAt(reloaded.CGImage, 1, 1) == 0xFF0000FFu, "PNG encode/decode round trip");

    // CoreGraphics bitmap context (bottom-left origin)
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    uint8_t pixels[4 * 4 * 4];
    memset(pixels, 0, sizeof pixels);
    CGContextRef bm = CGBitmapContextCreate(pixels, 4, 4, 8, 16, cs, kCGImageAlphaPremultipliedLast);
    CGContextSetRGBFillColor(bm, 0, 0, 1, 1);
    CGContextFillRect(bm, CGRectMake(0, 0, 4, 2));   // bottom half in CG coordinates = rows 2..3 in memory
    CHECK(pixels[0 * 16 + 2] == 0 && pixels[3 * 16 + 2] == 255 && pixels[3 * 16 + 3] == 255, "CGBitmapContext y-up fill");
    CGMutablePathRef path = CGPathCreateMutable();
    CGPathAddEllipseInRect(path, NULL, CGRectMake(0, 0, 10, 10));
    CHECK(CGPathContainsPoint(path, NULL, CGPointMake(5, 5), false) && !CGPathContainsPoint(path, NULL, CGPointMake(0.5f, 0.5f), false), "CGPathContainsPoint");
    CGPathRelease(path);
    CGContextRelease(bm);
    CGColorSpaceRelease(cs);
    CGRect inter = CGRectIntersection(CGRectMake(0, 0, 10, 10), CGRectMake(5, 5, 10, 10));
    CHECK(rectEq(inter, CGRectMake(5, 5, 5, 5)) && CGRectIsNull(CGRectIntersection(CGRectMake(0, 0, 1, 1), CGRectMake(5, 5, 1, 1))), "CGRectIntersection");
    CHECK([NSStringFromCGRect(CGRectMake(1, 2, 3, 4)) isEqualToString:@"{{1, 2}, {3, 4}}"] && rectEq(CGRectFromString(@"{{1, 2}, {3, 4}}"), CGRectMake(1, 2, 3, 4)),
          "NSStringFromCGRect / CGRectFromString");
    CHECK(rectEq([[NSValue valueWithCGRect:CGRectMake(1, 2, 3, 4)] CGRectValue], CGRectMake(1, 2, 3, 4)), "NSValue CGRect");

    // nibs, both compiled formats
    for (NSString *name in @[@"Panel", @"PanelKeyed"]) {
        self.vc.panelLabel = nil;
        NSArray *top = [[NSBundle mainBundle] loadNibNamed:name owner:self.vc options:nil];
        UIView *panel = top.count ? top[0] : nil;
        char msg[80];
        snprintf(msg, sizeof msg, "%s nib: top-level view", [name UTF8String]);
        CHECK(panel && panel.tag == 42 && rectEq(panel.frame, CGRectMake(60, 70, 200, 100)) && panel.subviews.count == 2, msg);
        snprintf(msg, sizeof msg, "%s nib: outlet + label text", [name UTF8String]);
        CHECK([self.vc.panelLabel.text isEqualToString:@"From nib"] && self.vc.panelLabel.tag == 7 && self.vc.panelLabel.superview == panel, msg);
        UIButton *nb = panel.subviews.count > 1 ? panel.subviews[1] : nil;
        int before2 = nibTaps;
        [nb sendActionsForControlEvents:UIControlEventTouchUpInside];
        snprintf(msg, sizeof msg, "%s nib: action connection + button title", [name UTF8String]);
        CHECK(nibTaps == before2 + 1 && [[nb titleForState:UIControlStateNormal] isEqualToString:@"Nib Button"], msg);
        if (panel && [name isEqualToString:@"Panel"]) [self.vc.view addSubview:panel];
    }

    // A missing nib is an NSInternalInconsistencyException the guest can catch.
    // Orientation words are not resource modifiers: PanelLandscape must not load Panel.
    BOOL missingNibRaised = NO, landscapeAliasRejected = NO, portraitAliasRejected = NO;
    @try {
        (void)[[NSBundle mainBundle] loadNibNamed:@"DefinitelyMissingNibThatDoesntExist" owner:nil options:nil];
    } @catch (NSException *e) {
        const char *reason = e.reason ? [e.reason UTF8String] : NULL;
        missingNibRaised = [e.name isEqualToString:@"NSInternalInconsistencyException"]
            && reason && strstr(reason, "Could not load NIB in bundle")
            && strstr(reason, "DefinitelyMissingNibThatDoesntExist");
    }
    CHECK(missingNibRaised, "loadNibNamed: missing NIB raises NSInternalInconsistencyException");
    @try {
        (void)[[NSBundle mainBundle] loadNibNamed:@"PanelLandscape" owner:self.vc options:nil];
    } @catch (NSException *e) {
        const char *reason = e.reason ? [e.reason UTF8String] : NULL;
        landscapeAliasRejected = [e.name isEqualToString:@"NSInternalInconsistencyException"]
            && reason && strstr(reason, "Could not load NIB in bundle")
            && strstr(reason, "PanelLandscape");
    }
    CHECK(landscapeAliasRejected, "loadNibNamed: does not strip Landscape to load Panel");
    @try {
        (void)[[NSBundle mainBundle] loadNibNamed:@"PanelPortrait" owner:self.vc options:nil];
    } @catch (NSException *e) {
        const char *reason = e.reason ? [e.reason UTF8String] : NULL;
        portraitAliasRejected = [e.name isEqualToString:@"NSInternalInconsistencyException"]
            && reason && strstr(reason, "Could not load NIB in bundle")
            && strstr(reason, "PanelPortrait");
    }
    CHECK(portraitAliasRejected, "loadNibNamed: does not strip Portrait to load Panel");

    // controls
    [self.vc.button sendActionsForControlEvents:UIControlEventTouchUpInside];
    CHECK(buttonTaps == 1, "UIButton target/action");
    CHECK([self.vc.button.currentTitle isEqualToString:@"Tap me"], "UIButton currentTitle");

    // animations
    [UIView animateWithDuration:0.1 animations:^{ child.alpha = 0.5; } completion:^(BOOL finished) { if (finished) animDone++; }];
    CHECK(near(child.alpha, 0.5f), "animation block applies immediately");
    [UIView beginAnimations:@"move" context:NULL];
    [UIView setAnimationDuration:0.1];
    [UIView setAnimationDelegate:self.vc];
    [UIView setAnimationDidStopSelector:@selector(animationDidStop:finished:context:)];
    child.center = CGPointMake(40, 40);
    [UIView commitAnimations];
    CHECK(near(child.center.x, 40), "beginAnimations/commitAnimations apply");

    // UIScrollView, UITableView, UINavigationController, UITabBarController, UITextField, UISwitch, UISlider, UISegmentedControl
    UIScrollView *sv = [[UIScrollView alloc] initWithFrame:CGRectMake(0, 0, 100, 100)];
    sv.contentSize = CGSizeMake(300, 500);
    [sv setContentOffset:CGPointMake(20, 40) animated:NO];
    CHECK(near(sv.contentOffset.x, 20) && near(sv.contentOffset.y, 40) && near(sv.bounds.origin.y, 40), "UIScrollView contentOffset updates bounds.origin");

    UITableView *tv = [[UITableView alloc] initWithFrame:CGRectMake(0, 0, 320, 200) style:0];
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:0 reuseIdentifier:@"Cell"];
    cell.textLabel.text = @"Row 0";
    CHECK([cell.reuseIdentifier isEqualToString:@"Cell"] && [cell.textLabel.text isEqualToString:@"Row 0"], "UITableViewCell textLabel & reuseIdentifier");
    (void)tv;

    UIViewController *rootVC = [[UIViewController alloc] initWithNibName:nil bundle:nil];
    rootVC.title = @"Home";
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:rootVC];
    UIViewController *detailVC = [[UIViewController alloc] initWithNibName:nil bundle:nil];
    detailVC.title = @"Detail";
    [nav pushViewController:detailVC animated:NO];
    CHECK(nav.topViewController == detailVC && nav.viewControllers.count == 2 && [detailVC.navigationController isEqual:nav], "UINavigationController push & hierarchy");
    [nav popViewControllerAnimated:NO];
    CHECK(nav.topViewController == rootVC && nav.viewControllers.count == 1, "UINavigationController pop");

    UITabBarController *tbc = [[UITabBarController alloc] init];
    tbc.viewControllers = @[rootVC, detailVC];
    tbc.selectedIndex = 1;
    CHECK(tbc.selectedViewController == detailVC && tbc.tabBar.items.count == 2, "UITabBarController selection");

    UITextField *tf = [[UITextField alloc] initWithFrame:CGRectMake(0, 0, 120, 30)];
    tf.placeholder = @"Enter name";
    tf.text = @"Hydrogen";
    CHECK([tf becomeFirstResponder] && tf.isEditing && [tf.text isEqualToString:@"Hydrogen"], "UITextField editing & first responder");
    [tf resignFirstResponder];

    UISwitch *sw = [[UISwitch alloc] initWithFrame:CGRectZero];
    [sw setOn:YES animated:NO];
    UISlider *sl = [[UISlider alloc] initWithFrame:CGRectMake(0, 0, 100, 23)];
    sl.minimumValue = 0; sl.maximumValue = 10; sl.value = 7.5f;
    UISegmentedControl *seg = [[UISegmentedControl alloc] initWithItems:@[@"One", @"Two"]];
    seg.selectedSegmentIndex = 1;
    CHECK(sw.isOn && near(sl.value, 7.5f) && seg.numberOfSegments == 2 && [[seg titleForSegmentAtIndex:1] isEqualToString:@"Two"], "UISwitch, UISlider, UISegmentedControl");

    // OpenGL ES 2.0 + CAEAGLLayer + EAGLContext
    CAEAGLLayer *glLayer = [CAEAGLLayer layer];
    glLayer.frame = CGRectMake(0, 0, 64, 64);
    EAGLContext *glCtx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    CHECK(glCtx != nil && [EAGLContext setCurrentContext:glCtx] && [EAGLContext currentContext] == glCtx, "EAGLContext creation & setCurrentContext");
    GLuint fbo = 0, rbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    CHECK([glCtx renderbufferStorage:GL_RENDERBUFFER fromDrawable:glLayer], "EAGLContext renderbufferStorage:fromDrawable:");
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "glCheckFramebufferStatus complete");
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    uint8_t glPx[4] = {0, 0, 0, 0};
    glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, glPx);
    CHECK(glPx[0] == 255 && glPx[1] == 0 && glPx[2] == 0 && glPx[3] == 255 && [glCtx presentRenderbuffer:GL_RENDERBUFFER], "OpenGL ES clear, glReadPixels & presentRenderbuffer");

    // display link
    self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame:)];
    [self.link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSDefaultRunLoopMode];
}

- (void)finish {
    // The scripted taps are timed from the first frame, which can come late
    // (simulator, slow CI machines): wait up to 5 s more for both of them.
    static int waits = 0;
    if ((touchesEnded < 1 || tapGestures < 1) && waits++ < 50) {
        [self performSelector:@selector(finish) withObject:nil afterDelay:0.1];
        return;
    }
    CHECK(drawCount >= 1, "drawRect: was called by the compositor");
    CHECK(linkFires >= 10, "CADisplayLink fires at frame rate");
    CHECK(animDone == 1 && animStop == 1, "animation completions fire");
    CHECK(touchesBegan >= 1 && touchesEnded >= 1, "touch delivery to a plain view (hvm --tap)");
    CHECK(near(lastTouch.x, 50) && near(lastTouch.y, 30), "touch location in view coordinates");
    CHECK(tapGestures == 1, "UITapGestureRecognizer");
    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) printf("ALL CHECKS PASSED\n");
    fflush(stdout);
    exit(failures ? 1 : 0);
}

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.backgroundColor = [UIColor blackColor];
    self.vc = [[TestViewController alloc] initWithNibName:nil bundle:nil];
    self.window.rootViewController = self.vc;
    [self.window makeKeyAndVisible];
    [self runSynchronousChecks];
    [self performSelector:@selector(finish) withObject:nil afterDelay:1.0];
    return YES;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
