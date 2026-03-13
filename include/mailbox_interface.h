#ifndef MAILBOX_INTERFACE_H
#define MAILBOX_INTERFACE_H

#include "mailbox.h"

#include <stdbool.h>
#include <stdarg.h>

#define MBOX_BUFFER_SIZE 64

enum {
    MBOX_REQUEST = 0,
    MBOX_REQUEST_SUCCESS = 0x80000000,
    MBOX_REQUEST_FAIL,
};

typedef enum {
    /* Videocore info */
    MBOX_TAG_GET_FIRMWARE_REVISION      = 0x00000001,

    /* Hardware info */
    MBOX_TAG_GET_BOARD_MODEL            = 0x00010001,
    MBOX_TAG_GET_BOARD_REVISION         = 0x00010002,
    MBOX_TAG_GET_BOARD_MAC_ADDR         = 0x00010003,
    MBOX_TAG_GET_BOARD_SERIAL           = 0x00010004,
    MBOX_TAG_GET_ARM_MEMORY             = 0x00010005,
    MBOX_TAG_GET_VC_MEMORY              = 0x00010006,
    MBOX_TAG_GET_CLOCKS                 = 0x00010007,

    /* Config */
    MBOX_TAG_GET_COMMAND_LINE           = 0x00050001,

    /* Shared resource */
    MXBO_TAG_GET_DMA_CHANNELS           = 0x00060001,

    /* Power */
    MBOX_TAG_GET_POWER_STATE            = 0x00020001,
    MBOX_TAG_GET_TIMING                 = 0x00020002,
    MBOX_TAG_SET_POWER_STATE            = 0x00028001,

    /* Clocks */
    MBOX_TAG_GET_CLOCK_STATE            = 0x00030001,
    MBOX_TAG_SET_CLOCK_STATE            = 0x00038001,
    MBOX_TAG_GET_CLOCK_RATE             = 0x00030002,
    MBOX_TAG_GET_CLOCK_RATE_MEASURED    = 0x00030047,
    MBOX_TAG_SET_CLOCK_RATE             = 0x00038002,
    MBOX_TAG_GET_MAX_CLOCK_RATE         = 0x00030004,
    MBOX_TAG_GET_MIN_CLOCK_RATE         = 0x00030007,
    MBOX_TAG_GET_TURBO                  = 0x00030009,
    MBOX_TAG_SET_TURBO                  = 0x00038009,

    /* Onboard LED */
    MBOX_TAG_GET_ONBOARD_LED_STATUS     = 0x00030041,
    MBOX_TAG_TEST_ONBOARD_LED_STATUS    = 0x00034041,
    MBOX_TAG_SET_ONBOARD_LED_STATUS     = 0x00038041,

    /* Voltage */
    MBOX_TAG_GET_VOLTAGE                = 0x00030003,
    MBOX_TAG_SET_VOLTAGE                = 0x00038003,
    MBOX_TAG_GET_MAX_VOLTAGE            = 0x00030005,
    MBOX_TAG_GET_MIN_VOLTAGE            = 0x00030008,

    /* Temperature */
    MBOX_TAG_GET_TEMP                   = 0x00030006,
    MBOX_TAG_GET_MAX_TEMP               = 0x0003000A,

    /* Memory */
    MBOX_TAG_ALLOCATE_MEMORY            = 0x0003000C,
    MBOX_TAG_LOCK_MEMORY                = 0x0003000D,
    MBOX_TAG_UNLOCK_MEMORY              = 0x0003000E,
    MBOX_TAG_RELEASE_MEMORY             = 0x0003000F,

    /* Execute code */
    MBOX_TAG_EXECUTE_CODE               = 0x00030010,

    /* Displaymax */
    MBOX_TAG_GET_DISPMANX_HANDLE        = 0x00030014,
    MBOX_TAG_GET_EDID_BLOCK             = 0x00030020,

    /* Framebuffer */
    MBOX_TAG_ALLOCATE_FRAMEBUFFER       = 0x00040001,
    MBOX_TAG_RELEASE_FRAMEBUFFER        = 0x00048001,
    MBOX_TAG_BLANK_SCREEN               = 0x00040002,

    MBOX_TAG_GET_PHYSICAL_WIDTH_HEIGHT  = 0x00040003,
    MBOX_TAG_GET_VIRTUAL_WIDTH_HEIGHT   = 0x00040004,
    MBOX_TAG_GET_DEPTH                  = 0x00040005,
    MBOX_TAG_GET_PIXEL_ORDER            = 0x00040006,
    MBOX_TAG_GET_ALPHA_MODE             = 0x00040007,
    MBOX_TAG_GET_PITCH                  = 0x00040008,
    MBOX_TAG_GET_VIRTUAL_OFFSET         = 0x00040009,
    MBOX_TAG_GET_OVERSCAN               = 0x0004000A,
    MBOX_TAG_GET_PALETTE                = 0x0004000B,

    MBOX_TAG_TEST_PHYSICAL_WIDTH_HEIGHT = 0x00044003,
    MBOX_TAG_TEST_VIRTUAL_WIDTH_HEIGHT  = 0x00044004,
    MBOX_TAG_TEST_DEPTH                 = 0x00044005,
    MBOX_TAG_TEST_PIXEL_ORDER           = 0x00044006,
    MBOX_TAG_TEST_ALPHA_MODE            = 0x00044007,
    MBOX_TAG_TEST_VIRTUAL_OFFSET        = 0x00044009,
    MBOX_TAG_TEST_OVERSCAN              = 0x0004400A,
    MBOX_TAG_TEST_PALETTE               = 0x0004400B,

    MBOX_TAG_SET_PHYSICAL_WIDTH_HEIGHT  = 0x00048003,
    MBOX_TAG_SET_VIRTUAL_WIDTH_HEIGHT   = 0x00048004,
    MBOX_TAG_SET_DEPTH                  = 0x00048005,
    MBOX_TAG_SET_PIXEL_ORDER            = 0x00048006,
    MBOX_TAG_SET_ALPHA_MODE             = 0x00048007,
    MBOX_TAG_SET_VIRTUAL_OFFSET         = 0x00048009,
    MBOX_TAG_SET_OVERSCAN               = 0x0004800A,
    MBOX_TAG_SET_PALETTE                = 0x0004800B,

    /* Cursor */
    MBOX_TAG_SET_CURSOR_INFO            = 0x00008010,
    MBOX_TAG_SET_CURSOR_STATE           = 0x00008011,

    /* QPU */
    MBOX_TAG_EXECUTE_QPU                = 0x00030011,
    MBOX_TAG_SET_ENABLE_QPU             = 0x00030012,

    /* SD Card */
    /* VHCIQ */
} MboxTag;

