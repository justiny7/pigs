.include <vc4.qinc>

.set stride, ra0
.set NUM_GAUSSIANS, ra1
.set qpu_num, ra2

.set base_row, ra3          # (qpu_num * 4)
.set loop_counter, ra5

.set pos_x, rb0
.set pos_y, rb1
.set pos_z, rb2

.set pos_x_init, ra29
.set pos_y_init, ra30
.set pos_z_init, ra31

.set cam_x, ra7
.set cam_y, ra8
.set cam_z, ra9

.set color, ra6
.set sh0, rb3
.set sh1, rb4
.set sh2, rb5
.set sh3, ra10
.set sh4, ra11
.set sh5, ra12
.set sh6, ra13
.set sh7, ra14
.set sh8, ra15
.set sh9, ra16
.set sh10, ra17
.set sh11, ra18
.set sh12, ra19
.set sh13, ra20
.set sh14, ra21
.set sh15, ra22

.set x, rb6
.set y, rb7
.set z, rb8
.set xx, rb9
.set yy, ra23
.set zz, ra24
.set xy, ra25
.set xz, ra26
.set yz, ra27

.set temp_b0, rb10
.set temp_b1, rb11
.set temp_b2, rb12
.set temp_b3, rb13

.set SH_C0, 0.28209479177387814
.set SH_C1, 0.4886025119029199

.set SH_C2_0, 1.0925484305920792
.set SH_C2_1, -1.0925484305920792
.set SH_C2_2, 0.31539156525252005
.set SH_C2_3, -1.0925484305920792
.set SH_C2_4, 0.5462742152960396

.set SH_C3_0, -0.5900435899266435
.set SH_C3_1, 2.890611442640554
.set SH_C3_2, -0.4570457994644658
.set SH_C3_3, 0.3731763325901154
.set SH_C3_4, -0.4570457994644658
.set SH_C3_5, 1.445305721320277
.set SH_C3_6, -0.5900435899266435


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

.macro view_setup
    mem_to_vpm_vec16 pos_x, 0
    mem_to_vpm_vec16 pos_y, 1
    mem_to_vpm_vec16 pos_z, 2

    vpm_to_reg_vec16 x, 0
    vpm_to_reg_vec16 y, 1
    vpm_to_reg_vec16 z, 2

    fsub r0, x, cam_x
    mov x, r0
    fmul r1, r0, r0

    fsub r0, y, cam_y
    fmul r2, r0, r0
    fadd r1, r1, r2

    fsub r3, z, cam_z
    fmul r2, r3, r3
    fadd r1, r1, r2

    mov sfu_recipsqrt, r1
    nop
    nop

    fmul x, x, r4
    fmul y, r0, r4
    fmul z, r3, r4

    # now x, y, z store dir.x, dir.y, dir.z
    mov r0, x
    mov r1, y
    fmul xx, r0, r0
    fmul yy, r1, r1
    fmul zz, z, z
    fmul xy, r0, r1
    fmul xz, r0, z
    fmul yz, r1, z
.endm

