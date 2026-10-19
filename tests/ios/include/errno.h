#pragma once
int *__error(void);
#define errno (*__error())
#define ENOENT 2
#define EINVAL 22
#define EAGAIN 35
#define ETIMEDOUT 60
