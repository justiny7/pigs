.include <vc4.qinc>

.set stride, ra0
.set NUM_INTERSECTIONS, ra1
.set qpu_num, ra2

.set src_id, ra3
.set dst_screen_x, ra4
.set dst_screen_y, ra5
.set dst_cov2d_inv_x, ra6
.set dst_cov2d_inv_y, ra7
.set dst_cov2d_inv_z, ra8
.set dst_color_r, ra9
.set dst_color_g, ra10
.set dst_color_b, ra11
.set dst_opacity, ra12

.set src_screen_x, rb0
.set src_screen_y, rb1
.set src_cov2d_inv_x, rb2
.set src_cov2d_inv_y, rb3
.set src_cov2d_inv_z, rb4
.set src_color_r, rb5
.set src_color_g, rb6
.set src_color_b, rb7
.set src_opacity, rb8


.set base_row, ra13          # (qpu_num * 4)
.set loop_counter, rb9


mov stride, unif
mov NUM_INTERSECTIONS, unif
mov qpu_num, unif

mov src_id, unif
mov dst_screen_x, unif
mov dst_screen_y, unif
mov dst_cov2d_inv_x, unif
mov dst_cov2d_inv_y, unif
mov dst_cov2d_inv_z, unif
mov dst_color_r, unif
mov dst_color_g, unif
mov dst_color_b, unif
mov dst_opacity, unif

mov src_screen_x, unif
mov src_screen_y, unif
mov src_cov2d_inv_x, unif
mov src_cov2d_inv_y, unif
mov src_cov2d_inv_z, unif
mov src_color_r, unif
mov src_color_g, unif
mov src_color_b, unif
mov src_opacity, unif


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


# loop counter (qpu_num * SIMD_WIDTH += stride until >= NUM_GAUSSIANS)
shl loop_counter, qpu_num, 4

# multiply qpu_num by 4 to get VPM base row
shl base_row, qpu_num, 2

:loop
    mem_to_vpm_vec16 src_id, 0
    vpm_to_reg_vec16 r3, 0

    # memory addresses
    shl r3, r3, 2
    add t0s, src_screen_x, r3
    add t0s, src_screen_y, r3
    add t0s, src_cov2d_inv_x, r3
    add t0s, src_cov2d_inv_y, r3
    add t1s, src_cov2d_inv_z, r3
    add t1s, src_color_r, r3
    add t1s, src_color_g, r3
    add t1s, src_color_b, r3

    ldtmu0
    reg_to_vpm_vec16 r4, 0
    ldtmu0
    reg_to_vpm_vec16 r4, 1
    ldtmu0
    reg_to_vpm_vec16 r4, 2
    ldtmu0
    reg_to_vpm_vec16 r4, 3

    add t0s, src_opacity, r3

    vpm_to_mem_vec16 dst_screen_x, 0
    vpm_to_mem_vec16 dst_screen_y, 1
    vpm_to_mem_vec16 dst_cov2d_inv_x, 2
    vpm_to_mem_vec16 dst_cov2d_inv_y, 3

    ldtmu1
    reg_to_vpm_vec16 r4, 0
    ldtmu1
    reg_to_vpm_vec16 r4, 1
    ldtmu1
    reg_to_vpm_vec16 r4, 2
    ldtmu1
    reg_to_vpm_vec16 r4, 3

    vpm_to_mem_vec16 dst_cov2d_inv_z, 0
    vpm_to_mem_vec16 dst_color_r, 1
    vpm_to_mem_vec16 dst_color_g, 2
    vpm_to_mem_vec16 dst_color_b, 3

    ldtmu0
    reg_to_vpm_vec16 r4, 0
    vpm_to_mem_vec16 dst_opacity, 0

    # increment pointers += stride * sizeof(float), check if reached array end
    shl r0, stride, 2
    add src_id, src_id, r0
    add dst_screen_x, dst_screen_x, r0
    add dst_screen_y, dst_screen_y, r0
    add dst_cov2d_inv_x, dst_cov2d_inv_x, r0
    add dst_cov2d_inv_y, dst_cov2d_inv_y, r0
    add dst_cov2d_inv_z, dst_cov2d_inv_z, r0
    add dst_color_r, dst_color_r, r0
    add dst_color_g, dst_color_g, r0
    add dst_color_b, dst_color_b, r0
    add dst_opacity, dst_opacity, r0

    add r0, loop_counter, stride
    sub.setf -, r0, NUM_INTERSECTIONS

    brr.anyc -, :loop
    mov loop_counter, r0
    nop
    nop

nop; thrend
nop
nop
