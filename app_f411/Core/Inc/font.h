#ifndef APP_FONT_H
#define APP_FONT_H

#include <stdint.h>

typedef struct {
    uint8_t h;
    uint8_t w;
    const uint8_t *chars;
} ASCIIFont;

extern const ASCIIFont afont16x8;

#endif
