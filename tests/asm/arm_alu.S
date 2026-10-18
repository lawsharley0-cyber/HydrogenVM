@ ARM-state unit tests: data processing, flags, shifts, conditional execution.
    .syntax unified
    .arm
    .text

@ TEST adds_overflow
@ EXPECT r0=0x80000000 nzcv=0x9
    .global adds_overflow
adds_overflow:
    mvn     r0, #0x80000000
    adds    r0, r0, #1
    bx      lr

@ TEST adds_carry
@ EXPECT r0=0 nzcv=0x6
    .global adds_carry
adds_carry:
    mvn     r0, #0
    adds    r0, r0, #1
    bx      lr

@ TEST subs_borrow
@ EXPECT r0=0xFFFFFFFF nzcv=0x8
    .global subs_borrow
subs_borrow:
    mov     r0, #0
    subs    r0, r0, #1
    bx      lr

@ TEST subs_equal
@ EXPECT r0=0 nzcv=0x6
    .global subs_equal
subs_equal:
    mov     r0, #5
    subs    r0, r0, #5
    bx      lr

@ TEST adc_chain
@ INIT r0=0xFFFFFFFF r1=0 r2=1 r3=0
@ EXPECT r0=0 r1=1 nzcv=0x0
    .global adc_chain
adc_chain:
    adds    r0, r0, r2
    adcs    r1, r1, r3
    bx      lr

@ TEST sbc_chain
@ INIT r0=0 r1=1 r2=1 r3=0
@ EXPECT r0=0xFFFFFFFF r1=0 nzcv=0x6
    .global sbc_chain
sbc_chain:
    subs    r0, r0, r2
    sbcs    r1, r1, r3
    bx      lr

@ TEST rsc_test
@ INIT r1=10 nzcv=0x0
@ EXPECT r0=0xFFFFFFF5
    .global rsc_test
rsc_test:
    rsc     r0, r1, #0          @ 0 - 10 - !C(=1) = -11
    bx      lr

@ TEST cmp_conditions
@ EXPECT r0=0x5A r1=0
    .global cmp_conditions
cmp_conditions:
    mov     r0, #0
    mov     r1, #0
    mov     r2, #3
    cmp     r2, #5
    orrlt   r0, r0, #0x02
    orrlo   r0, r0, #0x08
    orrge   r1, r1, #1
    orrhi   r1, r1, #2
    cmp     r2, #3
    orreq   r0, r0, #0x10
    orrls   r0, r0, #0x40
    orrne   r1, r1, #4
    bx      lr

@ TEST signed_conditions
@ EXPECT r0=0x7
    .global signed_conditions
signed_conditions:
    mov     r0, #0
    mvn     r2, #0              @ -1
    cmp     r2, #1
    orrlt   r0, r0, #1          @ signed: -1 < 1
    orrhi   r0, r0, #2          @ unsigned: 0xFFFFFFFF > 1
    orrmi   r0, r0, #4
    orrvs   r0, r0, #8
    bx      lr

@ TEST movs_shifter_carry
@ INIT r1=0x80000001
@ EXPECT r0=0x00000002 nzcv=0x2
    .global movs_shifter_carry
movs_shifter_carry:
    movs    r0, r1, lsl #1
    bx      lr

@ TEST lsr32_asr32
@ INIT r1=0x80000000
@ EXPECT r0=0 r2=0xFFFFFFFF nzcv=0xA
    .global lsr32_asr32
lsr32_asr32:
    movs    r0, r1, lsr #32
    movs    r2, r1, asr #32
    bx      lr

@ TEST rrx_test
@ INIT r1=0x00000003 nzcv=0x2
@ EXPECT r0=0x80000001 nzcv=0xA
    .global rrx_test
rrx_test:
    movs    r0, r1, rrx
    bx      lr

@ TEST shift_by_register
@ INIT r1=0x80000001 r2=33 r3=32 r4=0 r5=4
@ EXPECT r0=0 r6=1 r7=0x80000001 r8=0x18000000 r9=0xF8000000
    .global shift_by_register
shift_by_register:
    mov     r0, r1, lsl r2      @ >= 32 -> 0
    mov     r6, r1, lsl r3      @ == 32 -> 0, but test flags below
    movs    r6, r1, lsl r3
    movcs   r6, #1              @ carry = bit0 = 1
    mov     r7, r1, ror r3      @ ror 32 -> unchanged
    mov     r8, r1, ror r5      @ 0x18000000
    mov     r9, r1, asr r5      @ 0xF8000000
    bx      lr

@ TEST logical_keeps_v
@ INIT nzcv=0x1
@ EXPECT r0=0 nzcv=0x5
    .global logical_keeps_v
logical_keeps_v:
    ands    r0, r0, #0xFF       @ imm without rotation keeps C; V always kept
    bx      lr

@ TEST imm_rotation_carry
@ EXPECT r0=0xF000000F nzcv=0xA
    .global imm_rotation_carry
imm_rotation_carry:
    movs    r0, #0xF000000F
    bx      lr

