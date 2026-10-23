@ VFP and NEON unit tests (ARM state, plus a few Thumb-2 for the encoding translation).
    .syntax unified
    .fpu neon-vfpv4
    .arm
    .text

@ TEST vfp_mla_vs_fma
@ EXPECT r0=0 r1=0x33800000
    .global vfp_mla_vs_fma
vfp_mla_vs_fma:
    ldr     r0, =0x3F800800         @ 1 + 2^-12
    vmov    s0, r0
    vmov    s1, r0
    ldr     r0, =0xBF801000         @ -(1 + 2^-11)
    vmov    s2, r0
    vmov    s3, r0
    vmla.f32 s2, s0, s1             @ product rounds to 1+2^-11 first -> 0
    vfma.f32 s3, s0, s1             @ fused: exact 2^-24
    vmov    r0, s2
    vmov    r1, s3
    bx      lr

@ TEST vfp_nmul_family
@ EXPECT r0=0xC0C00000 r1=0xC0E00000 r2=0x40A00000 r3=0x40800000
    .global vfp_nmul_family
vfp_nmul_family:
    vmov.f32 s0, #2.0
    vmov.f32 s1, #3.0
    vmov.f32 s2, #1.0
    vmov.f32 s4, #1.0
    vmov.f32 s5, #10.0
    vnmul.f32 s3, s0, s1
    vnmla.f32 s2, s0, s1
    vnmls.f32 s4, s0, s1
    vmls.f32 s5, s0, s1
    vmov    r0, s3
    vmov    r1, s2
    vmov    r2, s4
    vmov    r3, s5
    bx      lr

@ TEST vfp_cmp_branch
@ EXPECT r0=1 r1=1 r2=1 r3=1
    .global vfp_cmp_branch
vfp_cmp_branch:
    mov     r1, #0
    mov     r2, #0
    mov     r3, #0
    vmov.f32 s0, #1.0
    vmov.f32 s1, #2.0
    vcmp.f32 s0, s1
    vmrs    APSR_nzcv, fpscr
    movlt   r0, #1
    movge   r0, #2
    vcmp.f32 s1, #0
    vmrs    APSR_nzcv, fpscr
    movgt   r1, #1
    vcmpe.f32 s0, s0
    vmrs    APSR_nzcv, fpscr
    moveq   r2, #1
    vmov.f64 d2, #-1.0
    vcmp.f64 d2, #0
    vmrs    APSR_nzcv, fpscr
    movmi   r3, #1
    bx      lr

@ TEST vfp_cvt_rounding
@ EXPECT r0=2 r1=3 r2=2 r3=0xFFFFFFFE r4=0xFFFFFFFD
    .global vfp_cvt_rounding
vfp_cvt_rounding:
    vmov.f32 s0, #2.5
    vcvtr.s32.f32 s1, s0            @ round to nearest even -> 2
    vmov    r0, s1
    mov     r5, #(1 << 22)          @ RP
    vmsr    fpscr, r5
    vcvtr.s32.f32 s1, s0            @ -> 3
    vmov    r1, s1
    mov     r5, #(2 << 22)          @ RM
    vmsr    fpscr, r5
    vcvtr.s32.f32 s1, s0            @ -> 2
    vmov    r2, s1
    vmov.f32 s2, #-2.5
    vcvt.s32.f32 s3, s2             @ always toward zero -> -2
    vmov    r3, s3
    vcvtr.s32.f32 s3, s2            @ RM -> -3
    vmov    r4, s3
    mov     r5, #0
    vmsr    fpscr, r5
    bx      lr

@ TEST vfp_cvt_fixed
@ EXPECT r0=0x18000 r1=0x3FC00000 r2=0 r3=0x3FF80000
    .global vfp_cvt_fixed
vfp_cvt_fixed:
    vmov.f32 s0, #1.5
    vcvt.s32.f32 s0, s0, #16
    vmov    r0, s0
    mov     r1, #0x180
    vmov    s1, r1
    vcvt.f32.u32 s1, s1, #8
    vmov    r1, s1
    vcvt.f64.f32 d2, s1
    vmov    r2, r3, d2
    bx      lr

