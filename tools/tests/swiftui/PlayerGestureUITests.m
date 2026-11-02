#import <XCTest/XCTest.h>
#import <UIKit/UIKit.h>

// XCTest's public gesture API has no multi-finger press. These private XCTest
// interfaces synthesize concurrent pointer paths in the UI test process only.
@interface XCPointerEventPath : NSObject
- (instancetype)initForTouchAtPoint:(CGPoint)point offset:(NSTimeInterval)offset;
- (void)moveToPoint:(CGPoint)point atOffset:(NSTimeInterval)offset;
- (void)liftUpAtOffset:(NSTimeInterval)offset;
@end

@interface XCSynthesizedEventRecord : NSObject
- (instancetype)initWithName:(NSString *)name interfaceOrientation:(UIInterfaceOrientation)orientation;
- (void)addPointerEventPath:(XCPointerEventPath *)path;
@end

@protocol PlayerEventSynthesizing
- (void)synthesizeEvent:(XCSynthesizedEventRecord *)event completion:(void (^)(BOOL, NSError *))completion;
@end

@interface XCUIDevice (PlayerEventSynthesizer)
- (id<PlayerEventSynthesizing>)eventSynthesizer;
@end

@interface PlayerGestureUITests : XCTestCase
@end

@implementation PlayerGestureUITests

- (void)waitForElement:(XCUIElement *)element label:(NSString *)label {
    NSPredicate *predicate = [NSPredicate predicateWithFormat:@"label == %@", label];
    [self expectationForPredicate:predicate evaluatedWithObject:element handler:nil];
    [self waitForExpectationsWithTimeout:10 handler:nil];
}

- (void)holdThreeFingersInApp:(XCUIApplication *)app screen:(XCUIElement *)screen {
    NSTimeInterval hold = app.staticTexts[@"probe-hold-seconds"].label.doubleValue;
    XCTAssertGreaterThan(hold, 0);
    Class recordClass = NSClassFromString(@"XCSynthesizedEventRecord");
    Class pathClass = NSClassFromString(@"XCPointerEventPath");
    XCTAssertNotNil(recordClass);
    XCTAssertNotNil(pathClass);
    XCSynthesizedEventRecord *event = [[recordClass alloc] initWithName:@"Three finger hold"
                                                  interfaceOrientation:UIInterfaceOrientationPortrait];
    CGRect frame = screen.frame;
    for (NSUInteger index = 0; index < 3; index++) {
        CGPoint point = CGPointMake(CGRectGetMinX(frame) + CGRectGetWidth(frame) * (0.38 + 0.12 * index),
                                    CGRectGetMinY(frame) + CGRectGetHeight(frame) * 0.68);
        XCPointerEventPath *path = [[pathClass alloc] initForTouchAtPoint:point offset:0];
        // Move after recognition, while all three fingers are still down.
        [path moveToPoint:CGPointMake(point.x + 3, point.y) atOffset:hold + 0.3];
        [path liftUpAtOffset:hold + 0.6];
        [event addPointerEventPath:path];
    }
    XCTestExpectation *synthesized = [self expectationWithDescription:@"three-finger event completed"];
    __block BOOL success = NO;
    __block NSError *synthesisError = nil;
    [[XCUIDevice.sharedDevice eventSynthesizer] synthesizeEvent:event completion:^(BOOL result, NSError *error) {
        success = result;
        synthesisError = error;
        [synthesized fulfill];
    }];
    [self waitForExpectationsWithTimeout:10 handler:nil];
    XCTAssertTrue(success, @"Event synthesis failed: %@", synthesisError);
}

- (void)testThreeFingerHoldSummonsAndHidesControlsWithoutInterruptingTouches {
    self.continueAfterFailure = NO;
    XCUIApplication *app = [[XCUIApplication alloc] init];
    app.launchArguments = @[@"-player.showDebugOverlay", @"NO"];
    app.launchEnvironment = @{ @"PLAYER_ORIENTATION": @"1" };
    [app launch];
    XCTAssertEqualObjects(app.staticTexts[@"probe-stop-count"].label, @"0");
    [app.buttons[@"Open Player"] tap];
    XCUIElement *screen = app.otherElements[@"player-viewport"];
    XCTAssertTrue([screen waitForExistenceWithTimeout:10]);
    [self waitForElement:app.staticTexts[@"probe-frame"] label:@"ready"];
    XCTAssertFalse(app.buttons[@"player-controls"].exists);
    XCTAssertFalse(app.buttons[@"player-pause"].exists);

    [self holdThreeFingersInApp:app screen:screen];

    XCTAssertTrue([app.buttons[@"player-pause"] waitForExistenceWithTimeout:10]);
    XCTAssertTrue(app.buttons[@"hud-minimize"].exists);
    [self waitForElement:app.staticTexts[@"probe-max-touches"] label:@"3"];
    [self waitForElement:app.staticTexts[@"probe-moved-fingers"] label:@"3"];
    [self waitForElement:app.staticTexts[@"probe-ended-fingers"] label:@"3"];
    XCTAssertEqualObjects(app.staticTexts[@"probe-cancellations"].label, @"0");

    [app.buttons[@"hud-minimize"] tap];
    NSPredicate *hidden = [NSPredicate predicateWithFormat:@"exists == NO"];
    [self expectationForPredicate:hidden evaluatedWithObject:app.buttons[@"player-pause"] handler:nil];
    [self waitForExpectationsWithTimeout:10 handler:nil];
    XCTAssertFalse(app.buttons[@"player-controls"].exists);

    [self holdThreeFingersInApp:app screen:screen];
    XCTAssertTrue([app.buttons[@"player-quit"] waitForExistenceWithTimeout:10]);
    [app.buttons[@"player-quit"] tap];
    XCTAssertTrue([app.buttons[@"Open Player"] waitForExistenceWithTimeout:10]);
    [self waitForElement:app.staticTexts[@"probe-stop-count"] label:@"1"];
    XCTAssertFalse(app.buttons[@"player-pause"].isHittable);
}
@end
