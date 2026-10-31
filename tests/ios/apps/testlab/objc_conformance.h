// Project-authored T8 fixtures. No upstream code or test harness is included.
#pragma once
#include <stdint.h>

#define TL_OBJC_MAX_CHECKS 20
typedef struct {
    uint32_t count;
    const char *oracle;
    const char *names[TL_OBJC_MAX_CHECKS];
    int32_t expected[TL_OBJC_MAX_CHECKS];
    int32_t actual[TL_OBJC_MAX_CHECKS];
} TLObjCObservation;

static inline void TLObjCCheck(TLObjCObservation *out, const char *name,
                               int32_t actual, int32_t expected) {
    if (out->count >= TL_OBJC_MAX_CHECKS) {
        out->count = TL_OBJC_MAX_CHECKS + 1; // main reports a fixture error
        return;
    }
    const uint32_t i = out->count++;
    out->names[i] = name;
    out->actual[i] = actual;
    out->expected[i] = expected;
}

void TLObjCSelectors(TLObjCObservation *out);
void TLObjCClassGraph(TLObjCObservation *out);
void TLObjCMethodLookup(TLObjCObservation *out);
void TLObjCMutation(TLObjCObservation *out);
void TLObjCLifetime(TLObjCObservation *out);
