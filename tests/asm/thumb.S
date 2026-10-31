@ Thumb / Thumb-2 unit tests.
    .syntax unified
    .thumb
    .text

@ TEST t_flags_outside_it
@ EXPECT r0=0 nzcv=0x6
    .global t_flags_outside_it
    .thumb_func
t_flags_outside_it:
    movs    r0, #1
    subs    r0, #1          @ 16-bit SUBS outside IT sets flags
    bx      lr

@ TEST t_it_block
@ INIT r1=5 r2=3
@ EXPECT r0=1 r3=2 r4=0 r5=7 nzcv=0x2
    .global t_it_block
    .thumb_func
t_it_block:
    movs    r4, #0
    cmp     r1, r2
    ite     gt
    movgt   r0, #1
    movle   r0, #2
    itte    lt
    movlt   r3, #1
    movlt   r4, #9
    movge   r3, #2
    it      hi
    addhi   r5, r1, #2      @ 16-bit ADD inside IT: does not set flags
    bx      lr

@ TEST t_it_no_flags
@ INIT r1=0
@ EXPECT r0=0xFFFFFFFF nzcv=0x6
    .global t_it_no_flags
    .thumb_func
t_it_no_flags:
    cmp     r1, #0          @ Z=1 C=1
    it      eq
    subeq   r0, r1, #1      @ inside IT: flags untouched
    bx      lr

@ TEST t_it_four
@ INIT r1=1
@ EXPECT r0=0xF r2=0
    .global t_it_four
    .thumb_func
t_it_four:
    movs    r0, #0
    movs    r2, #0
    cmp     r1, #1
    itett   eq
    orreq   r0, r0, #1
    orrne   r2, r2, #1
    orreq   r0, r0, #2
    orreq   r0, r0, #12
    bx      lr

@ TEST t_it_return
@ INIT r1=1
@ EXPECT r0=10
    .global t_it_return
    .thumb_func
t_it_return:
    movs    r0, #10
    cmp     r1, #1
    it      eq
    bxeq    lr
    movs    r0, #20
    bx      lr

@ TEST t_cbz_cbnz
@ INIT r1=0 r2=5
@ EXPECT r0=3
    .global t_cbz_cbnz
    .thumb_func
t_cbz_cbnz:
    movs    r0, #0
    cbz     r1, 1f
    adds    r0, #100
1:  adds    r0, #1
    cbnz    r2, 2f
    adds    r0, #100
2:  adds    r0, #2
    bx      lr

@ TEST t_tbb
@ INIT r1=2
@ EXPECT r0=30
    .global t_tbb
    .thumb_func
t_tbb:
    tbb     [pc, r1]
1:  .byte   (10f - 1b) / 2
    .byte   (20f - 1b) / 2
    .byte   (30f - 1b) / 2
    .byte   0
10: movs    r0, #10
    bx      lr
20: movs    r0, #20
    bx      lr
30: movs    r0, #30
    bx      lr

@ TEST t_tbh
@ INIT r1=1
@ EXPECT r0=22
    .global t_tbh
    .thumb_func
