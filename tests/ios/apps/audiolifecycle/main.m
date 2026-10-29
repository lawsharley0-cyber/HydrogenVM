#import <CoreFoundation/CoreFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <stdio.h>

int test_create_and_dispose(CFURLRef url) {
    SystemSoundID soundID = 0;
    int status = AudioServicesCreateSystemSoundID(url, &soundID);

    if (status != kAudioServicesNoError || soundID == 0) {
        printf("FAIL: soundID is 0 or status != noErr\n");
        return 1;
    }

    AudioServicesPlaySystemSound(soundID);

    status = AudioServicesDisposeSystemSoundID(soundID);
    if (status != kAudioServicesNoError) {
        printf("FAIL: Dispose failed\n");
        return 1;
    }
    return 0;
}

int test_repeat_creation_distinct(CFURLRef url1, CFURLRef url2) {
    SystemSoundID id1 = 0, id2 = 0;
    int s1 = AudioServicesCreateSystemSoundID(url1, &id1);
    int s2 = AudioServicesCreateSystemSoundID(url2, &id2);

    if (s1 != 0 || s2 != 0 || id1 == id2 || id1 == 0 || id2 == 0) {
         printf("FAIL: distinct URLs - statuses: %d, %d; id1 (%u) and id2 (%u) should be distinct non-zero\n", s1, s2, (unsigned)id1, (unsigned)id2);
         return 1;
    }

    int ds1 = AudioServicesDisposeSystemSoundID(id1);
    int ds2 = AudioServicesDisposeSystemSoundID(id2);

    if (ds1 != 0 || ds2 != 0) {
         printf("FAIL: distinct URLs dispose failed\n");
         return 1;
    }

    return 0;
}

int test_repeat_creation_same(CFURLRef url1) {
    SystemSoundID id1 = 0, id2 = 0;
    int s1 = AudioServicesCreateSystemSoundID(url1, &id1);
    int s2 = AudioServicesCreateSystemSoundID(url1, &id2);

    if (s1 != 0 || s2 != 0 || id1 == id2 || id1 == 0 || id2 == 0) {
         printf("FAIL: same URL - statuses: %d, %d; id1 (%u) and id2 (%u) should be distinct non-zero even with same URL\n", s1, s2, (unsigned)id1, (unsigned)id2);
         return 1;
    }

    int ds1 = AudioServicesDisposeSystemSoundID(id1);
    int ds2 = AudioServicesDisposeSystemSoundID(id2);

    if (ds1 != 0 || ds2 != 0) {
         printf("FAIL: same URL dispose failed\n");
         return 1;
    }

    return 0;
}

int test_invalid_dispose() {
    int status = AudioServicesDisposeSystemSoundID(99999);
    // Just verifying it doesn't crash.
    return 0;
}

int main() {
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (!mainBundle) {
        printf("FAIL: Could not get main bundle\n");
        return 1;
    }

    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, "tone", kCFStringEncodingUTF8);
    CFStringRef ext = CFStringCreateWithCString(kCFAllocatorDefault, "wav", kCFStringEncodingUTF8);
    CFURLRef url = CFBundleCopyResourceURL(mainBundle, name, ext, NULL);

    if (!url) {
        printf("FAIL: Could not find tone.wav resource\n");
        return 1;
    }

    if (test_create_and_dispose(url) != 0) return 1;
    CFStringRef name2 = CFStringCreateWithCString(kCFAllocatorDefault, "tone2", kCFStringEncodingUTF8);
    CFURLRef url2 = CFBundleCopyResourceURL(mainBundle, name2, ext, NULL);
    if (url2) {
        if (test_repeat_creation_distinct(url, url2) != 0) return 1;
        CFRelease(url2);
    } else {
        printf("FAIL: Could not find tone2.wav resource\n");
        return 1;
    }
    CFRelease(name2);
    if (test_repeat_creation_same(url) != 0) return 1;
    if (test_invalid_dispose() != 0) return 1;

    CFRelease(url);
    CFRelease(name);
    CFRelease(ext);

    printf("PASS\n");
    return 0;
}
