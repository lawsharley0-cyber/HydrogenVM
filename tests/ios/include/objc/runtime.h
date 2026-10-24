#pragma once
#include <objc/objc.h>
Class objc_getClass(const char *);
Class object_getClass(id);
const char *class_getName(Class);
Class class_getSuperclass(Class);
BOOL class_isMetaClass(Class);
size_t class_getInstanceSize(Class);
Method class_getInstanceMethod(Class, SEL);
Method class_getClassMethod(Class, SEL);
IMP method_getImplementation(Method);
IMP method_setImplementation(Method, IMP);
void method_exchangeImplementations(Method, Method);
BOOL class_addMethod(Class, SEL, IMP, const char *);
IMP class_replaceMethod(Class, SEL, IMP, const char *);
BOOL class_respondsToSelector(Class, SEL);
Ivar class_getInstanceVariable(Class, const char *);
Ivar object_getInstanceVariable(id, const char *, void **);
ptrdiff_t ivar_getOffset(Ivar);
const char *ivar_getName(Ivar);
Ivar *class_copyIvarList(Class, unsigned *);
objc_property_t *class_copyPropertyList(Class, unsigned *);
const char *property_getName(objc_property_t);
const char *property_getAttributes(objc_property_t);
Class objc_allocateClassPair(Class, const char *, size_t);
void objc_registerClassPair(Class);
BOOL class_addIvar(Class, const char *, size_t, uint8_t, const char *);
id class_createInstance(Class, size_t);
Protocol *objc_getProtocol(const char *);
BOOL class_conformsToProtocol(Class, Protocol *);
typedef enum { OBJC_ASSOCIATION_ASSIGN = 0, OBJC_ASSOCIATION_RETAIN_NONATOMIC = 1, OBJC_ASSOCIATION_COPY_NONATOMIC = 3,
               OBJC_ASSOCIATION_RETAIN = 01401, OBJC_ASSOCIATION_COPY = 01403 } objc_AssociationPolicy;
void objc_setAssociatedObject(id, const void *, id, objc_AssociationPolicy);
id objc_getAssociatedObject(id, const void *);
id objc_msgSend(id, SEL, ...);
