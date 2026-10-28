// Minimal CoreFoundation declarations for HydrogenVM's test apps.
#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef struct __CFString *CFMutableStringRef;
typedef const struct __CFArray *CFArrayRef;
typedef struct __CFArray *CFMutableArrayRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFNumber *CFNumberRef;
typedef const struct __CFBoolean *CFBooleanRef;
typedef const struct __CFData *CFDataRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFURL *CFURLRef;
typedef const struct __CFUUID *CFUUIDRef;
typedef struct __CFRunLoop *CFRunLoopRef;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef unsigned long CFHashCode;
typedef uint32_t CFStringEncoding;
typedef double CFTimeInterval;
typedef double CFAbsoluteTime;
typedef unsigned char Boolean;
typedef struct { CFIndex location, length; } CFRange;
static inline CFRange CFRangeMake(CFIndex loc, CFIndex len) { CFRange r = {loc, len}; return r; }
typedef CFIndex CFComparisonResult;
enum { kCFCompareLessThan = -1, kCFCompareEqualTo = 0, kCFCompareGreaterThan = 1 };
enum { kCFCompareCaseInsensitive = 1 };
enum { kCFStringEncodingUTF8 = 0x08000100, kCFStringEncodingASCII = 0x0600, kCFStringEncodingMacRoman = 0 };
typedef CFIndex CFNumberType;
enum { kCFNumberSInt32Type = 3, kCFNumberSInt64Type = 4, kCFNumberFloat32Type = 5, kCFNumberFloat64Type = 6, kCFNumberIntType = 9, kCFNumberDoubleType = 13 };
typedef struct { CFIndex version; const void *retain, *release, *copyDescription, *equal; } CFArrayCallBacks;
typedef struct { CFIndex version; const void *retain, *release, *copyDescription, *equal, *hash; } CFDictionaryKeyCallBacks;
typedef struct { CFIndex version; const void *retain, *release, *copyDescription, *equal; } CFDictionaryValueCallBacks;
extern const CFAllocatorRef kCFAllocatorDefault;
extern const CFArrayCallBacks kCFTypeArrayCallBacks;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryKeyCallBacks kCFCopyStringDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
extern const CFBooleanRef kCFBooleanTrue, kCFBooleanFalse;
#ifdef __OBJC__
#define CFSTR(s) ((CFStringRef)@s)
#else
#define CFSTR(s) ((CFStringRef)__builtin___CFStringMakeConstantString(s))
#endif
CFTypeRef CFRetain(CFTypeRef);
void CFRelease(CFTypeRef);
CFIndex CFGetRetainCount(CFTypeRef);
Boolean CFEqual(CFTypeRef, CFTypeRef);
CFHashCode CFHash(CFTypeRef);
CFTypeID CFGetTypeID(CFTypeRef);
CFTypeID CFStringGetTypeID(void);
CFTypeID CFArrayGetTypeID(void);
CFTypeID CFDictionaryGetTypeID(void);
CFTypeID CFNumberGetTypeID(void);
CFStringRef CFCopyDescription(CFTypeRef);
CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, CFStringEncoding);
CFStringRef CFStringCreateWithFormat(CFAllocatorRef, CFDictionaryRef, CFStringRef, ...);
CFMutableStringRef CFStringCreateMutable(CFAllocatorRef, CFIndex);
CFMutableStringRef CFStringCreateMutableCopy(CFAllocatorRef, CFIndex, CFStringRef);
CFIndex CFStringGetLength(CFStringRef);
Boolean CFStringGetCString(CFStringRef, char *, CFIndex, CFStringEncoding);
const char *CFStringGetCStringPtr(CFStringRef, CFStringEncoding);
void CFStringAppend(CFMutableStringRef, CFStringRef);
void CFStringAppendCString(CFMutableStringRef, const char *, CFStringEncoding);
CFComparisonResult CFStringCompare(CFStringRef, CFStringRef, CFIndex);
CFRange CFStringFind(CFStringRef, CFStringRef, CFIndex);
Boolean CFStringHasPrefix(CFStringRef, CFStringRef);
int32_t CFStringGetIntValue(CFStringRef);
CFArrayRef CFStringCreateArrayBySeparatingStrings(CFAllocatorRef, CFStringRef, CFStringRef);
CFNumberRef CFNumberCreate(CFAllocatorRef, CFNumberType, const void *);
Boolean CFNumberGetValue(CFNumberRef, CFNumberType, void *);
Boolean CFBooleanGetValue(CFBooleanRef);
CFArrayRef CFArrayCreate(CFAllocatorRef, const void **, CFIndex, const CFArrayCallBacks *);
CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef, CFIndex, const CFArrayCallBacks *);
CFIndex CFArrayGetCount(CFArrayRef);
const void *CFArrayGetValueAtIndex(CFArrayRef, CFIndex);
void CFArrayAppendValue(CFMutableArrayRef, const void *);
void CFArrayRemoveValueAtIndex(CFMutableArrayRef, CFIndex);
Boolean CFArrayContainsValue(CFArrayRef, CFRange, const void *);
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef, const void **, const void **, CFIndex, const CFDictionaryKeyCallBacks *, const CFDictionaryValueCallBacks *);
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef, CFIndex, const CFDictionaryKeyCallBacks *, const CFDictionaryValueCallBacks *);
CFIndex CFDictionaryGetCount(CFDictionaryRef);
const void *CFDictionaryGetValue(CFDictionaryRef, const void *);
Boolean CFDictionaryGetValueIfPresent(CFDictionaryRef, const void *, const void **);
void CFDictionarySetValue(CFMutableDictionaryRef, const void *, const void *);
void CFDictionaryRemoveValue(CFMutableDictionaryRef, const void *);
CFDataRef CFDataCreate(CFAllocatorRef, const uint8_t *, CFIndex);
CFIndex CFDataGetLength(CFDataRef);
const uint8_t *CFDataGetBytePtr(CFDataRef);
CFAbsoluteTime CFAbsoluteTimeGetCurrent(void);
CFUUIDRef CFUUIDCreate(CFAllocatorRef);
CFStringRef CFUUIDCreateString(CFAllocatorRef, CFUUIDRef);
CFRunLoopRef CFRunLoopGetCurrent(void);
CFRunLoopRef CFRunLoopGetMain(void);
int32_t CFRunLoopRunInMode(CFStringRef mode, CFTimeInterval seconds, Boolean returnAfterSourceHandled);
void CFRunLoopStop(CFRunLoopRef);
extern const CFStringRef kCFRunLoopDefaultMode;
typedef struct { CFIndex version; void *info; const void *retain, *release, *copyDescription; } CFRunLoopTimerContext;
typedef struct __CFRunLoopTimer *CFRunLoopTimerRef;
typedef void (*CFRunLoopTimerCallBack)(CFRunLoopTimerRef timer, void *info);
CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef, CFAbsoluteTime fireDate, CFTimeInterval interval, unsigned long flags, CFIndex order, CFRunLoopTimerCallBack cb, CFRunLoopTimerContext *ctx);
void CFRunLoopAddTimer(CFRunLoopRef, CFRunLoopTimerRef, CFStringRef mode);
void CFRunLoopTimerInvalidate(CFRunLoopTimerRef);
typedef struct __CFBundle *CFBundleRef;
CFBundleRef CFBundleGetMainBundle(void);
CFStringRef CFBundleGetIdentifier(CFBundleRef);
CFURLRef CFBundleCopyResourceURL(CFBundleRef, CFStringRef name, CFStringRef type, CFStringRef subdir);
Boolean CFURLGetFileSystemRepresentation(CFURLRef, Boolean resolveAgainstBase, uint8_t *buffer, CFIndex maxBufLen);
CFTypeRef CFPreferencesCopyAppValue(CFStringRef key, CFStringRef appId);
void CFPreferencesSetAppValue(CFStringRef key, CFTypeRef value, CFStringRef appId);
extern const CFStringRef kCFPreferencesCurrentApplication;

typedef const struct __CFURL *CFURLRef;
CFURLRef CFURLCreateWithString(CFAllocatorRef allocator, CFStringRef URLString, CFURLRef baseURL);
