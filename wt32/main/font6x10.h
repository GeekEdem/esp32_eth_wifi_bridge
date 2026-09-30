/* 6x10 bitmap font for the display (misc-fixed, public domain), generated
 * by wt32/tools/gen_font.py: ASCII, Cyrillic and a few punctuation marks. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define FONT_W 6
#define FONT_H 10

typedef struct {
    uint16_t cp;                /* Unicode code point; the table is sorted by it */
    uint8_t rows[FONT_H];       /* bit 7 = leftmost pixel */
} font_glyph_t;

extern const font_glyph_t font6x10[];
extern const size_t font6x10_count;
