// Project-authored behavioral conformance fixtures, not copied Apple tests.
// Topic mapping and pinned references: docs/T8_OBJC_TEST_DESIGN.md.
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#include "objc_conformance.h"

@interface HVMObjCGraphBase : NSObject @end
@implementation HVMObjCGraphBase @end
@interface HVMObjCGraphChild : HVMObjCGraphBase @end
@implementation HVMObjCGraphChild @end

@interface HVMObjCLookupBase : NSObject
- (int)hvmLookupValue;
+ (int)hvmLookupValue;
@end
@implementation HVMObjCLookupBase
- (int)hvmLookupValue { return 13; }
+ (int)hvmLookupValue { return 31; }
@end
@interface HVMObjCLookupChild : HVMObjCLookupBase @end
@implementation HVMObjCLookupChild
- (int)hvmLookupValue { return 23; }
@end

@interface HVMObjCMutationBase : NSObject
- (int)hvmMutationValue;
@end
@implementation HVMObjCMutationBase
- (int)hvmMutationValue { return 11; }
@end

void TLObjCSelectors(TLObjCObservation *out) {
    *out = (TLObjCObservation){0};
    out->oracle = "Public selector interning/name contract; equal bytes identify one SEL";
    char first[] = "hvm_t8_selector:";
    char second[] = "hvm_t8_selector:";
    SEL a = sel_registerName(first), b = sel_registerName(second);
    SEL other = sel_registerName("hvm_t8_other:");
    // All expected booleans follow the API identity contract, not HVM output.
    TLObjCCheck(out, "distinctInputBuffers", &first[0] != &second[0], 1);
    TLObjCCheck(out, "registeredNonNull", a != NULL && b != NULL && other != NULL, 1);
    TLObjCCheck(out, "equalNamesSameSelector", a == b, 1);
    TLObjCCheck(out, "compiledSelectorMatches", a == @selector(hvm_t8_selector:), 1);
    TLObjCCheck(out, "differentNamesDiffer", a != other, 1);
    const char *name = a ? sel_getName(a) : NULL;
    TLObjCCheck(out, "selectorNameRoundTrip", name && strcmp(name, first) == 0, 1);
}

void TLObjCClassGraph(TLObjCObservation *out) {
    *out = (TLObjCObservation){0};
    out->oracle = "Authored Child : Base : NSObject declarations and public metaclass inheritance contract";
    Class base = objc_getClass("HVMObjCGraphBase"), child = objc_getClass("HVMObjCGraphChild");
    Class metaBase = object_getClass(base), metaChild = object_getClass(child);
    TLObjCCheck(out, "classesExist", base != Nil && child != Nil, 1);
    TLObjCCheck(out, "metaclassesExist", metaBase != Nil && metaChild != Nil, 1);
    TLObjCCheck(out, "childSuperclass", class_getSuperclass(child) == base, 1);
    TLObjCCheck(out, "baseSuperclass", class_getSuperclass(base) == objc_getClass("NSObject"), 1);
    TLObjCCheck(out, "metaclassSuperclass", class_getSuperclass(metaChild) == metaBase, 1);
    TLObjCCheck(out, "baseIsMetaclass", class_isMetaClass(base) != 0, 0);
    TLObjCCheck(out, "childIsMetaclass", class_isMetaClass(child) != 0, 0);
    TLObjCCheck(out, "metaBaseIsMetaclass", class_isMetaClass(metaBase) != 0, 1);
    TLObjCCheck(out, "metaChildIsMetaclass", class_isMetaClass(metaChild) != 0, 1);
}

typedef int (*TLIntMethod)(id, SEL);
static int invokeMethod(Method method, id receiver, SEL selector) {
    const IMP imp = method ? method_getImplementation(method) : NULL;
    return imp ? ((TLIntMethod)imp)(receiver, selector) : -1;
}

