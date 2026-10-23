// Minimal UIKit declarations for HydrogenVM's test apps (iOS 6 SDK signatures, 32-bit).
#pragma once
#import <Foundation/Foundation.h>
#include <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>

typedef struct UIEdgeInsets { CGFloat top, left, bottom, right; } UIEdgeInsets;
static inline UIEdgeInsets UIEdgeInsetsMake(CGFloat t, CGFloat l, CGFloat b, CGFloat r) { UIEdgeInsets i = {t, l, b, r}; return i; }
NSString *NSStringFromCGRect(CGRect r);
NSString *NSStringFromCGPoint(CGPoint p);
NSString *NSStringFromCGSize(CGSize s);
CGRect CGRectFromString(NSString *s);
int UIApplicationMain(int argc, char *argv[], NSString *principalClassName, NSString *delegateClassName);

typedef NSInteger UIInterfaceOrientation;
enum { UIInterfaceOrientationPortrait = 1, UIInterfaceOrientationPortraitUpsideDown = 2, UIInterfaceOrientationLandscapeLeft = 3, UIInterfaceOrientationLandscapeRight = 4 };
enum { UIViewAutoresizingNone = 0, UIViewAutoresizingFlexibleLeftMargin = 1, UIViewAutoresizingFlexibleWidth = 2, UIViewAutoresizingFlexibleRightMargin = 4,
       UIViewAutoresizingFlexibleTopMargin = 8, UIViewAutoresizingFlexibleHeight = 16, UIViewAutoresizingFlexibleBottomMargin = 32 };
enum { UIViewContentModeScaleToFill, UIViewContentModeScaleAspectFit, UIViewContentModeScaleAspectFill, UIViewContentModeRedraw, UIViewContentModeCenter };
enum { UIControlStateNormal = 0, UIControlStateHighlighted = 1, UIControlStateDisabled = 2, UIControlStateSelected = 4 };
enum { UIControlEventTouchDown = 1 << 0, UIControlEventTouchUpInside = 1 << 6, UIControlEventValueChanged = 1 << 12 };
enum { UIButtonTypeCustom = 0, UIButtonTypeRoundedRect = 1 };
enum { NSTextAlignmentLeft = 0, NSTextAlignmentCenter = 1, NSTextAlignmentRight = 2 };
enum { NSLineBreakByWordWrapping = 0, NSLineBreakByTruncatingTail = 4 };
enum { UITouchPhaseBegan, UITouchPhaseMoved, UITouchPhaseStationary, UITouchPhaseEnded, UITouchPhaseCancelled };
enum { UIGestureRecognizerStatePossible, UIGestureRecognizerStateBegan, UIGestureRecognizerStateChanged, UIGestureRecognizerStateEnded };

@class UIView, UIWindow, UIViewController, UIEvent, UIImage, UIColor, UIFont, UIGestureRecognizer;

@interface UIResponder : NSObject
- (UIResponder *)nextResponder;
- (BOOL)becomeFirstResponder;
- (BOOL)resignFirstResponder;
- (BOOL)isFirstResponder;
- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event;
- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event;
- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event;
- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event;
@end

@interface UITouch : NSObject
- (CGPoint)locationInView:(UIView *)view;
- (CGPoint)previousLocationInView:(UIView *)view;
@property(readonly) NSInteger phase;
@property(readonly) NSUInteger tapCount;
@property(readonly) UIView *view;
@property(readonly) UIWindow *window;
@end
@interface UIEvent : NSObject
- (NSSet *)allTouches;
- (NSSet *)touchesForView:(UIView *)view;
@end

