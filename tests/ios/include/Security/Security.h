#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>

/* Minimal Security.framework surface for the Test Lab keychain case. */
#ifndef __OSSTATUS_DEFINED
#define __OSSTATUS_DEFINED
typedef int32_t OSStatus;
#endif
enum { errSecSuccess = 0, errSecDuplicateItem = -25299, errSecItemNotFound = -25300 };

extern const CFStringRef kSecClass;
extern const CFStringRef kSecClassGenericPassword;
extern const CFStringRef kSecAttrService;
extern const CFStringRef kSecAttrAccount;
extern const CFStringRef kSecValueData;
extern const CFStringRef kSecReturnData;
extern const CFStringRef kSecMatchLimit;
extern const CFStringRef kSecMatchLimitOne;

OSStatus SecItemAdd(CFDictionaryRef attributes, CFTypeRef *result);
OSStatus SecItemCopyMatching(CFDictionaryRef query, CFTypeRef *result);
OSStatus SecItemUpdate(CFDictionaryRef query, CFDictionaryRef attributesToUpdate);
OSStatus SecItemDelete(CFDictionaryRef query);
