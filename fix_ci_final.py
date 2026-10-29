import sys
# Ah!
# The CI failed with:
# "exit 1 (want 0); expected line missing"
# FAIL rd_setObjectForKey copies key, retains key copy and value (line 435)
# FAIL CFDictionarySetValue only retains key (line 447)
# --- stderr ---
# NSLog works: yes 1

# My test in `main.m` FAILED.
# Why did it fail?
# `CHECK(firstKey->_copyCount == 1 && firstKey->_retainCount == 1 && valObj->_retainCount == 1, "rd_setObjectForKey copies key, retains key copy and value");`
# And `CHECK(secondKey == copyKey && copyKey->_copyCount == 0 && copyKey->_retainCount == 1 && valObj->_retainCount == 1, "CFDictionarySetValue only retains key");`
# Why did these fail?
# Because `_copyCount` and `_retainCount` aren't what I expect them to be!
# `firstKey->_retainCount == 1`?
# In `__HVMCFRawDictionary` mutators (`CoreFoundation.cpp`):
# `void rd_setObjectForKey(Ctx& x, id self, SEL, uint32_t v, uint32_t k)`
# If `mutableDict` is true, it calls `d_set(x, self, k, v)`.
# `d_setMode` calls `rawSet`.
# `rawSet` does NOT call `setObject:forKey:` if it's a `RawDict`.
# `rawSet` manually checks `d.keyObjects` and `d.valueObjects`.
# `if (d.keyObjects) key = d.keyEqual && isString(x, key) ? id(send(x, key, "copy")) : R(x).retain(x.cpu, key);`
# It DOES NOT call `copy` unless it's a string AND `keyEqual` is true!
# `_kCFTypeDictionaryKeyCallBacks` has `keyEqual = true`?
# But `isString` might be FALSE! `MyCopyKey` is NOT a string!
# So it DOES NOT copy the key! It only retains it!
# Therefore, `firstKey->_copyCount` is 0, not 1!
# Wait, `firstKey` was passed to `setObject:forKey:`.
# Objective-C's `setObject:forKey:` in `NSMutableDictionary` DOES copy the key.
# But HydrogenVM's `rawSet` specifically does `isString(x, key) ? id(send(x, key, "copy")) : R(x).retain(x.cpu, key);`.
# So it ONLY copies strings.
# This means my test is failing because it expects a copy but HydrogenVM intentionally only copies strings!
