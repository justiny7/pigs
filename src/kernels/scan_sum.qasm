.include <vc4.qinc>

.set stride, ra0
.set NUM_INTERSECTIONS, ra1
.set qpu_num, ra2

.set arr, ra3
.set pref, ra4

.set base_row, ra5          # (qpu_num * 4)
.set loop_counter, rb9

mov stride, unif
mov NUM_INTERSECTIONS, unif
mov qpu_num, unif

mov arr, unif
mov pref, unif


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
    # read pref
    mem_to_vpm_vec16 pref, 1
    vpm_to_reg_vec16 r2, 1

    mov r0, elem_num

    # zero out lanes 1-15 of pref
    sub.setf -, r0, 0
    mov.ifnz r2, 0

    # read arr now bc can't rotate right after write
    mem_to_vpm_vec16 arr, 0

    # rotate by 1, 2, 4, 8 to broadcast lane 0
    mov r1, r2 >> 1
    or r2, r1, r2

    # interleaving...
    vpm_to_reg_vec16 r3, 0

    mov r1, r2 >> 2
    or r2, r1, r2
    nop
    mov r1, r2 >> 4
    or r2, r1, r2
    nop
    mov r1, r2 >> 8
    or r2, r1, r2

    add r3, r2, r3
    reg_to_vpm_vec16 r3, 0
    vpm_to_mem_vec16 arr, 0

    # increment pointers += stride * sizeof(float), check if reached array end
    shl r0, stride, 2
    add arr, arr, r0
    add pref, pref, r0

    add r0, loop_counter, stride
    sub.setf -, r0, NUM_INTERSECTIONS

    brr.anyc -, :loop
    mov loop_counter, r0
    nop
    nop

nop; thrend
nop
nop