@interface UIColor : NSObject
+ (UIColor *)blackColor; + (UIColor *)whiteColor; + (UIColor *)redColor; + (UIColor *)greenColor; + (UIColor *)blueColor;
+ (UIColor *)clearColor; + (UIColor *)yellowColor; + (UIColor *)grayColor; + (UIColor *)darkGrayColor; + (UIColor *)orangeColor;
+ (UIColor *)colorWithRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a;
+ (UIColor *)colorWithWhite:(CGFloat)w alpha:(CGFloat)a;
+ (UIColor *)colorWithHue:(CGFloat)h saturation:(CGFloat)s brightness:(CGFloat)b alpha:(CGFloat)a;
+ (UIColor *)colorWithPatternImage:(UIImage *)image;
- (BOOL)getRed:(CGFloat *)r green:(CGFloat *)g blue:(CGFloat *)b alpha:(CGFloat *)a;
- (UIColor *)colorWithAlphaComponent:(CGFloat)a;
@property(readonly) CGColorRef CGColor;
- (void)set;
- (void)setFill;
- (void)setStroke;
@end

@interface UIFont : NSObject
+ (UIFont *)systemFontOfSize:(CGFloat)s;
+ (UIFont *)boldSystemFontOfSize:(CGFloat)s;
+ (UIFont *)fontWithName:(NSString *)name size:(CGFloat)s;
@property(readonly) CGFloat pointSize, lineHeight, ascender, descender;
@property(readonly) NSString *fontName;
@end

@interface UIImage : NSObject
+ (UIImage *)imageNamed:(NSString *)name;
+ (UIImage *)imageWithContentsOfFile:(NSString *)path;
+ (UIImage *)imageWithData:(NSData *)data;
+ (UIImage *)imageWithCGImage:(CGImageRef)img;
@property(readonly) CGSize size;
@property(readonly) CGFloat scale;
@property(readonly) CGImageRef CGImage;
- (void)drawAtPoint:(CGPoint)p;
- (void)drawInRect:(CGRect)r;
- (UIImage *)stretchableImageWithLeftCapWidth:(NSInteger)l topCapHeight:(NSInteger)t;
- (UIImage *)resizableImageWithCapInsets:(UIEdgeInsets)i;
@end
NSData *UIImagePNGRepresentation(UIImage *image);
void UIGraphicsBeginImageContext(CGSize size);
void UIGraphicsBeginImageContextWithOptions(CGSize size, BOOL opaque, CGFloat scale);
UIImage *UIGraphicsGetImageFromCurrentImageContext(void);
void UIGraphicsEndImageContext(void);
CGContextRef UIGraphicsGetCurrentContext(void);
void UIRectFill(CGRect r);

@interface NSString (UIStringDrawing)
- (CGSize)sizeWithFont:(UIFont *)font;
- (CGSize)sizeWithFont:(UIFont *)font constrainedToSize:(CGSize)size;
- (CGSize)drawAtPoint:(CGPoint)p withFont:(UIFont *)font;
- (CGSize)drawInRect:(CGRect)r withFont:(UIFont *)font lineBreakMode:(NSInteger)m alignment:(NSInteger)a;
@end
@interface NSValue (UIGeometry)
+ (NSValue *)valueWithCGRect:(CGRect)r;
+ (NSValue *)valueWithCGPoint:(CGPoint)p;
- (CGRect)CGRectValue;
- (CGPoint)CGPointValue;
@end

@interface UIScreen : NSObject
+ (UIScreen *)mainScreen;
@property(readonly) CGRect bounds, applicationFrame;
@property(readonly) CGFloat scale;
@end

