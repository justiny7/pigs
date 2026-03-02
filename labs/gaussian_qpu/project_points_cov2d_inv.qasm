.include <vc4.qinc>

.set stride, ra0
.set NUM_GAUSSIANS, ra1
.set qpu_num, ra2

.set pos_x, rb0
.set pos_y, rb1
.set pos_z, rb2

.set depth, rb3
.set screen_x, rb4
.set screen_y, ra5
.set radius, ra4

.set base_row, ra3          # (qpu_num * 4)
.set loop_counter, rb5

.set tx_reg, ra6
.set ty_reg, ra7
.set tz_reg, ra8

.set w2c0, rb6
.set w2c1, rb7
.set w2c2, rb8
.set w2c3, rb9
.set w2c4, rb10
.set w2c5, rb11
.set w2c6, rb12
.set w2c7, rb13
.set w2c8, rb14
.set w2c9, rb15
.set w2c10, rb16
.set w2c11, rb17

.set fx, ra9
.set cx, ra10
.set fy, ra11
.set cy, ra12

# covariance matrix:
# a b c
# b d e
# c e f
.set cov3da, rb18
.set cov3db, rb19
.set cov3dc, rb20
.set cov3dd, rb21
.set cov3de, rb22
.set cov3df, rb23

# Jacobian matrix:
# a 0 b
# 0 c d
# 0 0 0
.set Ja, ra13
.set Jb, ra14
.set Jc, ra15
.set Jd, ra16

# J @ W matrix:
# J00 J01 J02
# J10 J11 J12
#  0   0   0
.set J00, ra17
.set J01, ra18
.set J02, ra19
.set J10, ra20
.set J11, ra21
.set J12, ra22

.set cov2d_inv_x, ra23
.set cov2d_inv_y, ra24
.set cov2d_inv_z, ra25

# temporary scratchpad memory registers
.set temp_b0, rb24
.set temp_b1, rb25
.set temp_b2, rb26
.set temp_b3, rb27
.set temp_b4, rb28
.set temp_b5, rb29

.set temp_a0, ra26
.set temp_a1, ra27
.set temp_a2, ra28
.set temp_a3, ra29
.set temp_a4, ra30
.set temp_a5, ra31

mov stride, unif
mov NUM_GAUSSIANS, unif
mov qpu_num, unif

mov pos_x, unif    # pos_x
mov pos_y, unif    # pos_y
mov pos_z, unif    # pos_z

mov depth, unif    # depth
mov screen_x, unif    # screen_x
mov screen_y, unif    # screen_y
mov radius, unif   # radius

mov w2c0, unif
mov w2c1, unif
mov w2c2, unif
mov w2c3, unif
mov w2c4, unif
mov w2c5, unif
mov w2c6, unif
mov w2c7, unif
mov w2c8, unif
mov w2c9, unif
mov w2c10, unif
mov w2c11, unif

mov fx, unif
mov cx, unif
mov fy, unif
mov cy, unif

mov cov3da, unif
mov cov3db, unif
mov cov3dc, unif
mov cov3dd, unif
mov cov3de, unif
mov cov3df, unif

mov cov2d_inv_x, unif
mov cov2d_inv_y, unif
mov cov2d_inv_z, unif

.macro mem_to_vpm_vec16, addr_reg, offset
    # read 1 row of length 16 at VPM row [offset]
    mov r0, vdr_setup_0(1, 16, 1, vdr_h32(1, offset, 0))

    # shift by 4 to switch to [base_row]'th row, add to vr_setup
    shl r1, base_row, 4
    add vr_setup, r0, r1

    # read [addr_reg] into ([base_row] + [offset])'th row of VPM
    mov vr_addr, addr_reg
    mov -, vr_wait
.endm

.macro vpm_to_mem_vec16, addr_reg, offset
    # write 1 row of length 16 at VPM row [offset]
    mov r0, vdw_setup_0(1, 16, dma_h32(offset, 0))

    # shift by 7 to switch to [base_row]'th row, add to vw_setup
    shl r1, base_row, 7
    add vw_setup, r0, r1

    # write ([base_row + [offset])'th row of VPM into [addr_reg]
    mov vw_addr, addr_reg
    mov -, vw_wait
.endm

.macro reg_to_vpm_vec16, src_reg, offset
    # set up vpm at row [offset]
    mov r0, vpm_setup(1, 1, h32(offset))

    # add [base_row] to [offset] and kick off VPM write
    add vw_setup, r0, base_row
    mov vpm, src_reg
.endm

.macro vpm_to_reg_vec16, dst_reg, offset
    # set up vpm at row [offset]
    mov r0, vpm_setup(1, 1, h32(offset))

    # add [base_row] to [offset] and kick off VPM read
    add vr_setup, r0, base_row
    mov dst_reg, vpm
.endm

