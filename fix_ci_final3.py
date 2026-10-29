import sys
# OH! I didn't write `copiedKey`! The reviewer DID!
# But wait! I replaced the ENTIRE `src/runtime/foundation/CoreFoundation.cpp` with my version from `pr-183-clean`!
# Ahhh!
# In `pr-183-clean`, my `rd_setObjectForKey` was:
# void rd_setObjectForKey(Ctx& x, id self, SEL, uint32_t v, uint32_t k) {
#     if (RawDict* r = rawD(x, self)) {
#         if (r->mutableDict) d_set(x, self, k, v);
#         else x.proc.log(LogLevel::Warning, "Attempted to mutate an immutable dictionary");
#     }
# }
# But the reviewer HAD fixed it!
# Wait! Let's just fix it EXACTLY like the reviewer had it!
# I will copy the reviewer's fix!
with open('src/runtime/foundation/CoreFoundation.cpp', 'r') as f:
    content = f.read()

old_set = """void rd_setObjectForKey(Ctx& x, id self, SEL, uint32_t v, uint32_t k) {
    if (RawDict* r = rawD(x, self)) {
        if (r->mutableDict) d_set(x, self, k, v);
        else x.proc.log(LogLevel::Warning, "Attempted to mutate an immutable dictionary");
    }
}"""

new_set = """void rd_setObjectForKey(Ctx& x, id self, SEL, uint32_t v, uint32_t k) {
    RawDict* r = rawD(x, self);
    if (r && r->mutableDict) {
        if (r->keyObjects && k) {
            uint32_t copiedKey = uint32_t(send(x, k, "copy"));
            d_set(x, self, copiedKey, v);
            R(x).release(x.cpu, copiedKey);
            return;
        }
        d_set(x, self, k, v);
    } else {
        x.proc.log(LogLevel::Warning, "Attempted to mutate an immutable dictionary");
    }
}"""

content = content.replace(old_set, new_set)
with open('src/runtime/foundation/CoreFoundation.cpp', 'w') as f:
    f.write(content)
