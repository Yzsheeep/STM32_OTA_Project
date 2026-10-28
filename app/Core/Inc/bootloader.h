#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include <stdint.h>

#define APP_FLASH_ADDR 0x08004000

void Bootloader_JumpToApp(uint32_t app_addr);

#endif
