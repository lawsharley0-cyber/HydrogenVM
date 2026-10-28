#import <AudioToolbox/AudioToolbox.h>
#import <CoreFoundation/CoreFoundation.h>
#import <stdio.h>

int test_create_and_dispose() {
    SystemSoundID soundID = 0;
    CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, CFSTR("tone.wav"), kCFURLPOSIXPathStyle, false);
    AudioServicesCreateSystemSoundID(url, &soundID);

    if (soundID == 0) {
        printf("FAIL: soundID is 0\n");
        return 1;
    }

    // Test play before dispose
    AudioServicesPlaySystemSound(soundID);

    AudioServicesDisposeSystemSoundID(soundID);
    CFRelease(url);
    return 0;
}

int test_repeat_creation() {
    CFURLRef url1 = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, CFSTR("tone.wav"), kCFURLPOSIXPathStyle, false);
    CFURLRef url2 = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, CFSTR("tone.wav"), kCFURLPOSIXPathStyle, false);

    SystemSoundID id1 = 0, id2 = 0;
    AudioServicesCreateSystemSoundID(url1, &id1);
    AudioServicesCreateSystemSoundID(url2, &id2);

    if (id1 == id2 || id1 == 0 || id2 == 0) {
         printf("FAIL: id1 (%u) and id2 (%u) should be distinct non-zero\n", (unsigned)id1, (unsigned)id2);
         return 1;
    }

    AudioServicesDisposeSystemSoundID(id1);
    AudioServicesDisposeSystemSoundID(id2);
    CFRelease(url1);
    CFRelease(url2);
    return 0;
}

int test_invalid_dispose() {
    AudioServicesDisposeSystemSoundID(99999);
    // Should not crash
    return 0;
}

int main() {
    if (test_create_and_dispose() != 0) return 1;
    if (test_repeat_creation() != 0) return 1;
    if (test_invalid_dispose() != 0) return 1;

    printf("PASS\n");
    return 0;
}