typedef enum {
    MBOX_CLK_EMMC = 0x00000001,
    MBOX_CLK_UART,
    MBOX_CLK_ARM,
    MBOX_CLK_CORE,
    MBOX_CLK_V3D,
    MBOX_CLK_H264,
    MBOX_CLK_ISP,
    MBOX_CLK_SDRAM,
    MBOX_CLK_PIXEL,
    MBOX_CLK_PWM,
    MBOX_CLK_HEVC,
    MBOX_CLK_EMMC2,
    MBOX_CLK_M2MC,
    MBOX_CLK_PIXEL_BVB,
} MboxClock;

typedef enum {
    MEM_FLAG_DISCARDABLE      = 1 << 0, /* can be resized to 0 at any time. Use for cached data */
    MEM_FLAG_NORMAL           = 0 << 2, /* normal allocating alias. Don't use from ARM */
    MEM_FLAG_DIRECT           = 1 << 2, /* 0xC alias uncached */
    MEM_FLAG_COHERENT         = 2 << 2, /* 0x8 alias. Non-allocating in L2 but coherent */
    MEM_FLAG_L1_NONALLOCATING = (MEM_FLAG_DIRECT | MEM_FLAG_COHERENT), /* Allocating in L2 */
    MEM_FLAG_ZERO             = 1 << 4,  /* initialise buffer to all zeros */
    MEM_FLAG_NO_INIT          = 1 << 5, /* don't initialise (default is initialise to all ones */
    MEM_FLAG_HINT_PERMALOCK   = 1 << 6, /* Likely to be locked for long periods of time. */
} MboxGPUFlag;

