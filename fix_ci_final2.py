import sys
# Wait, let's verify what `isString` does.
# Is `MyCopyKey` an NSString? No.
# If I change my test in `main.m` to expect `_copyCount == 0` for `rd_setObjectForKey`?
# But wait, `rd_setObjectForKey` is supposed to accurately reflect `[dict setObject:forKey:]`.
# In native Cocoa, `setObject:forKey:` copies the key REGARDLESS of whether it's a string, as long as the dictionary copies keys!
# The reviewer wrote:
# "fix: rd_setObjectForKey copies keys and retains copies/values"
# Ah!
# The reviewer WANTS ME to copy the key in `rd_setObjectForKey`!
# Let me look at `rd_setObjectForKey` again.