t_tbh:
    adr     r2, 1f
    tbh     [r2, r1, lsl #1]
3:  .p2align 2
1:  .hword  (10f - 3b) / 2
    .hword  (20f - 3b) / 2
10: movs    r0, #11
    bx      lr
20: movs    r0, #22
    bx      lr

@ TEST t_literal_adr
@ EXPECT r0=0x12345678 r1=1
    .global t_literal_adr
    .thumb_func
t_literal_adr:
    ldr     r0, =0x12345678
    adr     r2, 1f
    ldr     r3, [r2]
    movs    r1, #0
    cmp     r3, r0
    it      eq
    moveq   r1, #1
    bx      lr
    .p2align 2
1:  .word   0x12345678
    .ltorg

@ TEST t_movw_movt_wide
@ EXPECT r0=0xCAFEF00D r1=0x00FF00FF r2=0xFF00FF00 r3=0xABABABAB r4=0x3FC00000
    .global t_movw_movt_wide
    .thumb_func
t_movw_movt_wide:
    movw    r0, #0xF00D
    movt    r0, #0xCAFE
    mov.w   r1, #0x00FF00FF
    mov.w   r2, #0xFF00FF00
    mov.w   r3, #0xABABABAB
    mov.w   r4, #0x3FC00000
    bx      lr

@ TEST t_wide_dp
@ INIT r1=100 r2=0x0F0F
@ EXPECT r0=0 r3=0xFF00F00F r4=4095 r5=0xFF00FF0F r6=0x2EF nzcv=0x0
    .global t_wide_dp
    .thumb_func
t_wide_dp:
    lsl.w   r0, r2, #8
    and.w   r0, r0, #0xF000
    add.w   r0, r0, r1
    sub.w   r0, r0, #100
    eor.w   r3, r2, #0xFF00FF00
    movw    r4, #4095
    orn     r5, r2, #0x00FF00FF
    mvn     r5, r5
    mvn     r5, r5
    add.w   r6, r1, r1, lsl #3
    sub     r6, r6, #(900 - 0x2EF)
    adds    r7, r1, #0      @ clear flags (C=0)
    bx      lr

@ TEST t_shifts_reg
@ INIT r1=0x80000001 r2=1 r3=32 r4=33
@ EXPECT r0=0x40000000 r5=0 r6=0xFFFFFFFF r7=0xC0000000
    .global t_shifts_reg
    .thumb_func
t_shifts_reg:
    lsr.w   r0, r1, r2
    lsl.w   r5, r1, r3
    asr.w   r6, r1, r4
    ror.w   r7, r1, r2
    bx      lr

@ TEST t_push_pop_pc
@ EXPECT r0=9
    .global t_push_pop_pc
    .thumb_func
t_push_pop_pc:
    push    {r4-r7, lr}
    movs    r4, #4
    movs    r5, #5
    bl      1f
    adds    r0, r4, r5
    pop     {r4-r7, pc}
1:  push    {r4, lr}
    movs    r4, #77
    pop     {r4, pc}

@ TEST t_push_pop_wide
@ EXPECT r0=36
    .global t_push_pop_wide
    .thumb_func
t_push_pop_wide:
    push.w  {r4-r11, lr}
    movs    r0, #0
    mov     r8, #8
    mov     r9, #9
    mov     r10, #10
    mov     r11, #9
    add     r0, r8
    add     r0, r9
    add     r0, r10
    add     r0, r11
    pop.w   {r4-r11, pc}

@ TEST t_ldr_str_wide
@ EXPECT r0=0x11223344 r1=0x44 r2=0x3344 r3=0xFFFFFF88 r4=8
    .global t_ldr_str_wide
    .thumb_func
t_ldr_str_wide:
    sub     sp, #32
    ldr     r5, =0x11223344
    str.w   r5, [sp, #16]
    ldr.w   r0, [sp, #16]
    ldrb.w  r1, [sp, #16]
    ldrh.w  r2, [sp, #16]
    movs    r5, #0x88
    strb.w  r5, [sp, #20]
    add     r6, sp, #20
    ldrsb   r3, [r6, #-0]
    add     r7, sp, #0
    str     r5, [r7, #8]!   @ pre-index
    mov     r6, sp
    sub     r4, r7, r6
    add     sp, #32
    bx      lr
    .ltorg

@ TEST t_ldr_post_reg
@ EXPECT r0=3 r1=4 r2=8
    .global t_ldr_post_reg
    .thumb_func
t_ldr_post_reg:
    sub     sp, #16
    movs    r3, #3
    str     r3, [sp]
    movs    r3, #4
    str     r3, [sp, #4]
    mov     r5, sp
    ldr     r0, [r5], #4
    ldr     r1, [r5], #4
    mov     r2, sp
    sub     r2, r5, r2
    movs    r6, #1
    ldr     r7, [sp, r6, lsl #2]
    add     sp, #16
    bx      lr

@ TEST t_ldrd_strd
@ EXPECT r0=7 r1=8 r2=1
    .global t_ldrd_strd
    .thumb_func
t_ldrd_strd:
    sub     sp, #16
    movs    r4, #7
    movs    r5, #8
    strd    r4, r5, [sp, #8]
    ldrd    r0, r1, [sp, #8]
    ldr     r3, [sp, #12]
    movs    r2, #0
    cmp     r3, #8
    it      eq
    moveq   r2, #1
    add     sp, #16
    bx      lr

@ TEST t_exclusive
@ EXPECT r0=0 r1=11 r2=0x0201
    .global t_exclusive
    .thumb_func
t_exclusive:
    sub     sp, #16
    movs    r1, #10
    str     r1, [sp]
1:  ldrex   r1, [sp]
    adds    r1, #1
    strex   r0, r1, [sp]
    cmp     r0, #0
    bne     1b
    movs    r3, #1
    strb    r3, [sp, #4]
    add     r6, sp, #4
    ldrexb  r2, [r6]
    adds    r2, #1
    strexb  r3, r2, [r6]
    ldrh    r2, [sp, #4]
    lsls    r2, r2, #8
    orr     r2, r2, #1
    add     sp, #16
    bx      lr

@ TEST t_bitfield_wide
@ INIT r1=0xABCD1234
@ EXPECT r0=0xCD r2=0xFFFFFFCD r3=0xABCDF234 r4=0xAB0D1234
    .global t_bitfield_wide
    .thumb_func
t_bitfield_wide:
    ubfx    r0, r1, #16, #8
    sbfx    r2, r1, #16, #8
    mov     r3, r1
    movs    r5, #0xF
    bfi     r3, r5, #12, #4
    mov     r4, r1
    bfc     r4, #20, #4
    bx      lr

@ TEST t_mul_div
@ INIT r1=0xFFFFFFFE r2=3
@ EXPECT r0=0xFFFFFFFA r3=2 r4=0xFFFFFFFA r5=0xFFFFFFFF r6=0x55555554 r7=0xFFFFFFFF
    .global t_mul_div
    .thumb_func
t_mul_div:
    mul     r0, r1, r2
    umull   r4, r3, r1, r2      @ 0x2_FFFFFFFA
    smull   r4, r5, r1, r2      @ -6
    udiv    r6, r1, r2
    sdiv    r7, r1, r2          @ -2/3 = 0 ... adjust below
    subs    r7, r7, #1
    bx      lr

@ TEST t_mla_mls
@ INIT r1=3 r2=4 r3=100
@ EXPECT r0=112 r4=88 r5=12
    .global t_mla_mls
    .thumb_func
t_mla_mls:
    mla     r0, r1, r2, r3
    mls     r4, r1, r2, r3
    muls    r5, r1, r5          @ 16-bit MULS: r5 = r1 * r5... r5 starts 0
    mul     r5, r1, r2
    bx      lr

@ TEST t_extend_rev
@ INIT r1=0x80F17F81
@ EXPECT r0=0xFFFFFF81 r2=0x7F81 r3=0x817FF180 r4=0x00F1 r5=0xFFFFFFF1
    .global t_extend_rev
    .thumb_func
t_extend_rev:
    sxtb    r0, r1
    uxth    r2, r1
    rev     r3, r1
    uxtb.w  r4, r1, ror #16
    sxtb.w  r5, r1, ror #16
    bx      lr

@ TEST t_blx_to_arm
@ EXPECT r0=50 thumb=0
    .global t_blx_to_arm
    .thumb_func
t_blx_to_arm:
    push    {lr}
    blx     arm_helper
    ldr     r1, =arm_helper2
    blx     r1
    pop     {pc}
    .ltorg

    .arm
arm_helper:
    mov     r0, #45
    bx      lr
arm_helper2:
    add     r0, r0, #5
    bx      lr
    .thumb

@ TEST t_mov_pc_lr
@ EXPECT r0=4
    .global t_mov_pc_lr
    .thumb_func
t_mov_pc_lr:
    push    {lr}
    bl      1f
    pop     {pc}
1:  movs    r0, #4
    mov     pc, lr          @ BranchWritePC: stays in Thumb

@ TEST t_add_pc_table
@ INIT r1=2
@ EXPECT r0=3
    .global t_add_pc_table
    .thumb_func
t_add_pc_table:
    lsls    r1, r1, #1      @ index * 2 (each entry is a 16-bit B)
    add     pc, r1          @ PC reads as this insn + 4
    nop
    b       10f
    b       20f
    b       30f
10: movs    r0, #1
    bx      lr
20: movs    r0, #2
    bx      lr
30: movs    r0, #3
    bx      lr

@ TEST t_wide_cond_branch
@ INIT r1=1000
@ EXPECT r0=1000
    .global t_wide_cond_branch
    .thumb_func
t_wide_cond_branch:
    movs    r0, #0
1:  adds    r0, #1
    cmp     r0, r1
    bne.w   1b
    bx      lr

@ TEST t_ssat_usat
@ INIT r1=1000 r2=0xFFFFFC18
@ EXPECT r0=127 r3=0xFFFFFF80 r4=255 r5=0
    .global t_ssat_usat
    .thumb_func
t_ssat_usat:
    ssat    r0, #8, r1
    ssat    r3, #8, r2
    usat    r4, #8, r1
    usat    r5, #8, r2
    bx      lr

@ TEST t_clz_rbit
@ INIT r1=0x00010000
@ EXPECT r0=15 r2=0x00008000
    .global t_clz_rbit
    .thumb_func
t_clz_rbit:
    clz     r0, r1
    rbit    r2, r1
    bx      lr

@ TEST t_adc_sbc_wide
@ INIT r0=0xFFFFFFFF r1=0 r2=1 r3=0
@ EXPECT r0=0 r1=1 r4=0xFFFFFFFF r5=0
    .global t_adc_sbc_wide
    .thumb_func
t_adc_sbc_wide:
    adds.w  r0, r0, r2
    adc.w   r1, r1, r3
    subs.w  r4, r0, #1      @ 0 - 1: borrow
    sbc     r5, r1, #0      @ 1 - 0 - 1 = 0
    bx      lr

@ TEST t_ldm_stm_16
@ EXPECT r0=6 r1=0xC
    .global t_ldm_stm_16
    .thumb_func
t_ldm_stm_16:
    sub     sp, #16
    mov     r4, sp
    movs    r1, #1
    movs    r2, #2
    movs    r3, #3
    stmia   r4!, {r1-r3}
    mov     r5, sp
    ldmia   r5!, {r1-r3}
    adds    r0, r1, r2
    adds    r0, r0, r3
    mov     r1, sp
    subs    r1, r5, r1
    add     sp, #16
    bx      lr

@ TEST t_undefined_inst
@ EXPECT undef=0xDE00
    .global t_undefined_inst
    .thumb_func
t_undefined_inst:
    .hword  0xDE00          @ UDF #0 (16-bit)
    bx      lr