@ TEST tst_teq
@ INIT r1=0xF0
@ EXPECT r0=3
    .global tst_teq
tst_teq:
    mov     r0, #0
    tst     r1, #0x0F
    orreq   r0, r0, #1
    teq     r1, #0xF0
    orreq   r0, r0, #2
    bx      lr

@ TEST bic_mvn_orr
@ INIT r1=0xFFFF r2=0xF0F0
@ EXPECT r0=0x0F0F r3=0xFFFF0F0F r4=0xFFFFFFFF
    .global bic_mvn_orr
bic_mvn_orr:
    bic     r0, r1, r2
    mvn     r3, r2
    orr     r4, r3, r2
    bx      lr

@ TEST dead_flags_preserved
@ EXPECT r0=1
    .global dead_flags_preserved
dead_flags_preserved:
    mov     r1, #0
    cmp     r1, #0              @ Z=1 must survive the non-S ops below
    add     r2, r1, #5
    sub     r3, r2, #1
    moveq   r0, #1
    movne   r0, #2
    bx      lr

@ TEST flags_across_blocks
@ EXPECT r0=1
    .global flags_across_blocks
flags_across_blocks:
    mov     r1, #1
    cmp     r1, #2
    b       1f
1:  movlt   r0, #1
    movge   r0, #2
    bx      lr

@ TEST pc_relative_add
@ EXPECT r0=1
    .global pc_relative_add
pc_relative_add:
    add     r1, pc, #4          @ r1 = address of label 2
    mov     r0, #0
    b       3f
2:  .word   0
3:  adr     r2, 2b
    cmp     r1, r2
    moveq   r0, #1
    bx      lr

@ TEST jump_table_add_pc
@ INIT r1=2
@ EXPECT r0=30
    .global jump_table_add_pc
jump_table_add_pc:
    add     pc, pc, r1, lsl #2
    nop
    b       10f
    b       20f
    b       30f
10: mov     r0, #10
    bx      lr
20: mov     r0, #20
    bx      lr
30: mov     r0, #30
    bx      lr

@ TEST ldr_pc_table
@ INIT r1=1
@ EXPECT r0=22
    .global ldr_pc_table