void TLObjCMethodLookup(TLObjCObservation *out) {
    *out = (TLObjCObservation){0};
    out->oracle = "Authored bodies return 13/23/31; lookup follows override and metaclass inheritance";
    HVMObjCLookupBase *base = [HVMObjCLookupBase new];
    HVMObjCLookupChild *child = [HVMObjCLookupChild new];
    Class bc = [HVMObjCLookupBase class], cc = [HVMObjCLookupChild class];
    SEL selector = @selector(hvmLookupValue);
    Method bi = class_getInstanceMethod(bc, selector), ci = class_getInstanceMethod(cc, selector);
    Method bm = class_getClassMethod(bc, selector), cm = class_getClassMethod(cc, selector);
    TLObjCCheck(out, "methodsExist", bi && ci && bm && cm, 1);
    // Constants are declared in the guest bodies above, independently of lookup.
    TLObjCCheck(out, "baseInstance", invokeMethod(bi, base, selector), 13);
    TLObjCCheck(out, "childInstanceOverride", invokeMethod(ci, child, selector), 23);
    TLObjCCheck(out, "baseClassMethod", invokeMethod(bm, bc, selector), 31);
    TLObjCCheck(out, "inheritedClassMethod", invokeMethod(cm, cc, selector), 31);
    TLObjCCheck(out, "distinctInstanceImplementations",
        bi && ci && method_getImplementation(bi) != method_getImplementation(ci), 1);
    TLObjCCheck(out, "sharedInheritedClassImplementation",
        bm && cm && method_getImplementation(bm) == method_getImplementation(cm), 1);
    SEL missing = sel_registerName("hvm_t8_no_such_method");
    TLObjCCheck(out, "missingInstanceMethod", class_getInstanceMethod(cc, missing) == NULL, 1);
    TLObjCCheck(out, "missingClassMethod", class_getClassMethod(cc, missing) == NULL, 1);
}

static int mutation29(id self, SEL cmd) { return 29; }
static int mutation47(id self, SEL cmd) { return 47; }

void TLObjCMutation(TLObjCObservation *out) {
    *out = (TLObjCObservation){0};
    out->oracle = "Authored 11 -> add 29 -> reject duplicate -> replace 47 state sequence; parent remains 11";
    // Fresh child metadata makes repeated phone/menu runs independent too.
    static unsigned sequence;
    char name[64];
    snprintf(name, sizeof(name), "HVMObjCMutationChild_%u", ++sequence);
    Class childClass = objc_allocateClassPair([HVMObjCMutationBase class], name, 0);
    TLObjCCheck(out, "allocatedFreshClass", childClass != Nil, 1);
    if (!childClass) return;
    objc_registerClassPair(childClass);
    HVMObjCMutationBase *child = [childClass new], *base = [HVMObjCMutationBase new];
    SEL selector = @selector(hvmMutationValue);
    // Repeat sends around mutations so a warmed dispatch cache is exercised.
    TLObjCCheck(out, "inheritedCold", [child hvmMutationValue], 11);
    TLObjCCheck(out, "inheritedWarm", [child hvmMutationValue], 11);
    TLObjCCheck(out, "addInheritedOverride", class_addMethod(childClass, selector, (IMP)mutation29, "i@:") != 0, 1);
    TLObjCCheck(out, "addedCold", [child hvmMutationValue], 29);
    TLObjCCheck(out, "addedWarm", [child hvmMutationValue], 29);
    TLObjCCheck(out, "duplicateRejected", class_addMethod(childClass, selector, (IMP)mutation47, "i@:") != 0, 0);
    TLObjCCheck(out, "duplicatePreservesImplementation", [child hvmMutationValue], 29);
    IMP previous = class_replaceMethod(childClass, selector, (IMP)mutation47, "i@:");
    TLObjCCheck(out, "replaceReturnsPriorImplementation", previous == (IMP)mutation29, 1);
    TLObjCCheck(out, "replacedCold", [child hvmMutationValue], 47);
    TLObjCCheck(out, "replacedWarm", [child hvmMutationValue], 47);
    TLObjCCheck(out, "parentUnchanged", [base hvmMutationValue], 11);
}
