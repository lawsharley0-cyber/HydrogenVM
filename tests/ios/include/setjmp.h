#pragma once
typedef int jmp_buf[27];
typedef int sigjmp_buf[28];
int setjmp(jmp_buf);
void longjmp(jmp_buf, int) __attribute__((noreturn));
int _setjmp(jmp_buf);
void _longjmp(jmp_buf, int) __attribute__((noreturn));