.macro accum_txyz, offset, w2c_reg1, w2c_reg2, w2c_reg3
    # load p.x/y/z into r0
    vpm_to_reg_vec16 r0, offset

    mov r1, w2c_reg1
    fmul r1, r1, r0
    fadd tx_reg, tx_reg, r1

    mov r1, w2c_reg2
    fmul r1, r1, r0
    fadd ty_reg, ty_reg, r1

    mov r1, w2c_reg3
    fmul r1, r1, r0
    fadd tz_reg, tz_reg, r1
.endm

.macro calc_uv_and_J, offset0, offset1, offset2
    # move depth to VPM (can overrwrite pos)
    mov r2, tz_reg
    reg_to_vpm_vec16 r2, offset0

    # set clipping plane flags for radius
    mov r0, 0.1     # near plane
    fmax r1, r0, r2
    mov r0, 100.0   # far plane
    fmin r1, r0, r1

    # if r2 was between 0.1 and 100, then r1 == r2
    fsub.setf -, r1, r2

    # get r2 reciprocal
    mov sfu_recip, r2

    # calculate u: load cx first (use r3 bc r0 is used in VPM move)
    mov r3, cx   # c->cx
    mov r1, fx   # c->fx
    fmul r1, r1, tx_reg
    fmul r1, r1, r4
    fadd r3, r3, r1
    reg_to_vpm_vec16 r3, offset1

    # calculate v: load cy first (use r3 bc r0 is used in VPM move)
    mov r3, cy   # c->cy
    mov r1, fy   # c->fy
    fmul r1, r1, ty_reg
    fmul r1, r1, r4
    fadd r3, r3, r1
    reg_to_vpm_vec16 r3, offset2

    # calculate Jacobian
    mov r0, r4           # move 1/tz to r0
    mov r3, -1.0

    fmul r2, r2, r2      # tz^2
    mov sfu_recip, r2    # get 1/tz^2

    fmul Ja, fx, r0      # Ja = fx / tz
    fmul Jc, fy, r0      # Jc = fy / tz

    fmul r1, fx, r3
    fmul r1, r1, tx_reg
    fmul Jb, r1, r4      # Jb = -fx * tx / tz^2

    fmul r1, fy, r3
    fmul r1, r1, ty_reg
    fmul Jd, r1, r4      # Jd = -fy * ty / tz^2
.endm

.macro dot2, dst_reg, a0, a1, b0, b1
    fmul r0, a0, b0
    fmul r1, a1, b1
    fadd dst_reg, r0, r1
.endm

.macro dot3, dst_reg, a0, a1, a2, b0, b1, b2
    fmul dst_reg, a0, b0
    fmul r0, a1, b1
    fmul r1, a2, b2
    fadd r0, r0, r1
    fadd dst_reg, dst_reg, r0
.endm

