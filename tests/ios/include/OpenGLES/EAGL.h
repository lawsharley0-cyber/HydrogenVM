#pragma once
#import <Foundation/Foundation.h>
enum { kEAGLRenderingAPIOpenGLES1 = 1, kEAGLRenderingAPIOpenGLES2 = 2, kEAGLRenderingAPIOpenGLES3 = 3 };
extern NSString *const kEAGLDrawablePropertyRetainedBacking;
extern NSString *const kEAGLDrawablePropertyColorFormat;
extern NSString *const kEAGLColorFormatRGBA8;
extern NSString *const kEAGLColorFormatRGB565;
@interface EAGLSharegroup : NSObject
@end
@interface EAGLContext : NSObject
- (instancetype)initWithAPI:(NSUInteger)api;
- (instancetype)initWithAPI:(NSUInteger)api sharegroup:(EAGLSharegroup *)sharegroup;
+ (BOOL)setCurrentContext:(EAGLContext *)context;
+ (EAGLContext *)currentContext;
@property(readonly) NSUInteger API;
@property(readonly) EAGLSharegroup *sharegroup;
- (BOOL)renderbufferStorage:(NSUInteger)target fromDrawable:(id)drawable;
- (BOOL)presentRenderbuffer:(NSUInteger)target;
@end
