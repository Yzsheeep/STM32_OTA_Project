#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include <stdint.h>

#define APP_FLASH_ADDR 0x08004000
#define APP_FLASH_PAGES 48

#define AT24C02_ADDR      0xA0  // AT24C02 设备写地址 (7位地址0x50左移1位)
#define OTA_FLAG_MEM_ADDR 0x00  // 标志位存在 AT24C02 的第 0 号字节
#define OTA_FLAG_UPDATE   0xAA  // 0xAA：需要停在一楼升级
#define OTA_FLAG_NORMAL   0x00  // 0x00：正常启动，直接跳去二楼

void Bootloader_JumpToApp(uint32_t app_addr);

void Bootloader_EraseApp(void);

// 从 flash_addr 开始，把 p_data 数组里的 len 个字节写进 Flash
void Bootloader_WriteFlash(uint32_t flash_addr, uint8_t *p_data, uint32_t len);


uint8_t Bootloader_ReadFlag(void);
void Bootloader_WriteFlag(uint8_t flag);

#endif