@interface UIView : UIResponder
+ (Class)layerClass;
- (instancetype)initWithFrame:(CGRect)frame;
@property CGRect frame, bounds;
@property CGPoint center;
@property CGAffineTransform transform;
@property(strong) UIColor *backgroundColor;
@property CGFloat alpha;
@property(getter=isHidden) BOOL hidden;
@property(getter=isOpaque) BOOL opaque;
@property BOOL clipsToBounds;
@property NSInteger tag;
@property NSUInteger autoresizingMask;
@property NSInteger contentMode;
@property(getter=isUserInteractionEnabled) BOOL userInteractionEnabled;
@property(readonly) UIView *superview;
@property(readonly) NSArray *subviews;
@property(readonly) UIWindow *window;
@property(readonly) CALayer *layer;
- (void)addSubview:(UIView *)v;
- (void)insertSubview:(UIView *)v atIndex:(NSInteger)i;
- (void)bringSubviewToFront:(UIView *)v;
- (void)sendSubviewToBack:(UIView *)v;
- (void)removeFromSuperview;
- (UIView *)viewWithTag:(NSInteger)tag;
- (BOOL)isDescendantOfView:(UIView *)v;
- (void)setNeedsDisplay;
- (void)setNeedsLayout;
- (void)layoutIfNeeded;
- (void)layoutSubviews;
- (void)drawRect:(CGRect)rect;
- (CGSize)sizeThatFits:(CGSize)size;
- (void)sizeToFit;
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)e;
- (BOOL)pointInside:(CGPoint)p withEvent:(UIEvent *)e;
- (CGPoint)convertPoint:(CGPoint)p toView:(UIView *)v;
- (CGPoint)convertPoint:(CGPoint)p fromView:(UIView *)v;
- (CGRect)convertRect:(CGRect)r toView:(UIView *)v;
- (void)didMoveToWindow;
- (void)didMoveToSuperview;
- (void)addGestureRecognizer:(UIGestureRecognizer *)g;
+ (void)animateWithDuration:(NSTimeInterval)d animations:(void (^)(void))a completion:(void (^)(BOOL finished))c;
+ (void)beginAnimations:(NSString *)animationID context:(void *)context;
+ (void)setAnimationDuration:(NSTimeInterval)d;
+ (void)setAnimationDelegate:(id)d;
+ (void)setAnimationDidStopSelector:(SEL)s;
+ (void)commitAnimations;
@end

@interface UIWindow : UIView
- (void)makeKeyAndVisible;
@property(strong) UIViewController *rootViewController;
@property CGFloat windowLevel;
@property(readonly, getter=isKeyWindow) BOOL keyWindow;
@end

@interface UIViewController : UIResponder
- (instancetype)initWithNibName:(NSString *)nib bundle:(NSBundle *)bundle;
@property(strong) UIView *view;
@property(copy) NSString *title;
- (void)loadView;
- (void)viewDidLoad;
- (void)viewWillAppear:(BOOL)animated;
- (void)viewDidAppear:(BOOL)animated;
- (BOOL)isViewLoaded;
- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)o;
- (void)presentModalViewController:(UIViewController *)vc animated:(BOOL)a;
- (void)dismissModalViewControllerAnimated:(BOOL)a;
@property(readonly) UIViewController *presentedViewController;
@end

@interface UIControl : UIView
- (void)addTarget:(id)target action:(SEL)action forControlEvents:(NSUInteger)events;
- (void)sendActionsForControlEvents:(NSUInteger)events;
@property(getter=isEnabled) BOOL enabled;
@property(getter=isHighlighted) BOOL highlighted;
@property(getter=isSelected) BOOL selected;
@property(readonly) NSUInteger state;
@end
@interface UILabel : UIView
@property(copy) NSString *text;
@property(strong) UIFont *font;
@property(strong) UIColor *textColor;
@property NSInteger textAlignment, numberOfLines, lineBreakMode;
@end
@interface UIImageView : UIView
- (instancetype)initWithImage:(UIImage *)image;
@property(strong) UIImage *image;
@end
@interface UIButton : UIControl
+ (instancetype)buttonWithType:(NSInteger)type;
- (void)setTitle:(NSString *)t forState:(NSUInteger)s;
- (void)setTitleColor:(UIColor *)c forState:(NSUInteger)s;
- (void)setImage:(UIImage *)i forState:(NSUInteger)s;
- (void)setBackgroundImage:(UIImage *)i forState:(NSUInteger)s;
- (NSString *)titleForState:(NSUInteger)s;
@property(readonly) NSString *currentTitle;
@property(readonly) UILabel *titleLabel;
@end
@interface UIActivityIndicatorView : UIView
- (instancetype)initWithActivityIndicatorStyle:(NSInteger)s;
- (void)startAnimating;
@end
@interface UIAlertView : UIView
- (instancetype)initWithTitle:(NSString *)t message:(NSString *)m delegate:(id)d cancelButtonTitle:(NSString *)c otherButtonTitles:(NSString *)o, ... NS_REQUIRES_NIL_TERMINATION;
- (void)show;
@end

