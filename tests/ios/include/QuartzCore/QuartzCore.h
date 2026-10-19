#pragma once
#import <Foundation/Foundation.h>
#include <CoreGraphics/CoreGraphics.h>
typedef struct CATransform3D { CGFloat m11, m12, m13, m14, m21, m22, m23, m24, m31, m32, m33, m34, m41, m42, m43, m44; } CATransform3D;
extern const CATransform3D CATransform3DIdentity;
CATransform3D CATransform3DMakeRotation(CGFloat angle, CGFloat x, CGFloat y, CGFloat z);
CATransform3D CATransform3DMakeScale(CGFloat sx, CGFloat sy, CGFloat sz);
bool CATransform3DIsIdentity(CATransform3D t);
CFTimeInterval CACurrentMediaTime(void);
@interface CALayer : NSObject
+ (instancetype)layer;
@property CGRect frame, bounds;
@property CGPoint position, anchorPoint;
@property CATransform3D transform;
@property CGFloat cornerRadius, borderWidth;
@property float opacity;
@property BOOL masksToBounds, hidden;
@property CGColorRef backgroundColor, borderColor;
@property(strong) id contents;
@property(weak) id delegate;
@property(readonly) CALayer *superlayer;
@property(copy) NSArray *sublayers;
@property(copy) NSString *contentsGravity;
- (void)addSublayer:(CALayer *)l;
- (void)removeFromSuperlayer;
- (void)setNeedsDisplay;
- (void)drawInContext:(CGContextRef)ctx;
- (void)renderInContext:(CGContextRef)ctx;
- (CGPoint)convertPoint:(CGPoint)p toLayer:(CALayer *)l;
@end
@interface CADisplayLink : NSObject
+ (CADisplayLink *)displayLinkWithTarget:(id)target selector:(SEL)sel;
- (void)addToRunLoop:(NSRunLoop *)rl forMode:(NSString *)mode;
- (void)invalidate;
@property NSInteger frameInterval;
@property(readonly) CFTimeInterval timestamp;
@end
@interface CAAnimation : NSObject
+ (instancetype)animation;
@property double duration;
@property(strong) id delegate;
@end
@interface CABasicAnimation : CAAnimation
+ (instancetype)animationWithKeyPath:(NSString *)path;
@property(strong) id fromValue, toValue;
@end

@interface CAEAGLLayer : CALayer
@property(copy) NSDictionary *drawableProperties;
@end
