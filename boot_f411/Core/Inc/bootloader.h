#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include "main.h"

/* USER CODE BEGIN Definitions */
/* F411 分区：Bootloader 占前 64 KB，App 从 0x08010000 开始。
 * App 的 Keil 链接起点和 VTOR 必须与下方地址一致。
 * 擦除、写入和跳转接口已实现，并完成一次实际 OTA 验证。
 */
#define APP_FLASH_ADDR     0x08010000U

/* 暂存区仍只擦除 16 KB，暂不随 F411 的内部容量扩大固件上限。 */
#define OTA_STAGING_SIZE   (16U * 1024U)
#define AT24C02_ADDR       0xA0
#define OTA_META_MEM_ADDR  0x00
#define OTA_FLAG_UPDATE    0x55AA55AAU
#define OTA_FLAG_NORMAL    0x00000000U

/* 与上位机原协议相同：四个 uint32_t，按顺序共占 16 字节。 */
typedef struct
{
    uint32_t ota_flag;
    uint32_t firmware_size;
    uint32_t firmware_crc32;
    uint32_t version;
} OTA_MetaInfo_t;

void Bootloader_ReadMeta(OTA_MetaInfo_t *meta);
HAL_StatusTypeDef Bootloader_WriteMeta(OTA_MetaInfo_t *meta);

/* F411 片内 Flash 擦除、写入及 App 跳转接口。 */
void Bootloader_JumpToApp(uint32_t app_addr);
HAL_StatusTypeDef Bootloader_EraseApp(void);
HAL_StatusTypeDef Bootloader_WriteFlash(uint32_t flash_addr,
                                      uint8_t *p_data, uint32_t len);
/* USER CODE END Definitions */

#endif
