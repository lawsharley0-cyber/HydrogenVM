@ Stand-in for the iPhone OS 3.x/4.x crt1.o (Csu start.s), so the test suite
@ can produce LC_UNIXTHREAD binaries with the classic (pre-dyld-info) layout,
@ the same way old App Store builds look.
    .syntax unified
    .text
    .align 2
    .globl start
    .arm
start:
    ldr     r0, [sp]            @ argc
    add     r1, sp, #4          @ argv
    add     r4, r0, #1
    add     r2, r1, r4, lsl #2  @ envp
    bic     sp, sp, #7
    mov     r3, r2
1:  ldr     r4, [r3], #4        @ skip to the end of envp; next word is "apple"
    cmp     r4, #0
    bne     1b
    bl      _main
    b       _exit

    .globl dyld_stub_binding_helper
dyld_stub_binding_helper:
    ldr     ip, L_lazy
    ldr     pc, [ip]
L_lazy:
    .long   dyld__lazy_entry

    .globl __dyld_func_lookup
__dyld_func_lookup:
    ldr     ip, L_lookup
    ldr     pc, [ip]
L_lookup:
    .long   dyld__func_lookup

    .section __DATA,__dyld
    .align 2
dyld__lazy_entry:
    .long   0
dyld__func_lookup:
    .long   0
