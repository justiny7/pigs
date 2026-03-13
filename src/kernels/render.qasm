.include <vc4.qinc>

.set stride, ra0    # tile stride = NUM_QPUS
.set NUM_TILES, rb0
.set qpu_num, ra1

.set max_tile_width, ra2
.set max_tile_width_recip, rb1
.set TILE_SIZE, 16 # assume tile size is 16

.set tile_x, ra3
.set tile_y_16, ra4

.set x, rb2
.set y, ra5

.set cov2d_inv_x, rb3
.set cov2d_inv_y, rb4
.set cov2d_inv_z, rb5
.set opacity, rb6
.set screen_x, rb7
.set screen_y, rb8
.set color_r, rb9
.set color_g, rb10
.set color_b, rb11

.set gaussians_touched, ra6

.set pixels, ra7

.set base_row, ra8          # (qpu_num * 4)
.set tile_idx, rb12
.set gaussian_idx, rb13
.set gaussian_idx_end, ra9

.set cur_cov2d_inv_x, rb14
.set cur_cov2d_inv_y, rb15
.set cur_cov2d_inv_z, rb16
.set cur_opacity, rb17
.set cur_screen_x, rb18
.set cur_screen_y, rb19
.set cur_color_r, rb20
.set cur_color_g, rb21
.set cur_color_b, rb22

.set px, ra10
.set py, ra11

.set out_r, ra12
.set out_g, ra13
.set out_b, ra14
.set out_addr, ra15

.set pixel_buffer, ra16
# ra16 - ra31 are reserved

.set neg_half_M_LOG2E, rb24

mov stride, unif
mov NUM_TILES, unif
mov qpu_num, unif
mov max_tile_width, unif
mov max_tile_width_recip, unif

mov cov2d_inv_x, unif
mov cov2d_inv_y, unif
mov cov2d_inv_z, unif
mov opacity, unif
mov screen_x, unif
mov screen_y, unif
mov color_r, unif
mov color_g, unif
mov color_b, unif

mov gaussians_touched, unif
mov pixels, unif

# sfu exp is 2^exp
mov neg_half_M_LOG2E, -0.5 * M_LOG2E

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

.macro calc_tile_xy
    itof r0, tile_idx
    fadd r0, r0, 0.5   # stability
    fmul r0, r0, max_tile_width_recip
    ftoi r0, r0
    shl tile_y_16, r0, 4

    mul24 r0, r0, max_tile_width
    sub tile_x, tile_idx, r0
.endm

.macro render_tile
    # kick off TMU read for indices @ tile_idx, tile_idx + 1
    mov r0, 2
    shl r0, tile_idx, r0
    add t1s, gaussians_touched, r0
    add r0, r0, 4
    add t1s, gaussians_touched, r0

    shl r0, tile_x, 4
    add x, r0, elem_num

    # reset output of this tile
    .rep row, 16
        mov pixel_buffer+row, 0
    .endr

    ldtmu1
    mov gaussian_idx, r4

    ldtmu1
    mov gaussian_idx_end, r4
    nop

    # loop until gaussian_idx == gaussian_idx_end
    :1
        sub.setf -, gaussian_idx, gaussian_idx_end
        brr.anyz -, :2
        mov r0, 2
        shl r0, gaussian_idx, r0
        mov y, tile_y_16  # reset y to y * TILE_SIZE

        add t0s, cov2d_inv_x, r0
        add t0s, cov2d_inv_y, r0
        add t0s, cov2d_inv_z, r0
        add t0s, opacity, r0
        add t1s, screen_x, r0
        add t1s, screen_y, r0
        add t1s, color_r, r0

        itof px, x; ldtmu0
        mov cur_cov2d_inv_x, r4
        itof py, y; ldtmu0
        mov cur_cov2d_inv_y, r4

        add t0s, color_g, r0
        add t0s, color_b, r0

        ldtmu0
        mov cur_cov2d_inv_z, r4
        ldtmu0
        mov cur_opacity, r4

        fadd px, px, 0.5
        fadd py, py, 0.5

        ldtmu1
        mov cur_screen_x, r4
        ldtmu1
        mov cur_screen_y, r4

        ldtmu1
        mov cur_color_r, r4
        ldtmu0
        mov cur_color_g, r4
        ldtmu0
        mov cur_color_b, r4

        .rep row, 16
            # r0 holds dx, r1 holds dy
            fsub r0, px, cur_screen_x
            fsub r1, py, cur_screen_y; fmul r2, r0, r0

            fmul r2, cur_cov2d_inv_x, r2
            
            fmul r3, r0, 2.0
            fmul r3, r3, cur_cov2d_inv_y
            fmul r3, r3, r1; fadd py, py, 1.0  # add py here to join instructions
            fadd r2, r2, r3; fmul r0, cur_cov2d_inv_z, r1

            fmul r0, r0, r1
            fadd r2, r2, r0; mov r3, neg_half_M_LOG2E

            fmul r2, r2, r3
            mov sfu_exp, r2

            # unpack 8[a/b/c]f takes bytes 0/1/2 in range [0, 255] -> [0.0, 1.0]
            mov out_r, (pixel_buffer+row).unpack8cf
            mov out_g, (pixel_buffer+row).unpack8bf
            mov out_b, (pixel_buffer+row).unpack8af

            # r0 stores alpha
            fmul r0, cur_opacity, r4
            fsub r3, 1.0, r0
            
            # calc out_r
            fmul r1, cur_color_r, r0
            fmul r2, r3, out_r
            fadd r1, r1, r2; fmul r2, cur_color_g, r0
            mov.pack8csf pixel_buffer+row, r1  # converts [0.0, 1.0] to [0, 255] in byte 2

            # calc out_g
            fmul r1, r3, out_g
            fadd r1, r1, r2; fmul r2, cur_color_b, r0
            mov.pack8bsf pixel_buffer+row, r1  # same op as above in byte 1

            # calc out_b
            fmul r1, r3, out_b
            fadd r1, r1, r2
            mov.pack8asf pixel_buffer+row, r1  # same op as above in byte 0
        .endr

        brr -, :1
        mov r0, 1
        add gaussian_idx, gaussian_idx, r0
        nop
:2
    
    # y is already reset from beginning of :1 (bc next 3 instructions are run)
    # write tile to memory
    shl r2, max_tile_width, 4
    .rep row, 16
        # get width by max_tile_width * TILE_SIZE
        # now r3 stores pixel indices
        mul24 r3, r2, y
        add r3, r3, x

        # get actual out addresses
        shl r3, r3, 2
        add r3, pixels, r3
        add y, y, 1

        reg_to_vpm_vec16 pixel_buffer+row, 0
        vpm_to_mem_vec16 r3, 0
    .endr
.endm

# loop counter (qpu_num += stride until >= NUM_TILES)
mov tile_idx, qpu_num

# multiply qpu_num by 4 to get VPM base row
shl base_row, qpu_num, 2

:loop
    calc_tile_xy

    render_tile

    add r0, tile_idx, stride
    sub.setf -, r0, NUM_TILES

    brr.anyc -, :loop
    mov tile_idx, r0
    nop
    nop

nop; thrend
nop
nop

