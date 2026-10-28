#import <CoreFoundation/CoreFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <stdio.h>

int test_create_and_dispose() {
    SystemSoundID soundID = 0;
    CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault, "tone.wav", kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithString(kCFAllocatorDefault, path, NULL);
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
    CFRelease(url);
    CFRelease(path);
    return 0;
}

int test_repeat_creation_distinct() {
    CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault, "tone.wav", kCFStringEncodingUTF8);
    CFURLRef url1 = CFURLCreateWithString(kCFAllocatorDefault, path, NULL);
    CFURLRef url2 = CFURLCreateWithString(kCFAllocatorDefault, path, NULL);

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
    CFRelease(path);
    return 0;
}

int test_repeat_creation_same() {
    CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault, "tone.wav", kCFStringEncodingUTF8);
    CFURLRef url1 = CFURLCreateWithString(kCFAllocatorDefault, path, NULL);

    SystemSoundID id1 = 0, id2 = 0;
    AudioServicesCreateSystemSoundID(url1, &id1);
    AudioServicesCreateSystemSoundID(url1, &id2);

    if (id1 == id2 || id1 == 0 || id2 == 0) {
         printf("FAIL: id1 (%u) and id2 (%u) should be distinct non-zero even with same URL\n", (unsigned)id1, (unsigned)id2);
         return 1;
    }

    AudioServicesDisposeSystemSoundID(id1);
    AudioServicesDisposeSystemSoundID(id2);
    CFRelease(url1);
    CFRelease(path);
    return 0;
}

int test_invalid_dispose() {
    int status = AudioServicesDisposeSystemSoundID(99999);
    return 0;
}

int main() {
    if (test_create_and_dispose() != 0) return 1;
    if (test_repeat_creation_distinct() != 0) return 1;
    if (test_repeat_creation_same() != 0) return 1;
    if (test_invalid_dispose() != 0) return 1;

    printf("PASS\n");
    return 0;
}
