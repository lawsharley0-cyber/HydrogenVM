#pragma once
#define RTLD_DEFAULT ((void *)-2)
void *dlsym(void *, const char *);
void *dlopen(const char *, int);
