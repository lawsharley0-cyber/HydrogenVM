#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct objc_class *Class;
typedef struct objc_object { Class isa; } *id;
typedef struct objc_selector *SEL;
typedef id (*IMP)(id, SEL, ...);
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#define nil ((id)0)
#define Nil ((Class)0)
typedef struct objc_method *Method;
typedef struct objc_ivar *Ivar;
typedef struct objc_property *objc_property_t;
#ifdef __OBJC__
@class Protocol;
#else
typedef struct objc_object Protocol;
#endif
const char *sel_getName(SEL);
SEL sel_registerName(const char *);