@ TEST vfp_double_ops
@ EXPECT r0=0x667F3BCD r1=0x3FF6A09E r2=0 r3=0xC0000000 r4=0 r5=0xBFF00000
    .global vfp_double_ops
vfp_double_ops:
    vmov.f64 d0, #2.0
    vsqrt.f64 d1, d0
    vmov    r0, r1, d1
    vmov.f64 d2, #-2.0
    vmov    r2, r3, d2
    vdiv.f64 d3, d0, d2
    vmov    r4, r5, d3
    bx      lr

@ TEST vfp_push_pop
@ EXPECT r0=0x11111111 r1=0x22222222 r2=0x33333333 r3=0x44444444 r5=0 r6=0x55555555
    .global vfp_push_pop
vfp_push_pop:
    mov     r4, sp
    ldr     r0, =0x11111111
    ldr     r1, =0x22222222
    ldr     r2, =0x33333333
    ldr     r3, =0x44444444
    vmov    d8, r0, r1
    vmov    d9, r2, r3
    vpush   {d8-d9}
    vmov.f64 d8, #1.0
    vmov.f64 d9, #1.0
    vpop    {d8-d9}
    vmov    r0, r1, d8
    vmov    r2, r3, d9
    sub     r5, sp, r4
    sub     sp, sp, #16
    ldr     r6, =0x55555555
    vmov    s20, r6
    vstr    s20, [sp, #8]
    vldr    s21, [sp, #8]
    vmov    r6, s21
    add     sp, sp, #16
    bx      lr

@ TEST vfp_core_moves
@ EXPECT r1=0x40490FDB r4=1 r5=2
    .global vfp_core_moves
vfp_core_moves:
    ldr     r0, =0x40490FDB
    vmov    s5, r0
    vmov    r1, s5
    mov     r2, #1
    mov     r3, #2
    vmov    s6, s7, r2, r3
    vmov    r4, r5, s6, s7
    bx      lr

@ TEST neon_qadd_qc
@ EXPECT r0=0x7F7F7F7F r1=0x7F7F7F7F r2=0x08000000 r3=0x80808080
    .global neon_qadd_qc
neon_qadd_qc:
    mov     r0, #0x7F
    vdup.8  d0, r0
    vmov.i8 d1, #1
    vqadd.s8 d2, d0, d1
    vmov    r0, r1, d2
    vmrs    r2, fpscr
    and     r2, r2, #0x08000000
    vadd.i8 d3, d0, d1
    vmov    r3, r4, d3
    bx      lr

@ TEST neon_rhadd_bsl
@ EXPECT r4=0x03030202 r5=0x05050404 r6=0x04020202 r7=0x08020602
    .global neon_rhadd_bsl
neon_rhadd_bsl:
    ldr     r0, =0x04030201
    ldr     r1, =0x08070605
    vmov    d0, r0, r1
    vmov.i8 d1, #2
    vrhadd.u8 d2, d0, d1
    vmov    r4, r5, d2
    vmov.i16 d3, #0xFF00
    vbsl    d3, d0, d1
    vmov    r6, r7, d3
    bx      lr

@ TEST neon_structure_loads
@ EXPECT r0=0x06040200 r1=0x07050301 r2=0x07060100 r3=0x09080302 r5=0x0C080400 r6=0x0F0B0703 r7=0x03020100 r8=0x03020100 r9=0 r10=0x100 r11=8
    .global neon_structure_loads
neon_structure_loads:
    sub     sp, sp, #64
    mov     r4, sp
    mov     r0, #0
1:  strb    r0, [r4, r0]
    add     r0, r0, #1
    cmp     r0, #64
    bne     1b
    vld2.8  {d0, d1}, [r4]
    vmov    r0, r12, d0
    vmov    r1, r12, d1
    vld3.16 {d2, d3, d4}, [r4]
    vmov    r2, r12, d2
    vmov    r3, r12, d3
    vld4.8  {d4, d5, d6, d7}, [r4]
    vmov    r5, r12, d4
    vmov    r6, r12, d7
    vld1.32 {d8[]}, [r4]
    vmov    r7, r8, d8
    vmov.i32 d9, #0
    vld1.16 {d9[2]}, [r4]
    vmov    r9, r10, d9
    mov     r11, r4
    vld1.8  {d10}, [r11]!
    sub     r11, r11, r4
    add     sp, sp, #64
    bx      lr

@ TEST neon_structure_store
@ EXPECT r0=0x00110001 r1=0x00120002 r2=0x00140004 r3=0x00230022
    .global neon_structure_store
neon_structure_store:
    sub     sp, sp, #32
    ldr     r0, =0x00020001
    ldr     r1, =0x00040003
    vmov    d0, r0, r1
    ldr     r0, =0x00120011
    ldr     r1, =0x00140013
    vmov    d1, r0, r1
    mov     r2, sp
    vst2.16 {d0, d1}, [r2]
    ldr     r0, [sp]
    ldr     r1, [sp, #4]
    ldr     r2, [sp, #12]
    ldr     r3, =0x00230022
    vdup.32 d4, r3
    add     r12, sp, #16
    vst1.32 {d4[1]}, [r12]
    ldr     r3, [sp, #16]
    add     sp, sp, #32
    bx      lr

@ TEST neon_tbl_ext
@ EXPECT r0=0x17001210 r1=0x13001111 r2=0x17EE1210 r3=0x13EE1111 r4=0x16151413 r5=0x09020017
    .global neon_tbl_ext
neon_tbl_ext:
    ldr     r0, =0x13121110
    ldr     r1, =0x17161514
    vmov    d0, r0, r1
    ldr     r0, =0x07090200
    ldr     r1, =0x03FF0101
    vmov    d1, r0, r1
    vtbl.8  d2, {d0}, d1
    vmov    r0, r1, d2
    vmov.i8 d3, #0xEE
    vtbx.8  d3, {d0}, d1
    vmov    r2, r3, d3
    vext.8  d4, d0, d1, #3
    vmov    r4, r5, d4
    bx      lr

@ TEST neon_permutes
@ EXPECT r0=0x11011000 r1=0x13031202 r2=0x15051404 r3=0x17071606 r4=0x03020100 r5=0xA r6=0xC r7=0xB r8=0xD
    .global neon_permutes
neon_permutes:
    push    {r4-r8}
    ldr     r0, =0x03020100
    ldr     r1, =0x07060504
    vmov    d0, r0, r1
    ldr     r0, =0x13121110
    ldr     r1, =0x17161514
    vmov    d1, r0, r1
    vzip.8  d0, d1
    vmov    r0, r1, d0
    vmov    r2, r3, d1
    vuzp.8  d0, d1
    vmov    r4, r12, d0
    mov     r5, #0xA
    mov     r6, #0xB
    vmov    d2, r5, r6
    mov     r5, #0xC
    mov     r6, #0xD
    vmov    d3, r5, r6
    vtrn.32 d2, d3
    vmov    r5, r6, d2
    vmov    r7, r8, d3
    add     sp, sp, #20           @ discard saved r4-r8 so the test sees the results
    bx      lr

@ TEST neon_saturate_narrow
@ EXPECT r0=0x20002000 r1=0x7FFF7FFF r2=0x08000000 r3=0x12341234 r4=0 r5=0xFFFFFFFF r6=0x04080408 r8=0xFF81FF81
    .global neon_saturate_narrow
neon_saturate_narrow:
    push    {r4-r8}
    mov     r0, #0x4000
    vdup.16 d0, r0
    vqdmulh.s16 d1, d0, d0
    vmov    r0, r12, d1
    mov     r1, #0x8000
    vdup.16 d2, r1
    vqdmulh.s16 d3, d2, d2
    vmov    r1, r12, d3
    vmrs    r2, fpscr
    and     r2, r2, #0x08000000
    ldr     r3, =0x00012345
    vdup.32 q3, r3
    vqrshrn.s32 d8, q3, #4
    vmov    r3, r12, d8
    ldr     r4, =0xFF80
    vdup.16 q5, r4
    vqmovun.s16 d12, q5
    vmov    r4, r12, d12
    mov     r5, #300
    vdup.16 q7, r5
    vqmovun.s16 d16, q7
    vmov    r5, r12, d16
    vmov.i8 d20, #0x81
    vshll.u8 q11, d20, #3
    vmov    r6, r12, d22
    vmovl.s8 q12, d20
    vmov    r8, r12, d24
    add     sp, sp, #20
    bx      lr

@ TEST neon_scalar_ops
@ EXPECT r0=0x40C00000 r1=0x40C00000 r2=0x41000000 r3=300 r4=330
    .global neon_scalar_ops
neon_scalar_ops:
    push    {r4}
    vmov.f32 d0, #1.0
    vmov.f32 s1, #3.0
    vmov.f32 q1, #2.0
    vmul.f32 q2, q1, d0[1]
    vmov    r0, s8
    vmov    r1, s11
    vmla.f32 q2, q1, d0[0]
    vmov    r2, s9
    mov     r3, #3
    vdup.16 d6, r3
    vmov.i16 d7, #10
    mov     r3, #100
    vmov.16 d7[2], r3
    vmull.s16 q4, d6, d7[2]
    vmov    r3, s16
    vmlal.s16 q4, d6, d7[0]
    vmov    r4, s17
    add     sp, sp, #4
    bx      lr

@ TEST neon_estimates_cvt
@ EXPECT r0=0x3EFF8000 r1=0x3EFF8000 r2=0x3F804000 r3=0x7FFFFFFF r4=0 r5=0xFF800000 r6=0x3F348000
    .global neon_estimates_cvt
neon_estimates_cvt:
    push    {r4-r6}
    vmov.f32 d0, #2.0
    vrecpe.f32 d1, d0
    vmov    r0, s2
    vmov.f32 d2, #4.0
    vrsqrte.f32 d3, d2
    vmov    r1, s6
    vrecps.f32 d4, d0, d1
    vmov    r2, s8
    ldr     r3, =0x4F32D05E           @ 3e9
    vdup.32 d5, r3
    vcvt.s32.f32 d6, d5
    vmov    r3, s12
    vmov.f32 d7, #-1.0
    vcvt.u32.f32 d8, d7
    vmov    r4, s16
    mov     r5, #0x80000000
    vdup.32 d9, r5
    vrecpe.u32 d10, d9
    vmov    r5, s20
    vmov.f32 d11, #2.0
    vrsqrte.f32 d12, d11
    vmov    r6, s24
    add     sp, sp, #12
    bx      lr

@ TEST neon_bits_imm
@ EXPECT r0=0x04040404 r1=4 r2=0x03030303 r3=0x00FF0000 r4=0xFEFFFEFF r5=0 r6=0xFFFFFFFF r7=0x00FF00FF r8=0xFF00FF00
    .global neon_bits_imm
neon_bits_imm:
    push    {r4-r8}
    vmov.i8 d0, #0x0F
    vcnt.8  d1, d0
    vmov    r0, r12, d1
    vclz.i32 d2, d0
    vmov    r1, r12, d2
    vcls.s8 d3, d0
    vmov    r2, r12, d3
    vmov.i32 d4, #0x00FF0000
    vmov    r3, r12, d4
    vmvn.i16 d5, #0x0100
    vmov    r4, r12, d5
    vbic.i32 d4, #0x00FF0000
    vmov    r5, r12, d4
    vorr.i16 d5, #0x0100
    vmov    r6, r12, d5
    vmov.i64 d6, #0xFF00FF0000FF00FF
    vmov    r7, r8, d6
    add     sp, sp, #20
    bx      lr

@ TEST neon_pairwise_shift
@ EXPECT r0=0x00070003 r1=0x0046001E r2=0x00040002 r3=0x00280014 r4=0x000A0005 r5=0x00050003 r6=0x000A0008 r7=0xFF00FF00 r8=0x00010000
    .global neon_pairwise_shift
neon_pairwise_shift:
    push    {r4-r8}
    ldr     r0, =0x00020001
    ldr     r1, =0x00040003
    vmov    d0, r0, r1
    ldr     r0, =0x0014000A
    ldr     r1, =0x0028001E
    vmov    d1, r0, r1
    vpadd.i16 d2, d0, d1
    vmov    r0, r1, d2
    vpmax.s16 d3, d0, d1
    vmov    r2, r3, d3
    vmov.i8 d5, #0xFF
    vshl.s16 d4, d1, d5
    vmov    r4, r12, d4
    vmov.i8 d7, #0xFE
    vrshl.s16 d6, d1, d7
    vmov    r5, r6, d6
    vmov.i8 d8, #0xFF
    vsri.16 d8, d0, #8
    vmov    r7, r12, d8
    vmov.i32 d9, #0
    vsli.32 d9, d0, #16
    vmov    r8, r12, d9
    add     sp, sp, #20
    bx      lr

@ TEST neon_lane_moves
@ EXPECT r0=0x44 r1=0xFFFFFF88 r2=0xFFFF8877 r3=0x88776655 r4=0x443322AB r5=0x12341234
    .global neon_lane_moves
neon_lane_moves:
    push    {r4, r5}
    ldr     r0, =0x44332211
    ldr     r1, =0x88776655
    vmov    d0, r0, r1
    vmov.u8 r0, d0[3]
    vmov.s8 r1, d0[7]
    vmov.s16 r2, d0[3]
    vmov.32 r3, d0[1]
    mov     r4, #0xAB
    vmov.8  d0[0], r4
    vmov    r4, r12, d0
    ldr     r5, =0x1234
    vdup.16 d1, r5
    vmov    r5, r12, d1
    add     sp, sp, #8
    bx      lr

@ TEST neon_long_ops
@ EXPECT r0=0x00040004 r1=0x00080008 r2=0x7FFFFFFF r3=0xFFFEFFFE
    .global neon_long_ops
neon_long_ops:
    vmov.i8 d0, #5
    vmov.i8 d1, #9
    vabdl.u8 q1, d0, d1
    vmov    r0, r12, d2
    vabal.u8 q1, d1, d0
    vmov    r1, r12, d2
    mov     r2, #0x8000
    vdup.16 d4, r2
    vqdmull.s16 q3, d4, d4
    vmov    r2, s12
    vmov.i8 d10, #1
    vmov.i8 d11, #3
    vsubl.s8 q6, d10, d11
    vmov    r3, r12, d12
    bx      lr

@ TEST half_precision
@ EXPECT r0=0xC1003555 r1=0x3EAAA000 r2=0xC0200000 r3=0x00000002 r4=0x7BFF3C00 r5=0xC1003555 r6=0x477FE000
    .global half_precision
half_precision:
    push    {r4-r6}
    ldr     r0, =0x3EAAAAAB          @ 1/3
    vmov    s0, r0
    ldr     r0, =0x12345678
    vmov    s1, r0
    vmov.f32 s2, #-2.5
    vcvtb.f16.f32 s1, s0
    vcvtt.f16.f32 s1, s2
    vmov    r0, s1
    vcvtb.f32.f16 s3, s1
    vmov    r1, s3
    vcvtt.f32.f16 s4, s1
    vmov    r2, s4
    ldr     r3, =0x33D6BF95          @ 1e-7 -> half subnormal
    vmov    s5, r3
    mov     r3, #0
    vmov    s6, r3
    vcvtb.f16.f32 s6, s5
    vmov    r3, s6
    vmov.f32 s16, #1.0
    ldr     r4, =0x477FE000          @ 65504
    vmov    s17, r4
    vmov    s18, r0                  @ placeholder, overwritten below
    ldr     r4, =0x3EAAAAAB
    vmov    s18, r4
    vmov.f32 s19, #-2.5
    vcvt.f16.f32 d10, q4
    vmov    r4, r5, d10
    vcvt.f32.f16 q6, d10
    vmov    r6, s25
    add     sp, sp, #12
    bx      lr

@ ------------------------------------------------- Thumb-2 encodings ---
    .thumb

@ TEST t_neon_mix
@ EXPECT r0=0x09060300 r1=0x7F7F7F7F r2=0x17001210 r3=0x3EFF8000 r4=0x40C00000
    .global t_neon_mix
    .thumb_func
t_neon_mix:
    push    {r4, lr}
    sub     sp, #48
    mov     r4, sp
    movs    r0, #0
1:  strb    r0, [r4, r0]
    adds    r0, #1
    cmp     r0, #48
    bne     1b
    vld3.8  {d0, d1, d2}, [r4]
    vmov    r0, r12, d0
    movs    r1, #0x7F
    vdup.8  d3, r1
    vmov.i8 d4, #1
    vqadd.s8 d5, d3, d4
    vmov    r1, r12, d5
    ldr     r2, =0x13121110
    ldr     r3, =0x17161514
    vmov    d6, r2, r3
    ldr     r2, =0x07090200
    ldr     r3, =0x03FF0101
    vmov    d7, r2, r3
    vtbl.8  d8, {d6}, d7
    vmov    r2, r12, d8
    vmov.f32 d9, #2.0
    vrecpe.f32 d10, d9
    vmov    r3, s20
    vmov.f32 s22, #2.0
    vmov.f32 s23, #3.0
    vmul.f32 s24, s22, s23
    vmov    r4, s24
    add     sp, #48
    add     sp, #4                   @ drop saved r4 so the test sees its value
    pop     {pc}
    .ltorg

    .arm
@ NEON float always flushes denormals (inputs and results keep only the sign)
@ and returns the default NaN, in both the D and the Q forms.
@ TEST neon_ftz_default_nan
@ EXPECT r0=0x00000000 r1=0x80000000 r2=0x7FC00000 r3=0x7FC00000
    .global neon_ftz_default_nan
neon_ftz_default_nan:
    ldr     r0, =0x00000001         @ smallest positive denormal
    ldr     r1, =0x80000003         @ negative denormal
    ldr     r2, =0x7F800001         @ signalling NaN
    ldr     r3, =0x7F800000         @ +inf
    vmov    d0, r0, r1
    vmov    d1, r2, r3
    ldr     r0, =0x3F800000
    vdup.32 d6, r0                  @ 1.0
    vmul.f32 d4, d0, d6             @ denormal * 1 -> +0 / -0
    vadd.f32 d5, d1, d6             @ lane0: NaN + 1 -> default NaN
    vmov    d7, d1
    vsub.f32 q4, q3, q3             @ (Q form) inf - inf in lane 1 of d7 -> default NaN
    vmov    r0, r1, d4
    vmov    r2, s10
    vmov    r3, s19
    bx      lr

@ NEON VCVT with fraction bits, Q form, including saturation.
@ TEST neon_cvt_fixed_q
@ EXPECT r0=0x00000018 r1=0xFFFFFFDC r2=0x7FFFFFFF r3=0x0000000C
    .global neon_cvt_fixed_q
neon_cvt_fixed_q:
    ldr     r0, =0x3FC00000         @ 1.5
    ldr     r1, =0xC0100000         @ -2.25
    ldr     r2, =0x4F32D05E         @ 3e9 (saturates)
    ldr     r3, =0x3F400000         @ 0.75
    vmov    d0, r0, r1
    vmov    d1, r2, r3
    vcvt.s32.f32 q1, q0, #4
    vmov    r0, r1, d2
    vmov    r2, r3, d3
    bx      lr

@ NEON VCVT back (D form), VCGT.F32 (D form) and VABS.F32 (Q form).
@ TEST neon_cvt_cmp_abs
@ EXPECT r0=0x3FC00000 r1=0xC0100000 r2=0xFFFFFFFF r3=0x40100000
    .global neon_cvt_cmp_abs
neon_cvt_cmp_abs:
    ldr     r0, =0x00000018
    ldr     r1, =0xFFFFFFDC
    vmov    d2, r0, r1
    vcvt.f32.s32 d4, d2, #4         @ {1.5, -2.25}
    ldr     r2, =0xC0100000
    ldr     r3, =0x3FC00000
    vmov    d5, r2, r3              @ {-2.25, 1.5}
    vcgt.f32 d6, d4, d5             @ lane0: 1.5 > -2.25
    vabs.f32 q4, q2                 @ lane1: |-2.25|
    vmov    r0, r1, d4
    vmov    r2, s12
    vmov    r3, s17
    bx      lr
    .ltorg
