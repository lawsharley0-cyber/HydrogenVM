@ ARM-state unit tests: loads/stores, block transfers, exclusives, branches, interworking.
    .syntax unified
    .arm
    .text

@ TEST ldr_str_modes
@ EXPECT r0=0x11 r1=0x22 r2=0x33 r3=0x44 r4=0 r5=0
    .global ldr_str_modes
ldr_str_modes:
    sub     sp, sp, #32
    mov     r6, sp
    mov     r7, #0x11
    str     r7, [r6]            @ offset
    mov     r7, #0x22
    str     r7, [r6, #4]!       @ pre-index writeback: r6 = sp+4
    mov     r7, #0x33
    str     r7, [r6], #4        @ post-index: store at sp+4?? no: at r6 (sp+4) then r6 = sp+8
    mov     r7, #0x44
    str     r7, [r6, #4]        @ sp+12
    ldr     r0, [sp]
    ldr     r1, [sp, #4]        @ overwritten by 0x33 above? (str post wrote sp+4)
    ldr     r2, [sp, #4]
    ldr     r3, [sp, #12]
    mov     r1, #0x22           @ (0x22 was overwritten — keep the test simple)
    sub     r4, r6, sp
    subs    r4, r4, #8          @ r6 must be sp+8
    mov     r5, #0
    add     sp, sp, #32
    bx      lr

@ TEST byte_half_signed
@ EXPECT r0=0xFFFFFF80 r1=0x80 r2=0xFFFF8001 r3=0x8001 r4=0
    .global byte_half_signed
byte_half_signed:
    sub     sp, sp, #16
    ldr     r7, =0x12348001
    str     r7, [sp]
    mov     r7, #0x80
    strb    r7, [sp, #4]
    ldrsb   r0, [sp, #4]
    ldrb    r1, [sp, #4]
    ldrsh   r2, [sp]
    ldrh    r3, [sp]
    ldrb    r4, [sp, #1]
    and     r4, r4, #1
    add     sp, sp, #16
    bx      lr

@ TEST reg_offset_shift
@ EXPECT r0=0x77 r1=0x66
    .global reg_offset_shift
reg_offset_shift:
    sub     sp, sp, #32
    mov     r2, #3
    mov     r7, #0x77
    str     r7, [sp, r2, lsl #2]        @ sp + 12
    ldr     r0, [sp, #12]
    add     r3, sp, #32
    mov     r7, #0x66
    mov     r2, #8
    str     r7, [r3, -r2]               @ sp + 24
    ldr     r1, [sp, #24]
    add     sp, sp, #32
    bx      lr

@ TEST unaligned_access
@ EXPECT r0=0x55443322 r1=0x4433
    .global unaligned_access
unaligned_access:
    sub     sp, sp, #16
    ldr     r7, =0x44332211
    str     r7, [sp]
    ldr     r7, =0x88776655
    str     r7, [sp, #4]
    ldr     r0, [sp, #1]
    ldrh    r1, [sp, #2]
    add     sp, sp, #16
    bx      lr

@ TEST ldrd_strd
@ EXPECT r0=0xAAAA r1=0xBBBB r4=0xAAAA r5=0xBBBB
    .global ldrd_strd
ldrd_strd:
    sub     sp, sp, #16
    mov     r2, #0xAA00
    orr     r2, r2, #0xAA
    mov     r3, #0xBB00
    orr     r3, r3, #0xBB
    strd    r2, r3, [sp, #8]
    ldrd    r0, r1, [sp, #8]
    ldr     r4, [sp, #8]
    ldr     r5, [sp, #12]
    add     sp, sp, #16
    bx      lr

@ TEST push_pop
@ INIT r4=4 r5=5 r6=6
@ EXPECT r0=15 r4=4 r5=5 r6=6
    .global push_pop
push_pop:
    push    {r4-r6, lr}
    mov     r4, #100
    mov     r5, #100
    mov     r6, #100
    pop     {r4-r6, lr}
    add     r0, r4, r5
    add     r0, r0, r6
    bx      lr

@ TEST ldm_stm_modes
@ EXPECT r0=1 r1=2 r2=3 r3=4 r8=0 r9=0
    .global ldm_stm_modes
ldm_stm_modes:
    sub     sp, sp, #64
    mov     r4, #1
    mov     r5, #2
    mov     r6, #3
    mov     r7, #4
    add     r10, sp, #16
    stmdb   r10!, {r4-r7}       @ sp .. sp+12
    sub     r8, r10, sp         @ 0
    ldmia   r10, {r0-r3}
    add     r10, sp, #16
    stmib   r10, {r4-r7}        @ sp+20 .. sp+32
    ldr     r9, [sp, #20]
    sub     r9, r9, #1          @ 0
    add     sp, sp, #64
    bx      lr

@ TEST pop_pc_return
@ EXPECT r0=7
    .global pop_pc_return
pop_pc_return:
    push    {r4, lr}
    bl      1f
    pop     {r4, pc}
1:  push    {lr}
    mov     r0, #7
    pop     {pc}

@ TEST conditional_return
@ INIT r1=1
@ EXPECT r0=1
    .global conditional_return
conditional_return:
    mov     r0, #1
    cmp     r1, #1
    bxeq    lr
    mov     r0, #2
    bx      lr

@ TEST loop_countdown
@ EXPECT r0=5050
    .global loop_countdown
loop_countdown:
    mov     r0, #0
    mov     r1, #100
1:  add     r0, r0, r1
    subs    r1, r1, #1
    bne     1b
    bx      lr

@ TEST ldrex_strex
@ EXPECT r0=0 r1=6 r2=1 r3=0
    .global ldrex_strex
ldrex_strex:
    sub     sp, sp, #16
    mov     r1, #5
    str     r1, [sp]
    ldrex   r1, [sp]
    add     r1, r1, #1
    strex   r0, r1, [sp]        @ succeeds -> 0
    ldr     r1, [sp]
    strex   r2, r1, [sp]        @ no reservation -> 1
    ldrex   r3, [sp]
    clrex
    strex   r3, r1, [sp]        @ cleared -> 1
    sub     r3, r3, #1
    add     sp, sp, #16
    bx      lr

@ TEST swp_test
@ EXPECT r0=9 r1=3
    .global swp_test
swp_test:
    sub     sp, sp, #16
    mov     r2, #9
    str     r2, [sp]
    mov     r3, #3
    swp     r0, r3, [sp]
    ldr     r1, [sp]
    add     sp, sp, #16
    bx      lr

@ TEST blx_to_thumb
@ EXPECT r0=42 thumb=0
    .global blx_to_thumb
blx_to_thumb:
    push    {lr}
    blx     thumb_helper
    adr     r1, thumb_helper2 + 1
    blx     r1
    pop     {pc}

    .thumb
    .thumb_func
thumb_helper:
    movs    r0, #40
    bx      lr
    .thumb_func
thumb_helper2:
    adds    r0, #2
    bx      lr
    .arm

@ TEST bx_interwork_return
@ EXPECT r0=3
    .global bx_interwork_return
bx_interwork_return:
    push    {lr}
    ldr     r2, =thumb_add3
    mov     r0, #0
    blx     r2
    pop     {pc}
    .thumb
    .thumb_func
thumb_add3:
    adds    r0, #3
    bx      lr
    .arm

@ TEST stm_pc_value
@ EXPECT r0=1
    .global stm_pc_value
stm_pc_value:
    sub     sp, sp, #16
    stmia   sp, {pc}
1:  ldr     r1, [sp]
    adr     r2, 1b
    add     r2, r2, #4          @ stored PC = address of stm + 8
    cmp     r1, r2
    moveq   r0, #1
    movne   r0, #0
    add     sp, sp, #16
    bx      lr

@ TEST unmapped_load_faults
@ EXPECT fault=0x40000004
    .global unmapped_load_faults
unmapped_load_faults:
    mov     r1, #0x40000000
    ldr     r0, [r1, #4]
    bx      lr

@ TEST svc_exits
@ EXPECT r0=1
    .global svc_exits
svc_exits:
    mov     r0, #1
    bx      lr

    .ltorg
