.include <vc4.qinc>

.set stride, ra0
.set NUM_GAUSSIANS, ra1
.set qpu_num, ra2

.set max_tile_width, ra7
.set max_tile_height, ra8
.set TILE_SIZE, 16 # assume tile size is 16

.set screen_x, rb0
.set screen_y, rb1
.set radius, rb2

.set lb, ra4
.set rb, rb4
.set tb, ra5
.set bb, rb3
.set tiles_touched, ra6

.set base_row, ra3          # (qpu_num * 4)
.set loop_counter, rb5

.set temp_a0, ra10
.set temp_b0, rb10


mov stride, unif
mov NUM_GAUSSIANS, unif
mov qpu_num, unif

mov max_tile_width, unif
mov max_tile_height, unif

mov screen_x, unif
mov screen_y, unif
mov radius, unif

mov lb, unif
mov rb, unif
mov tb, unif
mov bb, unif
mov tiles_touched, unif


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

.macro calc_bbox
    mem_to_vpm_vec16 radius, 0
    mem_to_vpm_vec16 screen_x, 1
    mem_to_vpm_vec16 screen_y, 2

    vpm_to_reg_vec16 r1, 0
    vpm_to_reg_vec16 r2, 1

    # mask off radius == 0
    fsub.setf -, r1, 0.0

    fsub r0, r2, r1
    fmax r0, r0, 0
    ftoi r0, r0
    shr r3, r0, 4 # divide by TILE_SIZE
    sub temp_a0, 1, r3
    reg_to_vpm_vec16 r3, 0

    fadd r0, r2, r1
    ftoi r0, r0
    mov r3, TILE_SIZE - 1
    add r0, r0, r3
    shr r0, r0, 4 # divide by TILE_SIZE
    min r3, r0, max_tile_width
    add temp_a0, temp_a0, r3
    reg_to_vpm_vec16 r3, 1

    vpm_to_reg_vec16 r2, 2

    fsub r0, r2, r1
    fmax r0, r0, 0
    ftoi r0, r0
    shr r3, r0, 4
    sub temp_b0, 1, r3
    reg_to_vpm_vec16 r3, 2

    fadd r0, r2, r1
    ftoi r0, r0
    mov r3, 15
    add r0, r0, r3
    shr r0, r0, 4
    min r3, r0, max_tile_height
    add temp_b0, temp_b0, r3
    reg_to_vpm_vec16 r3, 3

    vpm_to_mem_vec16 lb, 0
    vpm_to_mem_vec16 rb, 1
    vpm_to_mem_vec16 tb, 2
    vpm_to_mem_vec16 bb, 3

    mov r1, 0
    mul24.ifnz r1, temp_a0, temp_b0
    reg_to_vpm_vec16 r1, 0
    vpm_to_mem_vec16 tiles_touched, 0
.endm

# loop counter (qpu_num * SIMD_WIDTH += stride until >= NUM_GAUSSIANS)
shl loop_counter, qpu_num, 4

# multiply qpu_num by 4 to get VPM base row
shl base_row, qpu_num, 2

:loop
    calc_bbox

    # increment pointers += stride * sizeof(float), check if reached array end
    shl r0, stride, 2
    add screen_x, screen_x, r0
    add screen_y, screen_y, r0
    add radius, radius, r0
    add lb, lb, r0
    add rb, rb, r0
    add tb, tb, r0
    add bb, bb, r0
    add tiles_touched, tiles_touched, r0

    add r0, loop_counter, stride
    sub.setf -, r0, NUM_GAUSSIANS

    brr.anyc -, :loop
    mov loop_counter, r0
    nop
    nop

nop; thrend
nop
nop