typedef enum {
    PIXEL_ORDER_BGR,
    PIXEL_ORDER_RGB,
} PixelOrder;

typedef enum {
    ALPHA_ENABLED,   // 0 = fully opaque
    ALPHA_REVERSED,  // 0 = fully transparent
    ALPHA_IGNORED,
} AlphaMode;

/* GENERAL */
bool mbox_get_property(uint32_t* msg);
bool mbox_get_property_batch(uint32_t count, ...);

/* HARDWARE */
uint32_t mbox_get_board_model();
uint32_t mbox_get_board_revision();
uint32_t mbox_get_board_serial();
uint32_t mbox_get_arm_memory();
uint32_t mbox_get_vc_memory();

/* TEMPERATURE */
uint32_t mbox_get_temp();
uint32_t mbox_get_max_temp();

/* CLOCKS */
uint32_t mbox_get_clock_rate(MboxClock clock_id);
uint32_t mbox_get_measured_clock_rate(MboxClock clock_id);
uint32_t mbox_get_max_clock_rate(MboxClock clock_id);
uint32_t mbox_get_min_clock_rate(MboxClock clock_id);
void mbox_set_clock_rate(MboxClock clock_id, uint32_t clock_rate);

/* MEMORY */
uint32_t mbox_allocate_memory(uint32_t size, uint32_t alignment, uint32_t flags);
uint32_t mbox_lock_memory(uint32_t handle);
uint32_t mbox_unlock_memory(uint32_t handle);
uint32_t mbox_release_memory(uint32_t handle);

/* FRAME BUFFER */
void mbox_framebuffer_init(uint32_t width, uint32_t height, uint32_t vwidth, uint32_t vheight,
        uint32_t depth, uint32_t** fb_ptr);
void mbox_allocate_framebuffer(uint32_t alignment, uint32_t** base_addr, uint32_t* buf_size);
void mbox_release_framebuffer();
void mbox_framebuffer_get_physical_width_height(uint32_t* width, uint32_t* height);
void mbox_framebuffer_get_virtual_width_height(uint32_t* width, uint32_t* height);
uint32_t mbox_framebuffer_get_depth();
PixelOrder mbox_framebuffer_get_pixel_order();
AlphaMode mbox_framebuffer_get_alpha_mode();
uint32_t mbox_framebuffer_get_pitch();
void mbox_framebuffer_get_virtual_offset(uint32_t* offset_x, uint32_t* offset_y);
void mbox_framebuffer_get_overscan(uint32_t* top, uint32_t* bottom, uint32_t* left, uint32_t* right);
// void mbox_framebuffer_get_palette();

void mbox_framebuffer_set_physical_width_height(uint32_t width, uint32_t height);
void mbox_framebuffer_set_virtual_width_height(uint32_t width, uint32_t height);
void mbox_framebuffer_set_depth(uint32_t depth);
void mbox_framebuffer_set_pixel_order(PixelOrder pixel_order);
void mbox_framebuffer_set_alpha_mode(AlphaMode alpha_mode);
void mbox_framebuffer_set_virtual_offset(uint32_t offset_x, uint32_t offset_y);
void mbox_framebuffer_set_overscan(uint32_t top, uint32_t bottom, uint32_t left, uint32_t right);
// void mbox_framebuffer_set_palette();

/* QPU */
// uint32_t mbox_execute_qpu(uint32_t num_gpus, uint32_t control, uint32_t noflush, uint32_t timeout);
uint32_t mbox_set_enable_qpu(uint32_t enable);
#endif
