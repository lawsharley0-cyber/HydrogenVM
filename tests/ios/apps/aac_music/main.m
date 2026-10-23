// Synthetic shared-path repro. Supply an independently generated tone.m4a resource.
#import <Foundation/Foundation.h>
#include <unistd.h>
@interface AVAudioPlayer : NSObject
- (id)initWithData:(NSData *)data error:(NSError **)error;
- (BOOL)play;
- (void)stop;
@end
int main(void) {
    @autoreleasepool {
        NSString *path = [[NSBundle mainBundle] pathForResource:@"tone" ofType:@"m4a"];
        AVAudioPlayer *player = [[AVAudioPlayer alloc] initWithData:[NSData dataWithContentsOfFile:path] error:0];
        if (![player play]) return 1;
        usleep(500000);
        [player stop];
    }
    return 0;
}