@interface UIGestureRecognizer : NSObject
- (instancetype)initWithTarget:(id)t action:(SEL)a;
@property(readonly) NSInteger state;
@property(readonly) UIView *view;
- (CGPoint)locationInView:(UIView *)v;
@end
@interface UITapGestureRecognizer : UIGestureRecognizer
@property NSUInteger numberOfTapsRequired;
@end
@interface UIPanGestureRecognizer : UIGestureRecognizer
- (CGPoint)translationInView:(UIView *)v;
@end

@interface UIApplication : UIResponder
+ (UIApplication *)sharedApplication;
@property(weak) id delegate;
@property(readonly) UIWindow *keyWindow;
@property(getter=isStatusBarHidden) BOOL statusBarHidden;
@property(readonly) NSInteger statusBarOrientation;
- (void)setStatusBarHidden:(BOOL)h animated:(BOOL)a;
@end
@protocol UIApplicationDelegate
@optional
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options;
- (void)applicationDidBecomeActive:(UIApplication *)app;
@end

@interface UIDevice : NSObject
+ (UIDevice *)currentDevice;
@property(readonly) NSString *systemVersion, *model, *systemName;
@property(readonly) NSInteger userInterfaceIdiom;
@end
@interface NSBundle (UINibLoading)
- (NSArray *)loadNibNamed:(NSString *)name owner:(id)owner options:(NSDictionary *)options;
@end

@interface NSIndexPath (UIKitAdditions)
+ (NSIndexPath *)indexPathForRow:(NSInteger)row inSection:(NSInteger)section;
@property(readonly) NSInteger section;
@property(readonly) NSInteger row;
@end

@interface UIScrollView : UIView
@property CGPoint contentOffset;
@property CGSize contentSize;
@property UIEdgeInsets contentInset;
@property(weak) id delegate;
@property(getter=isScrollEnabled) BOOL scrollEnabled;
@property(getter=isPagingEnabled) BOOL pagingEnabled;
@property BOOL bounces;
- (void)setContentOffset:(CGPoint)contentOffset animated:(BOOL)animated;
- (void)scrollRectToVisible:(CGRect)rect animated:(BOOL)animated;
@end

@interface UITableViewCell : UIView
- (instancetype)initWithStyle:(NSInteger)style reuseIdentifier:(NSString *)reuseIdentifier;
@property(readonly) UIView *contentView;
@property(readonly) UILabel *textLabel;
@property(readonly) UILabel *detailTextLabel;
@property(readonly) UIImageView *imageView;
@property(readonly, copy) NSString *reuseIdentifier;
@property(getter=isSelected) BOOL selected;
@end

@interface UITableView : UIScrollView
- (instancetype)initWithFrame:(CGRect)frame style:(NSInteger)style;
@property(weak) id dataSource;
@property CGFloat rowHeight;
@property(strong) UIView *tableHeaderView;
@property(strong) UIView *tableFooterView;
- (void)reloadData;
- (UITableViewCell *)dequeueReusableCellWithIdentifier:(NSString *)identifier;
- (NSInteger)numberOfSections;
- (NSInteger)numberOfRowsInSection:(NSInteger)section;
- (UITableViewCell *)cellForRowAtIndexPath:(NSIndexPath *)indexPath;
- (NSArray *)visibleCells;
- (void)selectRowAtIndexPath:(NSIndexPath *)indexPath animated:(BOOL)animated scrollPosition:(NSInteger)scrollPosition;
- (NSIndexPath *)indexPathForSelectedRow;
@end

@interface UIBarItem : NSObject
@property(copy) NSString *title;
@property(strong) UIImage *image;
@property NSInteger tag;
@property(getter=isEnabled) BOOL enabled;
@end

