#include "display_font.h"
#include "mailbox_interface.h"

#define WIDTH 1280
#define HEIGHT 720

void main() {
    uint32_t* fb;
    mbox_framebuffer_init(WIDTH, HEIGHT, WIDTH, HEIGHT * 2, 32, &fb); // double buffer

    uint32_t* pixels = (uint32_t*)((uintptr_t)fb & 0x3FFFFFFF);

    FontSettings fs;
    fs.height = HEIGHT;
    fs.width = WIDTH;
    fs.color = 0x00FFFFFF;
    fs.scale = 4;
    fs.fb = pixels;
    font_init();

    display_char(&fs, 'a', 0, 0);

    const char* s = "qwertyuiopasdfghjklzxcvbnm";
    display_text(&fs, s, 26, 30, 4);

    const char* ss = "0123456789";
    display_text(&fs, ss, 10, 60, 4);

    const char* test = "test.ply";
    display_text(&fs, test, 8, 90, 4);
}
