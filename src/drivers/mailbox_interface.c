#include "mailbox_interface.h"
#include "lib.h"
#include "mmu.h"

static uint32_t mbox_buf[MBOX_BUFFER_SIZE] __attribute__((aligned(16)));

/* GENERAL */
bool mbox_get_property(uint32_t* msg) {
    uint32_t addr = (uint32_t) msg;
    assert((addr & 0xF) == 0, "Mailbox address must be 16-byte aligned");

    mmu_flush_dcache();
    mbox_write(MB_TAGS_ARM_TO_VC, addr);
    mbox_read(MB_TAGS_ARM_TO_VC);
    mmu_flush_dcache();

    return msg[1] == MBOX_REQUEST_SUCCESS;
}

bool mbox_get_property_batch(uint32_t count, ...) {
    va_list args;
    va_start(args, count);

    mbox_buf[0] = (count + 3) * sizeof(uint32_t);
    mbox_buf[1] = MBOX_REQUEST;
    for (uint32_t i = 0; i < count; i++) {
        mbox_buf[i + 2] = va_arg(args, uint32_t);
    }
    mbox_buf[count + 2] = 0;

    va_end(args);
    return mbox_get_property(mbox_buf);
}


/* HARDARE */
uint32_t mbox_get_board_model() {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_GET_BOARD_MODEL, 4, 0, 0
    ), "Get board model failed");

    return mbox_buf[5];
}
uint32_t mbox_get_board_revision() {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_GET_BOARD_REVISION, 4, 0, 0
    ), "Get board revision failed");

    return mbox_buf[5];
}
uint32_t mbox_get_board_serial() {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_BOARD_SERIAL, 8, 0, 0, 0
    ), "Get board serial failed");

    return mbox_buf[5];
}
uint32_t mbox_get_arm_memory() {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_ARM_MEMORY, 8, 0, 0, 0
    ), "Get ARM memory failed");

    return mbox_buf[6];
}
uint32_t mbox_get_vc_memory() {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_VC_MEMORY, 8, 0, 0, 0
    ), "Get VC memory failed");

    return mbox_buf[6];
}


/* TEMPERATURE */
uint32_t mbox_get_temp() {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_TEMP, 8, 0, 0, 0
    ), "Get temperature failed");

    return mbox_buf[6];
}
uint32_t mbox_get_max_temp() {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_MAX_TEMP, 8, 0, 0, 0
    ), "Get max temperature failed");

    return mbox_buf[6];
}


/* CLOCK */
uint32_t mbox_get_clock_rate(MboxClock clock_id) {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_CLOCK_RATE, 8, 0, clock_id, 0
    ), "Get clock rate failed");

    return mbox_buf[6];
}
uint32_t mbox_get_measured_clock_rate(MboxClock clock_id) {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_CLOCK_RATE_MEASURED, 8, 0, clock_id, 0
    ), "Get measured clock rate failed");

    return mbox_buf[6];
}
uint32_t mbox_get_max_clock_rate(MboxClock clock_id) {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_MAX_CLOCK_RATE, 8, 0, clock_id, 0
    ), "Get max clock rate failed");

    return mbox_buf[6];
}
uint32_t mbox_get_min_clock_rate(MboxClock clock_id) {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_MIN_CLOCK_RATE, 8, 0, clock_id, 0
    ), "Get min clock rate failed");

    return mbox_buf[6];
}
void mbox_set_clock_rate(MboxClock clock_id, uint32_t clock_rate) {
    assert(mbox_get_property_batch(6,
        MBOX_TAG_SET_CLOCK_RATE, 12, 0, clock_id, clock_rate, 1 // skip turbo by default
    ), "Set clock rate failed");
}

/* MEMORY */
uint32_t mbox_allocate_memory(uint32_t size, uint32_t alignment, uint32_t flags) {
    assert(mbox_get_property_batch(6,
        MBOX_TAG_ALLOCATE_MEMORY, 12, 0, size, alignment, flags
    ), "Allocate GPU memory failed");

    return mbox_buf[5];
}
uint32_t mbox_lock_memory(uint32_t handle) {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_LOCK_MEMORY, 4, 0, handle
    ), "Lock GPU memory failed");

    return mbox_buf[5];
}
uint32_t mbox_unlock_memory(uint32_t handle) {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_UNLOCK_MEMORY, 4, 0, handle
    ), "Unlock GPU memory failed");

    return mbox_buf[5];
}
uint32_t mbox_release_memory(uint32_t handle) {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_RELEASE_MEMORY, 4, 0, handle
    ), "Release GPU memory failed");

    return mbox_buf[5];
}


/* FRAME BUFFER */

// Combined framebuffer init - sets all properties in one mailbox call
void mbox_framebuffer_init(uint32_t width, uint32_t height, uint32_t vwidth, uint32_t vheight,
        uint32_t depth, uint32_t** fb_ptr) {
    assert(mbox_get_property_batch(32,
        MBOX_TAG_SET_PHYSICAL_WIDTH_HEIGHT, 8, 0, width, height,
        MBOX_TAG_SET_VIRTUAL_WIDTH_HEIGHT, 8, 0, vwidth, vheight,
        MBOX_TAG_SET_VIRTUAL_OFFSET, 8, 0, 0, 0,
        MBOX_TAG_SET_DEPTH, 4, 0, depth,
        MBOX_TAG_SET_PIXEL_ORDER, 4, 0, PIXEL_ORDER_BGR,
        MBOX_TAG_ALLOCATE_FRAMEBUFFER, 8, 0, 16, 0,
        MBOX_TAG_GET_PITCH, 4, 0, 0
    ), "Init frame buffer failed");

    assert(mbox_buf[29] == vwidth * vheight * sizeof(uint32_t), "Framebuffer init failed: size mismatch");
    assert(mbox_buf[33] == width * 4, "Framebuffer init failed: pitch mismatch");

    *fb_ptr = (uint32_t*) mbox_buf[28];
}
void mbox_allocate_framebuffer(uint32_t alignment, uint32_t** base_addr, uint32_t* buf_size) {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_ALLOCATE_FRAMEBUFFER, 8, 0, alignment, 0
    ), "Allocate framebuffer failed");

    *base_addr = (uint32_t*) mbox_buf[5];
    *buf_size = mbox_buf[6];
}
void mbox_release_framebuffer() {
    assert(mbox_get_property_batch(3,
        MBOX_TAG_RELEASE_FRAMEBUFFER, 0, 0
    ), "Release framebuffer failed");
}
uint32_t mbox_framebuffer_blank_screen(uint32_t state) {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_BLANK_SCREEN, 4, 0, state
    ), "Framebuffer blank screen failed");

    return mbox_buf[5];
}

