// HydrogenVM test app: C++ and Objective-C exceptions through the SJLJ
// unwinder (armv7 iOS), including exceptions raised by framework code.
#import <Foundation/Foundation.h>
extern "C" {
#include <stdio.h>
}

static int failures = 0, checks = 0;
#define CHECK(cond, what) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s (line %d)\n", what, __LINE__); } else printf("ok %s\n", what); } while (0)

struct Base { int tag; Base() : tag(1) {} virtual ~Base() {} virtual int kind() const { return 1; } };
struct Derived : Base { Derived() { tag = 2; } int kind() const { return 2; } };
struct Other { int x; Other() : x(7) {} virtual ~Other() {} };
struct Multi : Other, Base { int m; Multi() : m(42) { tag = 3; } int kind() const { return 3; } };
struct Unrelated { int u; };

static int destroyed = 0;
struct Guard { ~Guard() { ++destroyed; } };

__attribute__((noinline)) static void throwDerived() { Guard g; throw Derived(); }
__attribute__((noinline)) static void level3() { Guard g; throwDerived(); }
__attribute__((noinline)) static void level2() { Guard g; level3(); }
__attribute__((noinline)) static int rethrower() {
    try { throw 5; } catch (int v) { if (v == 5) throw; }
    return 0;
}

@interface MyError : NSException
@end
@implementation MyError
@end

static int cxxTests(void) {
    { int got = 0; try { throw 42; } catch (int v) { got = v; } CHECK(got == 42, "catch int"); }
    { int k = 0; try { throw Derived(); } catch (const Base& b) { k = b.kind(); } CHECK(k == 2, "catch derived by base reference"); }
    {
        int tag = 0, kind = 0;
        try { throw Multi(); } catch (const Base& b) { tag = b.tag; kind = b.kind(); }
        CHECK(tag == 3 && kind == 3, "catch second base of multiple inheritance (adjusted pointer)");
    }
    { bool any = false; try { throw Unrelated(); } catch (const Base&) {} catch (...) { any = true; } CHECK(any, "catch (...)"); }
    {
        destroyed = 0;
        int k = 0;
        try { level2(); } catch (Base& b) { k = b.kind(); }
        CHECK(k == 2 && destroyed == 3, "unwind through 3 frames running destructors");
    }
    { int got = 0; try { rethrower(); } catch (int v) { got = v; } CHECK(got == 5, "rethrow"); }
    {
        int where = 0;
        try {
            try { throw Derived(); } catch (const Other&) { where = 1; }
        } catch (const Derived& d) { where = d.tag == 2 ? 2 : 3; }
        CHECK(where == 2, "inner handler doesn't match, outer does");
    }
    { int k = 0; try { throw new Derived(); } catch (Base* b) { k = b->kind(); delete b; } CHECK(k == 2, "catch derived pointer as base pointer"); }
    { const char* s = 0; try { throw "boom"; } catch (const char* m) { s = m; } CHECK(s && s[0] == 'b' && s[3] == 'm', "catch const char*"); }
    {
        int n = 0;
        for (int i = 0; i < 200; ++i) { try { if (i % 2) throw i; throw Derived(); } catch (int) { ++n; } catch (Base&) { ++n; } }
        CHECK(n == 200, "200 throws in a loop");
    }
    return 0;
}

static void objcTests(void) {
    @autoreleasepool {
        NSString* name = nil;
        @try { [NSException raise:@"TestException" format:@"value %d", 3]; }
        @catch (NSException* e) { name = [e name]; }
        CHECK([name isEqualToString:@"TestException"], "@catch an exception raised by Foundation (+raise:format:)");

        int path = 0;
        @try { @throw [MyError exceptionWithName:@"Mine" reason:@"r" userInfo:nil]; }
        @catch (NSString* s) { path = 1; }
        @catch (MyError* e) { path = 2; }
        @catch (id e) { path = 3; }
        CHECK(path == 2, "@catch by class picks the matching clause");

        bool finallyRan = false, caught = false;
        @try {
            @try { @throw [NSException exceptionWithName:@"Inner" reason:nil userInfo:nil]; }
            @finally { finallyRan = true; }
        } @catch (id e) { caught = true; }
        CHECK(finallyRan && caught, "@finally runs while unwinding");

        bool rangeCaught = false;
        @try { NSArray* a = [NSArray arrayWithObject:@"x"]; (void)[a objectAtIndex:5]; }
        @catch (NSException* e) { rangeCaught = [[e name] isEqualToString:NSRangeException]; }
        CHECK(rangeCaught, "@catch NSRangeException from -[NSArray objectAtIndex:]");
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    cxxTests();
    objcTests();
    printf("%d checks, %d failures\n", checks, failures);
    if (!failures) printf("ALL CHECKS PASSED\n");
    return failures ? 1 : 0;
}
