#ifndef QPU_H
#define QPU_H

#include "mailbox.h"
#include <stdint.h>

// V3D spec: http://www.broadcom.com/docs/support/videocore/VideoCoreIV-AG100-R.pdf
#define V3D_BASE 0x20C00000

#define PAGE_SIZE 4096
#define TO_CPU(ptr) ((uint32_t) (ptr) & 0x3FFFFFFF);
#define TO_BUS(ptr) ((uint32_t) (ptr) | GPU_L2_OFFSET)

enum {
    V3D_L2CACTL = V3D_BASE + 0x0020,
    V3D_SLCACTL = V3D_BASE + 0x0024,
    V3D_SRQPC = V3D_BASE + 0x0430,
    V3D_SRQUA = V3D_BASE + 0x0434,
    V3D_SRQCS = V3D_BASE + 0x043C,
    V3D_DBCFG = V3D_BASE + 0x0E00,
    V3D_DBQITE = V3D_BASE + 0x0E2C,
    V3D_DBQITC = V3D_BASE + 0x0E30,
};

uint32_t qpu_init(uint32_t num_bytes);
void qpu_free(uint32_t handle);
void qpu_wait(uint32_t num_qpus);
void qpu_execute(uint32_t num_qpus, uint32_t* mail);

#endif