@interface UIBarButtonItem : UIBarItem
- (instancetype)initWithTitle:(NSString *)title style:(NSInteger)style target:(id)target action:(SEL)action;
- (instancetype)initWithBarButtonSystemItem:(NSInteger)systemItem target:(id)target action:(SEL)action;
@property(weak) id target;
@property SEL action;
@end

@interface UITabBarItem : UIBarItem
- (instancetype)initWithTitle:(NSString *)title image:(UIImage *)image tag:(NSInteger)tag;
@property(copy) NSString *badgeValue;
@end

@interface UINavigationItem : NSObject
- (instancetype)initWithTitle:(NSString *)title;
@property(copy) NSString *title;
@property(strong) UIBarButtonItem *leftBarButtonItem;
@property(strong) UIBarButtonItem *rightBarButtonItem;
@property(strong) UIBarButtonItem *backBarButtonItem;
@end

@interface UINavigationBar : UIView
@property(copy) NSArray *items;
@property(readonly) UINavigationItem *topItem;
@property(readonly) UINavigationItem *backItem;
@end

@interface UINavigationController : UIViewController
- (instancetype)initWithRootViewController:(UIViewController *)rootViewController;
- (void)pushViewController:(UIViewController *)viewController animated:(BOOL)animated;
- (UIViewController *)popViewControllerAnimated:(BOOL)animated;
- (NSArray *)popToRootViewControllerAnimated:(BOOL)animated;
@property(readonly) UIViewController *topViewController;
@property(readonly) UIViewController *visibleViewController;
@property(copy) NSArray *viewControllers;
@property(readonly) UINavigationBar *navigationBar;
@property(getter=isNavigationBarHidden) BOOL navigationBarHidden;
@property(weak) id delegate;
@end

@interface UITabBar : UIView
@property(copy) NSArray *items;
@property(assign) UITabBarItem *selectedItem;
@end

@interface UITabBarController : UIViewController
@property(copy) NSArray *viewControllers;
@property(assign) UIViewController *selectedViewController;
@property NSUInteger selectedIndex;
@property(readonly) UITabBar *tabBar;
@property(weak) id delegate;
@end

@interface UIViewController (UINavTabAdditions)
@property(readonly) UINavigationItem *navigationItem;
@property(readonly) UINavigationController *navigationController;
@property(strong) UITabBarItem *tabBarItem;
@property(readonly) UITabBarController *tabBarController;
@end

@interface UISearchBar : UIView
@property(copy) NSString *text;
@property(copy) NSString *placeholder;
@property(copy) NSString *prompt;
@property(weak) id delegate;
@property BOOL showsCancelButton;
@property BOOL showsBookmarkButton;
@property NSInteger barStyle;
@end

@interface UITextField : UIControl
@property(copy) NSString *text;
@property(copy) NSString *placeholder;
@property(strong) UIFont *font;
@property(strong) UIColor *textColor;
@property NSInteger textAlignment;
@property NSInteger borderStyle;
@property(weak) id delegate;
@property(readonly, getter=isEditing) BOOL editing;
@property(getter=isSecureTextEntry) BOOL secureTextEntry;
@end

@interface UISwitch : UIControl
@property(getter=isOn) BOOL on;
- (void)setOn:(BOOL)on animated:(BOOL)animated;
@end

@interface UISlider : UIControl
@property float value;
@property float minimumValue;
@property float maximumValue;
- (void)setValue:(float)value animated:(BOOL)animated;
@end

@interface UISegmentedControl : UIControl
- (instancetype)initWithItems:(NSArray *)items;
@property(readonly) NSUInteger numberOfSegments;
@property NSInteger selectedSegmentIndex;
- (NSString *)titleForSegmentAtIndex:(NSUInteger)segment;
@end

@interface UIStoryboard : NSObject
+ (UIStoryboard *)storyboardWithName:(NSString *)name bundle:(NSBundle *)storyboardBundleOrNil;
- (id)instantiateInitialViewController;
- (id)instantiateViewControllerWithIdentifier:(NSString *)identifier;
@end
