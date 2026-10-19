// Objective-C runtime test (no Foundation). Every check prints "ok <name>"
// or "FAIL <name> ..." so the expected output is easy to maintain.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#import <objc/NSObject.h>
#import <objc/runtime.h>

static int failures;
#define CHECK(name, cond) do { if (cond) printf("ok %s\n", name); else { printf("FAIL %s (line %d)\n", name, __LINE__); failures++; } } while (0)

static int loadCount, initCount, deallocCount, destructCount;
static char log_buf[512];
static void L(const char *s) { strcat(log_buf, s); strcat(log_buf, ";"); }

@protocol Greeter
- (const char *)greet;
@end

@interface Animal : NSObject <Greeter> {
@public
    int legs;
}
@property (nonatomic) int age;
@property (nonatomic, strong) id friendObj;
@property (atomic, strong) id atomicObj;
@property (nonatomic, weak) id weakFriend;
@property (nonatomic, copy) id copied;
+ (int)kingdomCode;
- (const char *)name;
- (int)speedFor:(int)distance;
- (double)weightWithFactor:(double)f;
- (long long)bigNumber;
@end

@implementation Animal
+ (void)load { loadCount++; L("Animal+load"); }
+ (void)initialize { if (self == [Animal class]) { initCount++; L("Animal+initialize"); } }
+ (int)kingdomCode { return 7; }
- (instancetype)init { if ((self = [super init])) { legs = 4; _age = 1; } return self; }
- (const char *)name { return "animal"; }
- (const char *)greet { return "hello from animal"; }
- (int)speedFor:(int)distance { return distance * legs; }
- (double)weightWithFactor:(double)f { return 2.5 * f; }
- (long long)bigNumber { return 0x123456789LL; }
- (void)dealloc { deallocCount++; }
@end

@interface Dog : Animal
@end
@implementation Dog
+ (void)load { loadCount++; L("Dog+load"); }
- (const char *)name { return "dog"; }
- (const char *)greet { return [super greet]; }
- (int)speedFor:(int)distance { return [super speedFor:distance] + 1; }
@end

@interface Animal (Extras)
- (const char *)extra;
@end
@implementation Animal (Extras)
- (const char *)extra { return "category method"; }
@end

@interface Dog (Override)
@end
@implementation Dog (Override)
- (const char *)name { return "overridden dog"; }
+ (void)load { loadCount++; L("Dog(Override)+load"); }
@end

// A class with C++-like ivars needing .cxx_destruct (ARC strong ivars).
@interface Holder : NSObject { id strongIvar; }
- (void)hold:(id)o;
@end
@implementation Holder
- (void)hold:(id)o { strongIvar = o; }
@end

@interface Tracker : NSObject
@end
@implementation Tracker
- (void)dealloc { destructCount++; }
@end

@interface CopyMe : NSObject <NSCopying>
@property int value;
@end
@implementation CopyMe
- (id)copyWithZone:(NSZone *)zone { CopyMe *c = [[CopyMe alloc] init]; c.value = self.value + 100; return c; }
@end

@interface Forwarder : NSObject
@property (strong) id target;
@end
@implementation Forwarder
- (id)forwardingTargetForSelector:(SEL)s { return self.target; }
@end

static const char *swizzled(id self, SEL _cmd) { return "swizzled"; }
static int dynamicMethod(id self, SEL _cmd) { return 99; }

@interface Dynamic : NSObject
@end
@implementation Dynamic
+ (BOOL)resolveInstanceMethod:(SEL)sel {
    if (strcmp(sel_getName(sel), "magic") == 0) { class_addMethod(self, sel, (IMP)dynamicMethod, "i@:"); return YES; }
    return NO;
}
@end

typedef int (^IntBlock)(int);
static IntBlock makeAdder(int base) {
    __block int calls = 0;
    IntBlock b = ^(int v) { calls++; return base + v + calls * 1000; };
    return b;   // ARC copies the block to the heap
}

