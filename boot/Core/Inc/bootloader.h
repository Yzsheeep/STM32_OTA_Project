#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include <stdint.h>
#include "main.h"

#define APP_FLASH_ADDR 0x08004000
#define APP_FLASH_PAGES 48

#define AT24C02_ADDR        0xA0  // EEPROM 设备地址（0x50 左移 1 位）
#define OTA_META_MEM_ADDR   0x00  // EEPROM 固件信息起点
// 升级标志
#define OTA_FLAG_UPDATE     0x55AA55AA  // 升级请求，小端字节 AA 55 AA 55
#define OTA_FLAG_NORMAL     0x00000000

// 固件信息，共 16 字节
typedef struct {
    uint32_t ota_flag;       // 升级标志：0=正常，0x55AA55AA=升级
    uint32_t firmware_size;  // 固件长度（字节）
    uint32_t firmware_crc32; // 固件 CRC32，尚未校验
    uint32_t version;        // 固件版本，尚未比较
} OTA_MetaInfo_t;

void Bootloader_JumpToApp(uint32_t app_addr);

void Bootloader_EraseApp(void);

// 写入内部 Flash，长度需为 4 的倍数
void Bootloader_WriteFlash(uint32_t flash_addr, uint8_t *p_data, uint32_t len);


// EEPROM 固件信息读写接口
void Bootloader_ReadMeta(OTA_MetaInfo_t *meta);
HAL_StatusTypeDef Bootloader_WriteMeta(OTA_MetaInfo_t *meta);

#endif