ldr_pc_table:
    ldr     pc, [pc, r1, lsl #2]
    nop
    .word   1f
    .word   2f
1:  mov     r0, #11
    bx      lr
2:  mov     r0, #22
    bx      lr

@ TEST movw_movt
@ EXPECT r0=0xDEADBEEF r1=0x0000BEEF
    .global movw_movt
movw_movt:
    movw    r0, #0xBEEF
    movw    r1, #0xBEEF
    movt    r0, #0xDEAD
    bx      lr

@ TEST mul_family
@ INIT r1=7 r2=6 r3=100
@ EXPECT r0=42 r4=142 r5=58 nzcv=0x0
    .global mul_family
mul_family:
    mul     r0, r1, r2
    mla     r4, r1, r2, r3
    mls     r5, r1, r2, r3
    muls    r6, r1, r2
    bx      lr

@ TEST long_multiplies
@ INIT r2=0xFFFFFFFF r3=0xFFFFFFFF
@ EXPECT r0=1 r1=0xFFFFFFFE r4=1 r5=0 r6=2 r7=0
    .global long_multiplies
long_multiplies:
    umull   r0, r1, r2, r3      @ 0xFFFFFFFE00000001
    smull   r4, r5, r2, r3      @ (-1)*(-1) = 1
    mov     r6, #1
    mov     r7, #0
    smlal   r6, r7, r2, r3      @ 1 + 1 = 2
    bx      lr

@ TEST umaal_test
@ INIT r0=0xFFFFFFFF r1=0xFFFFFFFF r2=0xFFFFFFFF r3=0xFFFFFFFF
@ EXPECT r0=0xFFFFFFFF r1=0xFFFFFFFF
    .global umaal_test
umaal_test:
    umaal   r0, r1, r2, r3      @ max value: (2^32-1)^2 + 2*(2^32-1) = 2^64-1
    bx      lr

@ TEST halfword_mul
@ INIT r1=0x0003FFFE r2=0x00050004
@ EXPECT r0=0xFFFFFFF8 r3=15 r4=0xFFFFFFF6 r5=0xF
    .global halfword_mul
halfword_mul:
    smulbb  r0, r1, r2          @ -2 * 4
    smultt  r3, r1, r2          @ 3 * 5
    smulbt  r4, r1, r2          @ -2 * 5
    mov     r6, #0x10000
    smulwb  r5, r1, r2          @ (0x0003FFFE * 4) >> 16
    bx      lr

@ TEST smmul_smlad
@ INIT r1=0x40000000 r2=0x40000000 r3=0x00020003 r4=0x00040005 r6=100
@ EXPECT r0=0x10000000 r5=23 r7=123
    .global smmul_smlad
smmul_smlad:
    smmul   r0, r1, r2
    smuad   r5, r3, r4
    smlad   r7, r3, r4, r6
    bx      lr

@ TEST qadd_saturates
@ INIT r1=0x7FFFFFFF r2=1
@ EXPECT r0=0x7FFFFFFF r3=0x08000000
    .global qadd_saturates
qadd_saturates:
    qadd    r0, r1, r2
    mrs     r3, apsr
    and     r3, r3, #0x08000000
    bx      lr

@ TEST ssat_usat
@ INIT r1=300 r2=0xFFFFFF00
@ EXPECT r0=127 r3=0xFFFFFF80 r4=255 r5=0 q=1
    .global ssat_usat
ssat_usat:
    ssat    r0, #8, r1
    ssat    r3, #8, r2
    usat    r4, #8, r1
    usat    r5, #8, r2
    bx      lr

@ TEST parallel_add_sel
@ INIT r1=0x8001FFFF r2=0x80000001
@ EXPECT r0=0x00010000 ge=0xF r3=0x8001FFFF r4=0x0000FFFF
    .global parallel_add_sel
parallel_add_sel:
    uadd16  r0, r1, r2          @ lo: FFFF+1 = 10000 (carry, GE[1:0]=1), hi: 8001+8000 = 10001 (carry GE[3:2])
    sel     r3, r1, r2
    uhadd16 r4, r1, r1          @ (a+a)>>1 = a
    and     r4, r4, #0xFF
    orr     r4, r4, #0xFF00
    bx      lr

@ TEST qadd8_sadd16
@ INIT r1=0x7F80017F r2=0x01FF0101
@ EXPECT r0=0x7F80027F r3=0x807F0280
    .global qadd8_sadd16
qadd8_sadd16:
    qadd8   r0, r1, r2
    sadd8   r3, r1, r2
    bx      lr

@ TEST extends
@ INIT r1=0x8081F2F3
@ EXPECT r0=0xFFFFFFF3 r2=0xFFFFF2F3 r3=0xF3 r4=0xF2F3 r5=0x81 r6=0x00820001 r7=0xFF81FFF3
    .global extends
extends:
    sxtb    r0, r1
    sxth    r2, r1
    uxtb    r3, r1
    uxth    r4, r1
    uxtb    r5, r1, ror #16
    mov     r8, #1
    uxtab   r6, r8, r1, ror #16
    lsl     r6, r6, #16
    orr     r6, r6, #1
    sxtb16  r7, r1
    bx      lr

@ TEST bitfield_ops
@ INIT r1=0xABCD1234
@ EXPECT r0=0xCD r2=0xFFFFFFCD r3=0xABCDF234 r4=0xAB0D1234
    .global bitfield_ops
bitfield_ops:
    ubfx    r0, r1, #16, #8
    sbfx    r2, r1, #16, #8
    mov     r3, r1
    mov     r5, #0xF
    bfi     r3, r5, #12, #4
    mov     r4, r1
    bfc     r4, #20, #4
    bx      lr

@ TEST rev_clz_rbit
@ INIT r1=0x12345678
@ EXPECT r0=0x78563412 r2=0x34127856 r3=3 r4=0x1E6A2C48 r5=0x7856
    .global rev_clz_rbit
rev_clz_rbit:
    rev     r0, r1
    rev16   r2, r1
    clz     r3, r1
    rbit    r4, r1
    revsh   r5, r1
    bx      lr

@ TEST pkh_test
@ INIT r1=0x11112222 r2=0x33334444
@ EXPECT r0=0x44442222 r3=0x11113333
    .global pkh_test
pkh_test:
    pkhbt   r0, r1, r2, lsl #16
    pkhtb   r3, r1, r2, asr #16
    bx      lr

@ TEST sdiv_udiv
@ INIT r1=0xFFFFFF9C r2=7 r3=0
@ EXPECT r0=0xFFFFFFF2 r4=0x24924916 r5=0 r6=0x80000000
    .global sdiv_udiv
sdiv_udiv:
    sdiv    r0, r1, r2          @ -100 / 7 = -14
    udiv    r4, r1, r2
    udiv    r5, r1, r3          @ divide by zero -> 0
    mov     r7, #0x80000000
    mvn     r8, #0
    sdiv    r6, r7, r8          @ overflow -> INT_MIN
    bx      lr

@ TEST usad8_test
@ INIT r1=0x01020304 r2=0x04030201 r3=10
@ EXPECT r0=8 r4=18
    .global usad8_test
usad8_test:
    usad8   r0, r1, r2
    usada8  r4, r1, r2, r3
    bx      lr

@ TEST msr_flags
@ EXPECT r0=1 r1=0xF0000000
    .global msr_flags
msr_flags:
    mov     r2, #0xF0000000
    msr     APSR_nzcvq, r2
    mov     r0, #0
    moveq   r0, #1
    mrs     r1, apsr
    and     r1, r1, #0xF0000000
    bx      lr

@ TEST tpidr_read
@ EXPECT r0=0
    .global tpidr_read
tpidr_read:
    mrc     p15, 0, r0, c13, c0, 3
    bx      lr