void mbox_framebuffer_get_physical_width_height(uint32_t* width, uint32_t* height) {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_PHYSICAL_WIDTH_HEIGHT, 8, 0, 0, 0
    ), "Get framebuffer physical width/height failed");

    *width = mbox_buf[5];
    *height = mbox_buf[6];
}
void mbox_framebuffer_get_virtual_width_height(uint32_t* width, uint32_t* height) {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_VIRTUAL_WIDTH_HEIGHT, 8, 0, 0, 0
    ), "Get framebuffer virtual width/height failed");

    *width = mbox_buf[5];
    *height = mbox_buf[6];
}
uint32_t mbox_framebuffer_get_depth() {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_GET_DEPTH, 4, 0, 0
    ), "Framebuffer get depth failed");

    return mbox_buf[5];
}
PixelOrder mbox_framebuffer_get_pixel_order() {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_GET_PIXEL_ORDER, 4, 0, 0
    ), "Framebuffer get pixel order failed");

    return mbox_buf[5];
}
AlphaMode mbox_framebuffer_get_alpha_mode() {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_GET_ALPHA_MODE, 4, 0, 0
    ), "Framebuffer get alpha mode failed");

    return mbox_buf[5];
}
uint32_t mbox_framebuffer_get_pitch() {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_GET_PITCH, 4, 0, 0
    ), "Framebuffer get pitch (bytes per line) failed");

    return mbox_buf[5];
}
void mbox_framebuffer_get_virtual_offset(uint32_t* offset_x, uint32_t* offset_y) {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_GET_VIRTUAL_OFFSET, 8, 0, 0, 0
    ), "Get framebuffer virtual offset failed");

    *offset_x = mbox_buf[5];
    *offset_y = mbox_buf[6];
}
void mbox_framebuffer_get_overscan(uint32_t* top, uint32_t* bottom, uint32_t* left, uint32_t* right) {
    assert(mbox_get_property_batch(7,
        MBOX_TAG_GET_OVERSCAN, 16, 0, 0, 0, 0, 0
    ), "Get framebuffer overscan failed");

    *top = mbox_buf[5];
    *bottom = mbox_buf[6];
    *left = mbox_buf[7];
    *right = mbox_buf[8];
}
void mbox_framebuffer_get_palette() {
    // unimplemented
}

void mbox_framebuffer_set_physical_width_height(uint32_t width, uint32_t height) {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_SET_PHYSICAL_WIDTH_HEIGHT, 8, 0, width, height
    ), "Set framebuffer physical width/height failed");
}
void mbox_framebuffer_set_virtual_width_height(uint32_t width, uint32_t height) {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_SET_VIRTUAL_WIDTH_HEIGHT, 8, 0, width, height
    ), "Set framebuffer virtual width/height failed");
}
void mbox_framebuffer_set_depth(uint32_t depth) {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_SET_DEPTH, 4, 0, depth
    ), "Framebuffer set depth failed");
}
void mbox_framebuffer_set_pixel_order(PixelOrder pixel_order) {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_SET_PIXEL_ORDER, 4, 0, pixel_order
    ), "Framebuffer set pixel order failed");
}
void mbox_framebuffer_set_alpha_mode(AlphaMode alpha_mode) {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_SET_ALPHA_MODE, 4, 0, alpha_mode
    ), "Framebuffer set alpha mode failed");
}
void mbox_framebuffer_set_virtual_offset(uint32_t offset_x, uint32_t offset_y) {
    assert(mbox_get_property_batch(5,
        MBOX_TAG_SET_VIRTUAL_OFFSET, 8, 0, offset_x, offset_y
    ), "Set framebuffer virtual offset failed");
}
void mbox_framebuffer_set_overscan(uint32_t top, uint32_t bottom, uint32_t left, uint32_t right) {
    assert(mbox_get_property_batch(7,
        MBOX_TAG_SET_OVERSCAN, 16, 0, top, bottom, left, right
    ), "Set framebuffer overscan failed");
}
void mbox_framebuffer_set_palette() {
    // unimplemented
}


/* QPU */
uint32_t mbox_execute_gpu(uint32_t num_qpus, uint32_t control, uint32_t noflush, uint32_t timeout) { // doesn't seem to work...
    assert(mbox_get_property_batch(7,
        MBOX_TAG_EXECUTE_QPU, 16, 0, num_qpus, control, noflush, timeout
    ), "Execute QPU failed");

    return mbox_buf[5];
}
uint32_t mbox_set_enable_qpu(uint32_t enable) {
    assert(mbox_get_property_batch(4,
        MBOX_TAG_SET_ENABLE_QPU, 4, 0, enable
    ), "Enable QPU failed");

    return mbox_buf[5];
}
