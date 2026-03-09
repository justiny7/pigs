.include <vc4.qinc>

.set stride, ra0
.set NUM_GAUSSIANS, ra1
.set NUM_INTERSECTIONS, ra2
.set qpu_num, ra3

.set max_tile_width, ra4
.set max_tile_height, ra5
.set TILE_SIZE, 16 # assume tile size is 16

.set screen_x, rb0
.set screen_y, rb1
.set radius, rb2
.set depth_precomped, rb3

.set lb, ra6
.set tb, ra7
.set rb, rb4
.set tiles_touched, rb8

.set base_row, ra9          # (qpu_num * 4)
.set loop_counter, rb5

.set key, ra11
.set id, ra12
.set pg_indices, ra13
.set pg_all_indices, ra14

.set temp_a0, ra20
.set temp_b0, rb20

# disable TMU swap
mov tmurs, 1

mov stride, unif
mov NUM_GAUSSIANS, unif
mov NUM_INTERSECTIONS, unif
mov qpu_num, unif

mov max_tile_width, unif
mov max_tile_height, unif

mov screen_x, unif
mov screen_y, unif
mov radius, unif
mov depth_precomped, unif

mov tiles_touched, unif

mov key, unif
mov id, unif

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

.macro calc_indices
    # parallel binsearch: from i = 20 ... 0, add 2^i to indices
    # -> check which ones go over, mask + subtract those

    add pg_all_indices, loop_counter, elem_num

    # pg indices (set before, add at end of loop)
    mov r2, (1 << 20)

    # current bit
    mov r1, (1 << 20)
    :1
        # min with NUM_GAUSSIANS so TMU doesn't error
        # this is okay bc tiles_touched is size NUM_GAUSSIANS + 1
        min r3, r2, NUM_GAUSSIANS

        # kick off TMU read
        shl r3, r3, 2
        add t1s, tiles_touched, r3
        ldtmu1

        # compare against indices
        max.setf -, r4, pg_all_indices
        sub.ifc r2, r2, r1

        # r1 >>= 1
        shr.setf r1, r1, 1

        brr.anynz -, :1
        add r2, r2, r1
        nop
        nop

    # multiply indices by sizeof(float) to get byte offset
    shl pg_indices, r2, 2

    reg_to_vpm_vec16 r2, 2
    vpm_to_mem_vec16 id, 2
.endm

.macro calc_bbox
    add t0s, radius, pg_indices
    add t0s, screen_y, pg_indices
    add t0s, screen_x, pg_indices
    add t0s, tiles_touched, pg_indices

    ldtmu0
    mov r1, r4

    ldtmu0
    mov r2, r4

    # calc tb
    fsub r0, r2, r1
    fmax r0, r0, 0
    ftoi r0, r0
    shr tb, r0, 4 # divide by TILE_SIZE

    ldtmu0
    mov r2, r4

    # calc rb
    fadd r0, r2, r1
    ftoi r0, r0
    add r0, r0, TILE_SIZE - 1
    shr r0, r0, 4
    min r3, r0, max_tile_width

    # we don't care about bottom bound

    # calc lb
    fsub r0, r2, r1
    fmax r0, r0, 0
    ftoi r0, r0
    shr r0, r0, 4

    # offset from beginning index (tile_idx = (k - t) * (r - l + 1) + (j - l))
    # x = offset / (r - l + 1) + t
    # y = offset % (r - l + 1) + l

    # store d = offset / (r - l + 1)
    # then x = d + t, y = (offset - d * (r - l + 1)) + l

    # to calculate d, cast to float, divide, then cast back to int??
    sub r1, r3, r0
    add r1, r1, 1

    ldtmu0
    # get offset
    sub r2, pg_all_indices, r4
    mov temp_a0, r2

    # move to recip, need 2 ops
    itof r2, r1
    mov sfu_recip, r2
    mov rb, r3
    mov lb, r0

    # convert offset to float, divide, convert back to int, r2 now stores d
    itof r2, temp_a0
    fadd r2, r2, 0.5 # handle rounding errors?
    fmul r2, r2, r4
    ftoi r2, r2

    # in the meantime, kick off TMU for depth
    add t0s, depth_precomped, pg_indices

    # tile x contribution: (d + t) * (WIDTH / TILE_SIZE)
    # max_tile_height stores (WIDTH / TILE_SIZE) - 1
    add r0, r2, tb
    add r3, max_tile_width, 1
    mul24 r0, r0, r3

    # tile y: offset - d * (r - l + 1) + l
    # (r - l + 1) is still in r1
    mul24 r1, r1, r2
    sub r1, temp_a0, r1
    add r1, r1, lb
    add r1, r1, r0

    # convert depth + tile to key
    ldtmu0
    not r2, r4
    shr r2, r2, 12
    shl r1, r1, 20
    or r1, r1, r2
    reg_to_vpm_vec16 r1, 0
    vpm_to_mem_vec16 key, 0
.endm

# loop counter (qpu_num * SIMD_WIDTH += stride until >= NUM_INTERSECTIONS)
shl loop_counter, qpu_num, 4

# multiply qpu_num by 4 to get VPM base row
shl base_row, qpu_num, 2


:loop
    calc_indices

    calc_bbox

    # increment pointers += stride * sizeof(float), check if reached array end
    shl r0, stride, 2
    # add tile, tile, r0
    add key, key, r0
    add id, id, r0

    add r0, loop_counter, stride
    sub.setf -, r0, NUM_INTERSECTIONS

    brr.anyc -, :loop
    mov loop_counter, r0
    nop
    nop

nop; thrend
nop
nop
