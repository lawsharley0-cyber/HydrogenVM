#import <CoreFoundation/CoreFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <stdio.h>

static CFURLRef toneURL(void) {
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, "tone", kCFStringEncodingUTF8);
    CFStringRef ext = CFStringCreateWithCString(kCFAllocatorDefault, "wav", kCFStringEncodingUTF8);
    CFURLRef url = CFBundleCopyResourceURL(CFBundleGetMainBundle(), name, ext, NULL);
    CFRelease(name);
    CFRelease(ext);
    return url;
}

int test_create_and_dispose() {
    SystemSoundID soundID = 0;
    CFURLRef url = toneURL();
    if (!url) { printf("FAIL: bundled tone.wav resource URL missing\n"); return 1; }
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
    return 0;
}

int test_repeat_creation_distinct() {
    CFURLRef url1 = toneURL();
    CFURLRef url2 = toneURL();
    if (!url1 || !url2) { printf("FAIL: bundled distinct tone URLs missing\n"); return 1; }

    SystemSoundID id1 = 0, id2 = 0;
    const int status1 = AudioServicesCreateSystemSoundID(url1, &id1);
    const int status2 = AudioServicesCreateSystemSoundID(url2, &id2);

    if (status1 != kAudioServicesNoError || status2 != kAudioServicesNoError || id1 == id2 || id1 == 0 || id2 == 0) {
         printf("FAIL: id1 (%u) and id2 (%u) should be distinct non-zero\n", (unsigned)id1, (unsigned)id2);
         return 1;
    }

    if (AudioServicesDisposeSystemSoundID(id1) != kAudioServicesNoError ||
        AudioServicesDisposeSystemSoundID(id2) != kAudioServicesNoError) return 1;
    CFRelease(url1);
    CFRelease(url2);
    return 0;
}

int test_repeat_creation_same() {
    CFURLRef url1 = toneURL();
    if (!url1) { printf("FAIL: bundled shared tone URL missing\n"); return 1; }

    SystemSoundID id1 = 0, id2 = 0;
    const int status1 = AudioServicesCreateSystemSoundID(url1, &id1);
    const int status2 = AudioServicesCreateSystemSoundID(url1, &id2);

    if (status1 != kAudioServicesNoError || status2 != kAudioServicesNoError || id1 == id2 || id1 == 0 || id2 == 0) {
         printf("FAIL: id1 (%u) and id2 (%u) should be distinct non-zero even with same URL\n", (unsigned)id1, (unsigned)id2);
         return 1;
    }

    if (AudioServicesDisposeSystemSoundID(id1) != kAudioServicesNoError ||
        AudioServicesDisposeSystemSoundID(id2) != kAudioServicesNoError) return 1;
    CFRelease(url1);
    return 0;
}

int test_invalid_dispose() {
    // Deliberately no assertion on OSStatus until a native iOS invalid-ID
    // oracle establishes the exact result. Must not crash or create a voice.
    (void)AudioServicesDisposeSystemSoundID(99999);
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