.macro calc_cov2d_inv, offset0, offset1, offset2
    # start moving cov3d to memory
    # a, b, c, d
    mem_to_vpm_vec16 cov3da, 0
    mem_to_vpm_vec16 cov3db, 1
    mem_to_vpm_vec16 cov3dc, 2
    mem_to_vpm_vec16 cov3dd, 3
    vpm_to_reg_vec16 temp_b0, 0
    vpm_to_reg_vec16 temp_b1, 1
    vpm_to_reg_vec16 temp_b2, 2
    vpm_to_reg_vec16 temp_b3, 3

    # calculate J00 - J12
    dot2 J00, Ja, Jb, w2c0, w2c8
    dot2 J01, Ja, Jb, w2c1, w2c9
    dot2 J02, Ja, Jb, w2c2, w2c10
    dot2 J10, Jc, Jd, w2c4, w2c8
    dot2 J11, Jc, Jd, w2c5, w2c9
    dot2 J12, Jc, Jd, w2c6, w2c10

    # reg_to_vpm_vec16 J10, 0
    # reg_to_vpm_vec16 J11, 1
    # reg_to_vpm_vec16 J12, 2

    dot3 temp_a0, J00, J01, J02, temp_b0, temp_b1, temp_b2
    dot3 temp_a3, J10, J11, J12, temp_b0, temp_b1, temp_b2

    # e, b, c, d
    mem_to_vpm_vec16 cov3de, 0
    vpm_to_reg_vec16 temp_b0, 0
    dot3 temp_a1, J00, J01, J02, temp_b1, temp_b3, temp_b0
    dot3 temp_a4, J10, J11, J12, temp_b1, temp_b3, temp_b0

    # e, f, c, d
    mem_to_vpm_vec16 cov3df, 1
    vpm_to_reg_vec16 temp_b1, 1
    dot3 temp_a2, J00, J01, J02, temp_b2, temp_b0, temp_b1
    dot3 temp_a5, J10, J11, J12, temp_b2, temp_b0, temp_b1

    mov temp_b0, temp_a0
    mov temp_b1, temp_a1
    mov temp_b2, temp_a2
    mov temp_b3, temp_a3
    mov temp_b4, temp_a4
    mov temp_b5, temp_a5

    # move cov2d.x/y/z to temp_a0/1/2
    # use r2, r3 bc dot3 doesn't use it
    dot3 r2, temp_b0, temp_b1, temp_b2, J00, J01, J02
    mov r0, 0.3
    fadd temp_a0, r2, r0

    dot3 r3, temp_b0, temp_b1, temp_b2, J10, J11, J12
    mov temp_a1, r3

    dot3 r2, temp_b3, temp_b4, temp_b5, J10, J11, J12
    mov r0, 0.3
    fadd r2, r2, r0

    # calc 0.5 * (cov2d.x + cov2d.y) for radius
    fadd r0, r2, temp_a0
    fmul temp_a3, r0, 0.5

    # get determinant
    fmul r0, r2, temp_a0
    fmul r1, r3, r3
    fsub r0, r0, r1

    # max with 1e-6 for stability
    mov r1, 0.000001
    fmax r0, r0, r1

    mov sfu_recip, r0

    # need at least 2 ops after SFU write before reading r4
    mov temp_a2, r2

    # while det is still in r0, calc max(0.1f, mid * mid - det)
    fmul r2, temp_a3, temp_a3
    fsub r2, r2, r0
    mov r0, 0.1
    fmax r2, r2, r0

    # r4 safe to read now... (don't touch r3)
    mov r3, r4

    # move r2 (max(0.1f, mid * mid - det)) to SFU to get recipsqrt
    mov sfu_recipsqrt, r2

    # need at least 2 more ops

    # cov2d_inv.x
    fmul r1, temp_a2, r3
    reg_to_vpm_vec16 r1, 0

    # move r4 to SFU recip to get actual sqrt
    mov sfu_recip, r4

    # two more ops...

    # cov2d_inv.y
    fmul r1, temp_a1, r3
    mov r2, -1.0
    fmul r1, r1, r2

    # breaking this up to calculate sqrt(max(lambda1, lambda2)) - don't touch r1, r3
    # now we have sqrt(max(0.1, mid * mid - det)) in r4...
    fadd r0, temp_a3, r4
    fsub r2, temp_a3, r4
    fmax r0, r0, r2
    mov sfu_recipsqrt, r0

    reg_to_vpm_vec16 r1, 1

    # move r4 to SFU recip to get actual sqrt
    mov sfu_recip, r4

    # cov2d_inv.z
    fmul r1, temp_a0, r3
    reg_to_vpm_vec16 r1, 2

    # now we can finally get radius (use clipping planes)
    mov r1, 3.5
    mov r2, 0.0
    fmul.ifz r2, r1, r4
    reg_to_vpm_vec16 r2, 3
.endm

# loop counter (qpu_num * SIMD_WIDTH += stride until >= NUM_GAUSSIANS)
shl loop_counter, qpu_num, 4

# multiply qpu_num by 4 to get VPM base row
shl base_row, qpu_num, 2

:loop
    # load pos_x, pos_y, pos_z into VPM rows qpu_num * 3 + 0/1/2
    mem_to_vpm_vec16 pos_x, 0
    mem_to_vpm_vec16 pos_y, 1
    mem_to_vpm_vec16 pos_z, 2

    # initialize with w2c[3 / 7 / 11]
    mov tx_reg, w2c3
    mov ty_reg, w2c7
    mov tz_reg, w2c11
    accum_txyz 0, w2c0, w2c4, w2c8
    accum_txyz 1, w2c1, w2c5, w2c9
    accum_txyz 2, w2c2, w2c6, w2c10
    nop
    nop

    # calculate screen_x, screen_y + move to VPM
    calc_uv_and_J 0, 1, 2

    # write from VPM to mem for depth, screen_x, screen_y
    vpm_to_mem_vec16 depth, 0
    vpm_to_mem_vec16 screen_x, 1
    vpm_to_mem_vec16 screen_y, 2

    # calculate cov2d inverse
    calc_cov2d_inv 0, 1, 2

    vpm_to_mem_vec16 cov2d_inv_x, 0
    vpm_to_mem_vec16 cov2d_inv_y, 1
    vpm_to_mem_vec16 cov2d_inv_z, 2
    vpm_to_mem_vec16 radius, 3

    # increment pointers += stride * sizeof(float), check if reached array end
    shl r0, stride, 2
    add pos_x, pos_x, r0
    add pos_y, pos_y, r0
    add pos_z, pos_z, r0
    add depth, depth, r0
    add screen_x, screen_x, r0
    add screen_y, screen_y, r0
    add radius, radius, r0
    add cov3da, cov3da, r0
    add cov3db, cov3db, r0
    add cov3dc, cov3dc, r0
    add cov3dd, cov3dd, r0
    add cov3de, cov3de, r0
    add cov3df, cov3df, r0
    add cov2d_inv_x, cov2d_inv_x, r0
    add cov2d_inv_y, cov2d_inv_y, r0
    add cov2d_inv_z, cov2d_inv_z, r0

    add r0, loop_counter, stride
    sub.setf -, r0, NUM_GAUSSIANS

    brr.anyc -, :loop
    mov loop_counter, r0
    nop
    nop

nop; thrend
nop
nop
