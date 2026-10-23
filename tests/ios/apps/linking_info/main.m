// Diagnostic linking oracle: none of these missing APIs may run during inspection.
#import <Foundation/Foundation.h>
@interface HVMMissingClass : NSObject
+ (int)hvmMissingSelector;
@end
extern int HVMMissingData;
extern int HVMWeakData __attribute__((weak_import));
extern int hvm_missing_function(void);
int main(void) {
    // A run would stop at the missing function; info --linking must never call it.
    return hvm_missing_function() + HVMMissingData + (int)(unsigned long)&HVMWeakData
           + [HVMMissingClass hvmMissingSelector];
}
