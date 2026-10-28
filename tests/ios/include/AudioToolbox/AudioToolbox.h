#ifndef __AUDIOTOOLBOX_H__
#define __AUDIOTOOLBOX_H__
#include <CoreFoundation/CoreFoundation.h>

typedef uint32_t SystemSoundID;
typedef int (*AudioServicesSystemSoundCompletionProc)(SystemSoundID ssID, void *clientData);

enum {
    kAudioServicesNoError = 0
};

int AudioServicesCreateSystemSoundID(CFURLRef inFileURL, SystemSoundID *outSystemSoundID);
int AudioServicesDisposeSystemSoundID(SystemSoundID inSystemSoundID);
void AudioServicesPlaySystemSound(SystemSoundID inSystemSoundID);

#endif
