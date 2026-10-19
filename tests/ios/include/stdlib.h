#pragma once
#include <stddef.h>
#define NULL ((void*)0)
#define RAND_MAX 0x7fffffff
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
int atoi(const char *);
long atol(const char *);
double atof(const char *);
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
long long strtoll(const char *, char **, int);
double strtod(const char *, char **);
float strtof(const char *, char **);
int abs(int);
long labs(long);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
int rand(void);
void srand(unsigned);
long random(void);
void srandom(unsigned);
unsigned arc4random(void);
unsigned arc4random_uniform(unsigned);
char *getenv(const char *);
void exit(int) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*)(void));
