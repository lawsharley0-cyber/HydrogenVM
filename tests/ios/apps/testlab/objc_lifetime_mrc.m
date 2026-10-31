// Project-authored MRC fixture: explicit ownership is the independent oracle.
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include "objc_conformance.h"

static int deallocations;
@interface HVMObjCCountedValue : NSObject @end
@implementation HVMObjCCountedValue
- (void)dealloc {
    ++deallocations;
    [super dealloc];
}
@end

void TLObjCLifetime(TLObjCObservation *out) {
    *out = (TLObjCObservation){0};
    out->oracle = "Ownership ledger: allocation +1; association +1; caller release -1; clear -1; exactly one dealloc";
    deallocations = 0;
    static char key;
    NSObject *owner = [NSObject new];
    HVMObjCCountedValue *value = [HVMObjCCountedValue new];
    TLObjCCheck(out, "objectsAllocated", owner != nil && value != nil, 1);
    objc_setAssociatedObject(owner, &key, value, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [value release]; // association must now be the value's surviving owner
    TLObjCCheck(out, "associationKeepsValueAlive", deallocations, 0);
    @autoreleasepool {
        TLObjCCheck(out, "lookupIdentity", objc_getAssociatedObject(owner, &key) == value, 1);
    } // drain any getter autoreleases before testing the last owner's release
    TLObjCCheck(out, "aliveAfterGetterPool", deallocations, 0);
    objc_setAssociatedObject(owner, &key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    TLObjCCheck(out, "associationCleared", objc_getAssociatedObject(owner, &key) == nil, 1);
    TLObjCCheck(out, "clearReleasesLastOwner", deallocations, 1);
    [owner release];
    TLObjCCheck(out, "ownerReleaseDoesNotDeallocValueAgain", deallocations, 1);
}
