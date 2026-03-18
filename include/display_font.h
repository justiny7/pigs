#ifndef DISPLAY_FONT_H
#define DISPLAY_FONT_H

#include <stdint.h>

// DOS/V TWN16: https://int10h.org/oldschool-pc-fonts/fontlist/font?dos-v_twn16
#define FONT_WIDTH 8
#define FONT_HEIGHT 16

typedef struct {
    uint8_t bitmap[FONT_HEIGHT];
} FontLetter;

typedef struct {
    uint32_t scale;

    uint32_t* fb;
    uint32_t height, width;
    uint32_t color;
} FontSettings;

void font_init();

void display_char(FontSettings* fs, const uint8_t ch, uint32_t r, uint32_t c);
void display_text(FontSettings* fs, const uint8_t* s, uint32_t n,
        uint32_t r, uint32_t c);

#endif
