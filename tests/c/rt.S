@ Minimal freestanding runtime for guest test programs (ARM state; reached from
@ Thumb code via BLX, which also exercises interworking).
    .syntax unified
    .arm
    .text

@ unsigned 32-bit divide: r0 = r0 / r1, r1 = r0 % r1 (shift-subtract)
    .global __aeabi_uidivmod
    .global __aeabi_uidiv
    .type __aeabi_uidivmod, %function
    .type __aeabi_uidiv, %function
__aeabi_uidiv:
__aeabi_uidivmod:
    cmp     r1, #0
    moveq   r0, #0
    bxeq    lr
    mov     r2, #0          @ quotient
    mov     r3, #1          @ bit
1:  cmp     r1, r0
    bhs     2f
    tst     r1, #0x80000000
    bne     2f
    lsl     r1, r1, #1
    lsl     r3, r3, #1
    b       1b
2:  cmp     r0, r1
    subhs   r0, r0, r1
    orrhs   r2, r2, r3
    lsrs    r3, r3, #1
    lsr     r1, r1, #1
    bne     2b
    mov     r1, r0
    mov     r0, r2
    bx      lr

@ signed 32-bit divide
    .global __aeabi_idivmod
    .global __aeabi_idiv
    .type __aeabi_idivmod, %function
    .type __aeabi_idiv, %function
__aeabi_idiv:
__aeabi_idivmod:
    push    {r4, r5, lr}
    eor     r4, r0, r1      @ sign of quotient
    mov     r5, r0          @ sign of remainder = sign of dividend
    cmp     r0, #0
    rsblt   r0, r0, #0
    cmp     r1, #0
    rsblt   r1, r1, #0
    bl      __aeabi_uidivmod
    cmp     r4, #0
    rsblt   r0, r0, #0
    cmp     r5, #0
    rsblt   r1, r1, #0
    pop     {r4, r5, pc}

@ 64-bit divide: {r0:r1 quotient, r2:r3 remainder}
    .global __aeabi_uldivmod
    .type __aeabi_uldivmod, %function
__aeabi_uldivmod:
    push    {r4, lr}
    sub     sp, sp, #16
    add     r4, sp, #8
    str     r4, [sp]
    bl      rt_udivmod64
    ldr     r2, [sp, #8]
    ldr     r3, [sp, #12]
    add     sp, sp, #16
    pop     {r4, pc}

    .global __aeabi_ldivmod
    .type __aeabi_ldivmod, %function
__aeabi_ldivmod:
    push    {r4, lr}
    sub     sp, sp, #16
    add     r4, sp, #8
    str     r4, [sp]
    bl      rt_sdivmod64
    ldr     r2, [sp, #8]
    ldr     r3, [sp, #12]
    add     sp, sp, #16
    pop     {r4, pc}

@ memcpy / memset / memmove and their AEABI aliases
    .global memcpy
    .global __aeabi_memcpy
    .global __aeabi_memcpy4
    .global __aeabi_memcpy8
    .type memcpy, %function
memcpy:
__aeabi_memcpy:
__aeabi_memcpy4:
__aeabi_memcpy8:
    mov     r3, r0
    cmp     r2, #0
    bxeq    lr
1:  ldrb    r12, [r1], #1
    strb    r12, [r3], #1
    subs    r2, r2, #1
    bne     1b
    bx      lr

    .global memmove
    .global __aeabi_memmove
    .global __aeabi_memmove4
    .type memmove, %function
memmove:
__aeabi_memmove:
__aeabi_memmove4:
    cmp     r0, r1
    bls     memcpy
    add     r3, r0, r2
    add     r1, r1, r2
    cmp     r2, #0
    bxeq    lr
1:  ldrb    r12, [r1, #-1]!
    strb    r12, [r3, #-1]!
    subs    r2, r2, #1
    bne     1b
    bx      lr

    .global memset
    .type memset, %function
memset:                     @ (dst, value, n)
    mov     r3, r0
    cmp     r2, #0
    bxeq    lr
1:  strb    r1, [r3], #1
    subs    r2, r2, #1
    bne     1b
    bx      lr

    .global __aeabi_memset
    .global __aeabi_memset4
    .global __aeabi_memset8
__aeabi_memset:             @ (dst, n, value)
__aeabi_memset4:
__aeabi_memset8:
    mov     r3, r1
    mov     r1, r2
    mov     r2, r3
    b       memset

    .global __aeabi_memclr
    .global __aeabi_memclr4
    .global __aeabi_memclr8
__aeabi_memclr:
__aeabi_memclr4:
__aeabi_memclr8:
    mov     r2, r1
    mov     r1, #0
    b       memset
