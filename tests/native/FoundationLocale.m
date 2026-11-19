// Independent native Apple contract check; never linked into the emulator.
#import <Foundation/Foundation.h>
#include <stdio.h>
int main(void) {
    @autoreleasepool {
        NSArray<NSString *> *codes=@[@"en",@"ar",@"he",@"fa",@"ur"];
        const NSUInteger expected[]={1,2,2,2,2};
        int failed=0;
        for(NSUInteger i=0;i<codes.count;++i) {
            NSUInteger direction=[NSLocale characterDirectionForLanguage:codes[i]];
            printf("native language=%s direction=%lu expected=%lu\n",
                codes[i].UTF8String,(unsigned long)direction,(unsigned long)expected[i]);
            failed|=direction!=expected[i];
        }
        // Diagnostic only: this is a BCP-47 tag, not the ISO language-code
        // contract. Do not infer a test expectation from its returned value.
        printf("native diagnostic az-Arab=%lu\n",(unsigned long)[NSLocale characterDirectionForLanguage:@"az-Arab"]);
        return failed;
    }
}
