#ifndef APP_OLED_H
#define APP_OLED_H

#include "font.h"

typedef enum {
    OLED_COLOR_NORMAL = 0,
    OLED_COLOR_REVERSED
} OLED_ColorMode;

void OLED_Init(void);
void OLED_NewFrame(void);
void OLED_ShowFrame(void);
void OLED_PrintASCIIString(uint8_t x, uint8_t y, const char *str, const ASCIIFont *font, OLED_ColorMode color);
void OLED_DisPlay_On(void);
void OLED_DisPlay_Off(void);

#endif