.macro calc_sh
    mem_to_vpm_vec16 sh0, 0
    mem_to_vpm_vec16 sh1, 1
    mem_to_vpm_vec16 sh2, 2
    mem_to_vpm_vec16 sh3, 3

    vpm_to_reg_vec16 temp_b0, 0
    vpm_to_reg_vec16 temp_b1, 1
    vpm_to_reg_vec16 temp_b2, 2
    vpm_to_reg_vec16 temp_b3, 3

    # degree 0
    mov r0, SH_C0
    fmul r2, temp_b0, r0

    # degree 1
    mov r0, SH_C1
    fmul r1, r0, y
    fmul r1, r1, temp_b1
    fsub r2, r2, r1

    fmul r1, r0, z
    fmul r1, r1, temp_b2
    fadd r2, r2, r1

    fmul r1, r0, x
    fmul r1, r1, temp_b3
    fsub r2, r2, r1

    mem_to_vpm_vec16 sh4, 0
    mem_to_vpm_vec16 sh5, 1
    mem_to_vpm_vec16 sh6, 2
    mem_to_vpm_vec16 sh7, 3

    vpm_to_reg_vec16 temp_b0, 0
    vpm_to_reg_vec16 temp_b1, 1
    vpm_to_reg_vec16 temp_b2, 2
    vpm_to_reg_vec16 temp_b3, 3

    # degree 2
    mov r0, SH_C2_0
    fmul r1, r0, xy
    fmul r1, r1, temp_b0
    fadd r2, r2, r1

    mov r0, SH_C2_1
    fmul r1, r0, yz
    fmul r1, r1, temp_b1
    fadd r2, r2, r1

    mov r0, SH_C2_2
    fmul r1, 2.0, zz
    fsub r1, r1, xx
    fsub r1, r1, yy
    fmul r1, r1, r0
    fmul r1, r1, temp_b2
    fadd r2, r2, r1

    mov r0, SH_C2_3
    fmul r1, r0, xz
    fmul r1, r1, temp_b3
    fadd r2, r2, r1

    mem_to_vpm_vec16 sh8, 0
    mem_to_vpm_vec16 sh9, 1
    mem_to_vpm_vec16 sh10, 2
    mem_to_vpm_vec16 sh11, 3

    vpm_to_reg_vec16 temp_b0, 0
    vpm_to_reg_vec16 temp_b1, 1
    vpm_to_reg_vec16 temp_b2, 2
    vpm_to_reg_vec16 temp_b3, 3

    mov r0, SH_C2_4
    fsub r1, xx, yy
    fmul r1, r1, r0
    fmul r1, r1, temp_b0
    fadd r2, r2, r1

    # degree 3
    mov r0, SH_C3_0
    mov r3, 3.0
    fmul r1, r3, xx
    fsub r1, r1, yy
    fmul r1, r1, y
    fmul r1, r1, r0
    fmul r1, r1, temp_b1
    fadd r2, r2, r1

    mov r0, SH_C3_1
    fmul r1, r0, xy
    fmul r1, r1, z
    fmul r1, r1, temp_b2
    fadd r2, r2, r1

    mov r0, SH_C3_2
    fmul r1, 4.0, zz
    fsub r1, r1, xx
    fsub r1, r1, yy
    fmul r1, r1, y
    fmul r1, r1, r0
    fmul r1, r1, temp_b3
    fadd r2, r2, r1

    mem_to_vpm_vec16 sh12, 0
    mem_to_vpm_vec16 sh13, 1
    mem_to_vpm_vec16 sh14, 2
    mem_to_vpm_vec16 sh15, 3

    vpm_to_reg_vec16 temp_b0, 0
    vpm_to_reg_vec16 temp_b1, 1
    vpm_to_reg_vec16 temp_b2, 2
    vpm_to_reg_vec16 temp_b3, 3

    mov r0, 3.0
    fmul r1, 2.0, zz
    fadd r3, xx, yy
    fmul r3, r3, r0
    fsub r1, r1, r3
    mov r0, SH_C3_3
    fmul r1, r1, z
    fmul r1, r1, r0
    fmul r1, r1, temp_b0
    fadd r2, r2, r1

    mov r0, SH_C3_4
    fmul r1, 4.0, zz
    fsub r1, r1, xx
    fsub r1, r1, yy
    fmul r1, r1, x
    fmul r1, r1, r0
    fmul r1, r1, temp_b1
    fadd r2, r2, r1

    mov r0, SH_C3_5
    fsub r1, xx, yy
    fmul r1, r1, z
    fmul r1, r1, r0
    fmul r1, r1, temp_b2
    fadd r2, r2, r1

    mov r0, SH_C3_6
    mov r3, 3.0
    fmul r3, r3, yy
    fsub r1, xx, r3
    fmul r1, r1, x
    fmul r1, r1, r0
    fmul r1, r1, temp_b3
    fadd r2, r2, r1

    fadd r2, r2, 0.5
    fmax r2, r2, 0.0

    reg_to_vpm_vec16 r2, 0
    vpm_to_mem_vec16 color, 0
.endm


mov stride, unif
mov NUM_GAUSSIANS, unif
mov qpu_num, unif

mov pos_x_init, unif
mov pos_y_init, unif
mov pos_z_init, unif

mov cam_x, unif
mov cam_y, unif
mov cam_z, unif

# multiply qpu_num by 4 to get VPM base row
shl base_row, qpu_num, 2

# repeat for R, G, B
# (recalc view_setup for every color every time bc not enough space to store all SH unifs at once)
.rep _, 3
    # mov unifs for this color
    mov color, unif
    mov sh0, unif
    mov sh1, unif
    mov sh2, unif
    mov sh3, unif
    mov sh4, unif
    mov sh5, unif
    mov sh6, unif
    mov sh7, unif
    mov sh8, unif
    mov sh9, unif
    mov sh10, unif
    mov sh11, unif
    mov sh12, unif
    mov sh13, unif
    mov sh14, unif
    mov sh15, unif

    mov pos_x, pos_x_init
    mov pos_y, pos_y_init
    mov pos_z, pos_z_init

    # loop counter (qpu_num * SIMD_WIDTH += stride until >= NUM_GAUSSIANS)
    shl loop_counter, qpu_num, 4

    :1
        view_setup

        calc_sh
        # mem_to_vpm_vec16 sh15, 0
        # vpm_to_mem_vec16 color, 0

        # increment pointers += stride * sizeof(float), check if reached array end
        shl r0, stride, 2
        add pos_x, pos_x, r0
        add pos_y, pos_y, r0
        add pos_z, pos_z, r0
        add color, color, r0
        add sh0, sh0, r0
        add sh1, sh1, r0
        add sh2, sh2, r0
        add sh3, sh3, r0
        add sh4, sh4, r0
        add sh5, sh5, r0
        add sh6, sh6, r0
        add sh7, sh7, r0
        add sh8, sh8, r0
        add sh9, sh9, r0
        add sh10, sh10, r0
        add sh11, sh11, r0
        add sh12, sh12, r0
        add sh13, sh13, r0
        add sh14, sh14, r0
        add sh15, sh15, r0

        mov r1, stride
        add r0, loop_counter, r1
        sub.setf -, r0, NUM_GAUSSIANS

        brr.anyc -, :1
        mov loop_counter, r0
        nop
        nop
.endr

nop; thrend
nop
nop

