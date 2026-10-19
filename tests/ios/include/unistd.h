#pragma once
#include <stddef.h>
unsigned sleep(unsigned);
int usleep(unsigned);
int getpid(void);
int isatty(int);
int access(const char *, int);
long sysconf(int);
char *getcwd(char *, size_t);