int main(int argc, char **argv) {
    @autoreleasepool {
        CHECK("+load order", loadCount == 3 && strstr(log_buf, "Animal+load;Dog+load;Dog(Override)+load;") == log_buf);
        CHECK("+initialize not yet", initCount == 0);
        Animal *a = [[Animal alloc] init];
        CHECK("+initialize once", initCount == 1);
        CHECK("instance method", strcmp([a name], "animal") == 0);
        CHECK("ivar", a->legs == 4);
        CHECK("int arg", [a speedFor:10] == 40);
        CHECK("double arg/return", [a weightWithFactor:4.0] == 10.0);
        CHECK("long long return", [a bigNumber] == 0x123456789LL);
        CHECK("class method", [Animal kingdomCode] == 7);
        CHECK("category", strcmp([a extra], "category method") == 0);
        CHECK("property", a.age == 1 && (a.age = 5, a.age == 5));

        Dog *d = [Dog new];
        CHECK("subclass + category override", strcmp([d name], "overridden dog") == 0);
        CHECK("super call", [d speedFor:10] == 41);
        CHECK("super via protocol", strcmp([d greet], "hello from animal") == 0);
        CHECK("inherited class method", [Dog kingdomCode] == 7);
        CHECK("isKindOfClass", [d isKindOfClass:[Animal class]] && ![a isKindOfClass:[Dog class]]);
        CHECK("isMemberOfClass", [d isMemberOfClass:[Dog class]] && ![d isMemberOfClass:[Animal class]]);
        CHECK("respondsToSelector", [d respondsToSelector:@selector(extra)] && ![d respondsToSelector:@selector(fly)]);
        CHECK("conformsToProtocol", [d conformsToProtocol:@protocol(Greeter)]);
        CHECK("class names", strcmp(class_getName([d class]), "Dog") == 0 && strcmp(class_getName([Dog superclass]), "Animal") == 0);
        CHECK("metaclass", class_isMetaClass(object_getClass([Dog class])) && !class_isMetaClass([Dog class]));
        CHECK("performSelector", [d performSelector:@selector(self)] == d);
        CHECK("instance size", class_getInstanceSize([Animal class]) >= 8);

        // properties: strong / weak / copy / atomic. Getters may hand objects
        // back autoreleased, so deallocation is checked after a pool drains.
        @autoreleasepool {
            Tracker *t = [Tracker new];
            a.friendObj = t;
            a.atomicObj = t;
            a.weakFriend = t;
            CHECK("weak property set", a.weakFriend == t);
            t = nil;
            CHECK("still alive via strong props", destructCount == 0 && a.weakFriend != nil);
            a.friendObj = nil;
            a.atomicObj = nil;
        }
        CHECK("dealloc when last strong ref goes", destructCount == 1);
        CHECK("weak zeroed", a.weakFriend == nil);
        CopyMe *orig = [CopyMe new];
        orig.value = 1;
        a.copied = orig;
        CHECK("copy property", [(CopyMe *)a.copied value] == 101 && a.copied != orig);

        // ARC strong ivars released by .cxx_destruct
        @autoreleasepool {
            Holder *h = [Holder new];
            [h hold:[Tracker new]];
            h = nil;
        }
        CHECK(".cxx_destruct releases ivars", destructCount == 2);

        // __weak locals
        @autoreleasepool {
            id weakTarget = [Tracker new];
            __weak id w = weakTarget;
            CHECK("__weak local", w == weakTarget);
            weakTarget = nil;
            CHECK("__weak local zeroed", w == nil);
        }
        CHECK("__weak target deallocated", destructCount == 3);

        // blocks
        IntBlock add5 = makeAdder(5);
        CHECK("block call 1", add5(1) == 1006);
        CHECK("block call 2 (__block state)", add5(1) == 2006);
        IntBlock copy = [add5 copy];
        CHECK("block copy shares state", copy(0) == 3005);
        __block int counter = 0;
        void (^inc)(void) = ^{ counter += 2; };
        inc(); inc();
        CHECK("__block variable", counter == 4);
        int captured = 21;
        int (^twice)(void) = ^{ return captured * 2; };
        CHECK("captured value", twice() == 42);
        @autoreleasepool {
            id captureObj = [Tracker new];
            void (^holdObj)(void) = [^{ (void)captureObj; } copy];
            captureObj = nil;
            CHECK("block retains captured object", destructCount == 3);
            holdObj = nil;
        }
        CHECK("block release frees capture", destructCount == 4);

        // runtime API
        Method m = class_getInstanceMethod([Animal class], @selector(name));
        IMP old = method_setImplementation(m, (IMP)swizzled);
        CHECK("method_setImplementation", strcmp([a name], "swizzled") == 0);
        method_setImplementation(m, old);
        CHECK("restore implementation", strcmp([a name], "animal") == 0);
        Method m1 = class_getInstanceMethod([Animal class], @selector(name));
        Method m2 = class_getInstanceMethod([Animal class], @selector(extra));
        method_exchangeImplementations(m1, m2);
        CHECK("method_exchangeImplementations", strcmp([a name], "category method") == 0 && strcmp([a extra], "animal") == 0);
        method_exchangeImplementations(m1, m2);
        CHECK("dynamic method resolution", ((int (*)(id, SEL))objc_msgSend)([Dynamic new], sel_registerName("magic")) == 99);
        Forwarder *f = [Forwarder new];
        f.target = d;
        CHECK("forwardingTargetForSelector", strcmp([(Dog *)f name], "overridden dog") == 0);
        Ivar iv = class_getInstanceVariable([Animal class], "legs");
        CHECK("ivar lookup", iv && strcmp(ivar_getName(iv), "legs") == 0 && ivar_getOffset(iv) >= 4);
        unsigned nprops = 0;
        objc_property_t *props = class_copyPropertyList([Animal class], &nprops);
        CHECK("property list", nprops == 5 && props && strcmp(property_getName(props[0]), "age") == 0);
        free(props);
        static char key;
        id assoc = [Tracker new];
        objc_setAssociatedObject(a, &key, assoc, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        CHECK("associated object", objc_getAssociatedObject(a, &key) == assoc);
        Class dyn = objc_allocateClassPair([NSObject class], "MadeAtRuntime", 0);
        class_addMethod(dyn, @selector(speedFor:), (IMP)dynamicMethod, "i@:i");
        objc_registerClassPair(dyn);
        id inst = [[dyn alloc] init];
        CHECK("objc_allocateClassPair", objc_getClass("MadeAtRuntime") == dyn && [(Animal *)inst speedFor:0] == 99);
        @synchronized (a) { @synchronized (a) { CHECK("@synchronized (recursive)", 1); } }

        a = nil;
        d = nil;
    }
    printf("deallocs=%d destructs=%d\n", deallocCount, destructCount);
    printf("%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures;
}
