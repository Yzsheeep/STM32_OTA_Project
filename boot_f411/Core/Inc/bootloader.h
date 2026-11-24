#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include "main.h"


/* F411 分区：Bootloader 占前 64 KB，App 从 0x08010000 开始。
 * App 的 Keil 链接起点和 VTOR 必须与下方地址一致。
 * 擦除、写入和跳转接口已实现，并完成一次实际 OTA 验证。
 */
#define APP_FLASH_ADDR     0x08010000U

/* 暂存区仍只擦除 16 KB，暂不随 F411 的内部容量扩大固件上限。 */
#define OTA_STAGING_SIZE   (16U * 1024U)
#define OTA_BACKUP_ADDR   0x00004000U
#define OTA_BACKUP_SIZE   OTA_STAGING_SIZE
#define AT24C02_ADDR       0xA0
#define OTA_META_MEM_ADDR  0x00
#define OTA_FLAG_UPDATE    0x55AA55AAU
#define OTA_FLAG_NORMAL    0x00000000U

#define OTA_RECORD_A_MEM_ADDR  0x20U
#define OTA_RECORD_B_MEM_ADDR  0x60U
#define OTA_RECORD_MAGIC       0x4F544152U

#define OTA_STATE_NORMAL         0U // 已有确认可用的 App
#define OTA_STATE_INSTALLING     1U // 旧版备份已验证，准备或正在安装新版
#define OTA_STATE_TRIAL_READY    2U // 新版安装完成，尚未开始试运行
#define OTA_STATE_TRIAL_RUNNING  3U // 已进入试运行流程，等待确认
#define OTA_STATE_RESTORING      4U // 准备或正在恢复旧版

/* 与上位机原协议相同：四个 uint32_t，按顺序共占 16 字节。 */
typedef struct
{
    uint32_t ota_flag;
    uint32_t firmware_size;
    uint32_t firmware_crc32;
    uint32_t version;
} OTA_MetaInfo_t; //沿用现有协议，接收上位机发来的本次新固件信息

typedef struct
{
    uint32_t magic;          // 识别这是不是有效的启动记录
    uint32_t sequence;       // 记录序号，用于判断哪份更新
    uint32_t state;          // 当前升级进行到哪一步

    uint32_t app_size;       // 已确认可用 App 的长度
    uint32_t app_crc32;      // 已确认可用 App 的 CRC
    uint32_t app_version;    // 已确认可用 App 的版本

    uint32_t update_size;    // 待安装新固件的长度
    uint32_t update_crc32;   // 待安装新固件的 CRC
    uint32_t update_version; // 待安装新固件的版本

    uint32_t record_crc32;   // 对前36个字节的CRC
} OTA_BootRecord_t; //保存已确认 App、新固件信息和升级状态，供断电恢复、回滚使用

void Bootloader_ReadMeta(OTA_MetaInfo_t *meta);
HAL_StatusTypeDef Bootloader_WriteMeta(OTA_MetaInfo_t *meta);

/* F411 片内 Flash 擦除、写入及 App 跳转接口。 */
void Bootloader_JumpToApp(uint32_t app_addr);
HAL_StatusTypeDef Bootloader_EraseApp(void);
HAL_StatusTypeDef Bootloader_WriteFlash(uint32_t flash_addr,uint8_t *p_data, uint32_t len);
uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t len);
HAL_StatusTypeDef BootRecord_Load(OTA_BootRecord_t *record, uint16_t *active_addr);

HAL_StatusTypeDef Bootloader_Recover(void);
HAL_StatusTypeDef Bootloader_PrepareInstall(const OTA_MetaInfo_t *meta);

#endif
