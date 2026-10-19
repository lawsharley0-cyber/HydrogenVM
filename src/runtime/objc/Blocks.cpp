// HydrogenVM — the blocks runtime (libclosure): _Block_copy/_Block_release,
// __block variable copying, and the block classes.
//
// Layouts (32-bit):
//   block:  isa, flags, reserved, invoke, descriptor, captured...
//   descriptor: reserved, size, [copy_helper, dispose_helper], [signature]
//   byref:  isa, forwarding, flags, size, [keep, destroy], variable...
#include "ObjC.hpp"

namespace Hydrogen::Runtime::ObjC {
namespace {

enum : uint32_t {
    BLOCK_REFCOUNT_MASK = 0xFFFE,
    BLOCK_NEEDS_FREE = 1u << 24,
    BLOCK_HAS_COPY_DISPOSE = 1u << 25,
    BLOCK_IS_GLOBAL = 1u << 28,
    BLOCK_FIELD_IS_OBJECT = 3, BLOCK_FIELD_IS_BLOCK = 7, BLOCK_FIELD_IS_BYREF = 8,
    BLOCK_FIELD_IS_WEAK = 16, BLOCK_BYREF_CALLER = 128,
};

struct BlockState {
    explicit BlockState(Process&) {}
    uint32_t mallocBlockClass = 0;
    std::mutex mu;
};

uint32_t& flagsRef(Ctx& x, uint32_t p, uint32_t off) { return *reinterpret_cast<uint32_t*>(x.mem.host(p + off)); }

uint32_t latchingIncr(uint32_t* where) {
    for (;;) {
        uint32_t old = __atomic_load_n(where, __ATOMIC_RELAXED);
        if ((old & BLOCK_REFCOUNT_MASK) == BLOCK_REFCOUNT_MASK) return BLOCK_REFCOUNT_MASK;
        if (__atomic_compare_exchange_n(where, &old, old + 2, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return old + 2;
    }
}
// Returns true when the count drops to zero.
bool latchingDecr(uint32_t* where) {
    for (;;) {
        uint32_t old = __atomic_load_n(where, __ATOMIC_RELAXED);
        if ((old & BLOCK_REFCOUNT_MASK) == BLOCK_REFCOUNT_MASK) return false;
        if ((old & BLOCK_REFCOUNT_MASK) == 0) return false;
        const uint32_t nw = old - 2;
        if (__atomic_compare_exchange_n(where, &old, nw, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return (nw & BLOCK_REFCOUNT_MASK) == 0;
    }
}

uint32_t blockCopy(Ctx& x, uint32_t b) {
    if (!b) return 0;
    uint32_t& flags = flagsRef(x, b, 4);
    if (flags & BLOCK_NEEDS_FREE) { latchingIncr(&flags); return b; }
    if (flags & BLOCK_IS_GLOBAL) return b;
    const uint32_t desc = x.mem.read<uint32_t>(b + 16);
    const uint32_t size = x.mem.read<uint32_t>(desc + 4);
    const uint32_t copy = x.proc.heap().alloc(std::max<uint32_t>(size, 20));
    std::memcpy(x.mem.host(copy), x.mem.host(b), size);
    uint32_t nf = x.mem.read<uint32_t>(copy + 4);
    nf &= ~(BLOCK_REFCOUNT_MASK | 0x8000u);
    nf |= BLOCK_NEEDS_FREE | 2;
    x.mem.write<uint32_t>(copy + 4, nf);
    x.mem.write<uint32_t>(copy, x.proc.service<BlockState>().mallocBlockClass);
    if (nf & BLOCK_HAS_COPY_DISPOSE) {
        const uint32_t helper = x.mem.read<uint32_t>(desc + 8);
        if (helper) x.proc.callGuest(x.cpu, helper, {copy, b});
    }
    return copy;
}

void blockRelease(Ctx& x, uint32_t b) {
    if (!b) return;
    uint32_t& flags = flagsRef(x, b, 4);
    if (!(flags & BLOCK_NEEDS_FREE)) return;   // global or stack
    if (!latchingDecr(&flags)) return;
    if (flags & BLOCK_HAS_COPY_DISPOSE) {
        const uint32_t desc = x.mem.read<uint32_t>(b + 16);
        const uint32_t dispose = x.mem.read<uint32_t>(desc + 12);
        if (dispose) x.proc.callGuest(x.cpu, dispose, {b});
    }
    x.proc.heap().free(b);
}

uint32_t byrefCopy(Ctx& x, uint32_t src) {
    const uint32_t fwd = x.mem.read<uint32_t>(src + 4);
    uint32_t& fflags = flagsRef(x, fwd, 8);
    if ((fflags & BLOCK_REFCOUNT_MASK) == 0) {
        const uint32_t size = x.mem.read<uint32_t>(src + 12);
        const uint32_t copy = x.proc.heap().allocZeroed(std::max<uint32_t>(size, 16));
        const uint32_t sflags = x.mem.read<uint32_t>(src + 8);
        x.mem.write<uint32_t>(copy, 0);
        x.mem.write<uint32_t>(copy + 4, copy);
        x.mem.write<uint32_t>(copy + 8, (sflags & ~BLOCK_REFCOUNT_MASK) | BLOCK_NEEDS_FREE | 4);   // one for caller, one for stack
        x.mem.write<uint32_t>(copy + 12, size);
        x.mem.write<uint32_t>(src + 4, copy);
        if (sflags & BLOCK_HAS_COPY_DISPOSE) {
            const uint32_t keep = x.mem.read<uint32_t>(src + 16), destroy = x.mem.read<uint32_t>(src + 20);
            x.mem.write<uint32_t>(copy + 16, keep);
            x.mem.write<uint32_t>(copy + 20, destroy);
            if (size > 24) std::memcpy(x.mem.host(copy + 24), x.mem.host(src + 24), size - 24);   // layout / raw fields
            if (keep) x.proc.callGuest(x.cpu, keep, {copy, src});
        } else if (size > 16) {
            std::memcpy(x.mem.host(copy + 16), x.mem.host(src + 16), size - 16);
        }
        return copy;
    }
    if (fflags & BLOCK_NEEDS_FREE) latchingIncr(&fflags);
    return fwd;
}

void byrefRelease(Ctx& x, uint32_t byref) {
    const uint32_t b = x.mem.read<uint32_t>(byref + 4);   // forwarding
    uint32_t& flags = flagsRef(x, b, 8);
    if (!(flags & BLOCK_NEEDS_FREE)) return;
    if (!latchingDecr(&flags)) return;
    if (flags & BLOCK_HAS_COPY_DISPOSE) {
        const uint32_t destroy = x.mem.read<uint32_t>(b + 20);
        if (destroy) x.proc.callGuest(x.cpu, destroy, {b});
    }
    x.proc.heap().free(b);
}

uint32_t b_copy(Ctx& x, uint32_t b) { return blockCopy(x, b); }
void b_release(Ctx& x, uint32_t b) { blockRelease(x, b); }
void b_objectAssign(Ctx& x, GuestPtr dst, uint32_t obj, int32_t flags) {
    switch (uint32_t(flags) & (BLOCK_FIELD_IS_OBJECT | BLOCK_FIELD_IS_BLOCK | BLOCK_FIELD_IS_BYREF | BLOCK_FIELD_IS_WEAK | BLOCK_BYREF_CALLER)) {
        case BLOCK_FIELD_IS_OBJECT: x.mem.write<uint32_t>(dst, rt(x).retain(x.cpu, obj)); break;
        case BLOCK_FIELD_IS_BLOCK: x.mem.write<uint32_t>(dst, blockCopy(x, obj)); break;
        case BLOCK_FIELD_IS_BYREF | BLOCK_FIELD_IS_WEAK:
        case BLOCK_FIELD_IS_BYREF: x.mem.write<uint32_t>(dst, byrefCopy(x, obj)); break;
        default: x.mem.write<uint32_t>(dst, obj); break;   // BYREF_CALLER variants: plain assignment
    }
}
void b_objectDispose(Ctx& x, uint32_t obj, int32_t flags) {
    switch (uint32_t(flags) & (BLOCK_FIELD_IS_OBJECT | BLOCK_FIELD_IS_BLOCK | BLOCK_FIELD_IS_BYREF | BLOCK_FIELD_IS_WEAK | BLOCK_BYREF_CALLER)) {
        case BLOCK_FIELD_IS_OBJECT: rt(x).release(x.cpu, obj); break;
        case BLOCK_FIELD_IS_BLOCK: blockRelease(x, obj); break;
        case BLOCK_FIELD_IS_BYREF | BLOCK_FIELD_IS_WEAK:
        case BLOCK_FIELD_IS_BYREF: byrefRelease(x, obj); break;
        default: break;
    }
}
uint32_t b_retainBlock(Ctx& x, uint32_t b) { return blockCopy(x, b); }

// Block objects respond to retain/release/copy like ObjC objects.
id bl_copy(Ctx& x, id self, SEL) { return blockCopy(x, self); }
id bl_copyWithZone(Ctx& x, id self, SEL, uint32_t) { return blockCopy(x, self); }
id bl_retain(Ctx& x, id self, SEL) { return blockCopy(x, self); }
void bl_release(Ctx& x, id self, SEL) { blockRelease(x, self); }
id bl_autorelease(Ctx& x, id self, SEL) { rt(x).addToPool(self); return self; }
uint32_t bl_retainCount(Ctx& x, id self, SEL) { return (x.mem.read<uint32_t>(self + 4) & BLOCK_REFCOUNT_MASK) / 2; }
id bl_self(Ctx&, id self, SEL) { return self; }
void bl_noop(Ctx&, id, SEL) {}

} // namespace

void registerBlocks(Process& p, Runtime& r) {
    ClassBuilder nsblock = r.defineClass("NSBlock", "NSObject");
    nsblock.m<&bl_copy>("copy").m<&bl_copyWithZone>("copyWithZone:").m<&bl_retain>("retain")
           .m<&bl_release>("release", "v@:").m<&bl_autorelease>("autorelease").m<&bl_retainCount>("retainCount", "I@:");
    ClassBuilder stack = r.defineClass("__NSStackBlock__", "NSBlock");
    stack.m<&bl_noop>("release", "v@:").m<&bl_self>("autorelease");
    ClassBuilder malloc = r.defineClass("__NSMallocBlock__", "NSBlock");
    ClassBuilder global = r.defineClass("__NSGlobalBlock__", "NSBlock");
    global.m<&bl_self>("copy").m<&bl_self>("retain").m<&bl_noop>("release", "v@:").m<&bl_self>("autorelease");
    p.service<BlockState>().mallocBlockClass = malloc.cls();
    p.defineData("__NSConcreteStackBlock", stack.cls());
    p.defineData("__NSConcreteMallocBlock", malloc.cls());
    p.defineData("__NSConcreteGlobalBlock", global.cls());
    p.defineData("__NSConcreteAutoBlock", malloc.cls());
    p.defineData("__NSConcreteFinalizingBlock", malloc.cls());
    p.defineData("__NSConcreteWeakBlockVariable", malloc.cls());
    r.makeImmortal(global.cls());
    p.def<&b_copy>("__Block_copy");
    p.def<&b_release>("__Block_release");
    p.def<&b_objectAssign>("__Block_object_assign");
    p.def<&b_objectDispose>("__Block_object_dispose");
    p.def<&b_retainBlock>("_objc_retainBlock");
}

} // namespace Hydrogen::Runtime::ObjC
