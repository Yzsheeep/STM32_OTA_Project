#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include "main.h"


/* F411 Flash 分区: Bootloader 0x08000000 (64KB), App 0x08010000 (448KB) */
#define APP_FLASH_ADDR     0x08010000U

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

#define OTA_STATE_NORMAL         0U // 正常运行
#define OTA_STATE_INSTALLING     1U // 正在安装新版
#define OTA_STATE_TRIAL_RUNNING  2U // 新版试运行待确认
#define OTA_STATE_RESTORING      3U // 正在从备份区回滚旧版

/* 上位机 'H' 握手下发的 16 字节元数据 */
typedef struct
{
    uint32_t ota_flag;        // 升级请求标志 (0x55AA55AA)
    uint32_t firmware_size;   // 固件字节数
    uint32_t firmware_crc32;  // 整包 CRC32
    uint32_t version;         // 版本号 (如 0x00010008)
} OTA_MetaInfo_t;

/* EEPROM A/B 双备份启动记录 (40 字节) */
typedef struct
{
    uint32_t magic;          // 有效记录魔数 "OTAR"
    uint32_t sequence;       // 递增序号，用于选取最新副本
    uint32_t state;          // 当前升级阶段状态

    uint32_t app_size;       // 已确认旧版长度
    uint32_t app_crc32;      // 已确认旧版 CRC32
    uint32_t app_version;    // 已确认旧版版本

    uint32_t update_size;    // 待安装新版长度
    uint32_t update_crc32;   // 待安装新版 CRC32
    uint32_t update_version; // 待安装新版版本

    uint32_t record_crc32;   // 前 36 字节 CRC32
} OTA_BootRecord_t;

void Bootloader_ReadMeta(OTA_MetaInfo_t *meta);
HAL_StatusTypeDef Bootloader_WriteMeta(OTA_MetaInfo_t *meta);

void Bootloader_JumpToApp(uint32_t app_addr);
HAL_StatusTypeDef Bootloader_EraseApp(void);
HAL_StatusTypeDef Bootloader_WriteFlash(uint32_t flash_addr,uint8_t *p_data, uint32_t len);
uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t len);
HAL_StatusTypeDef BootRecord_Load(OTA_BootRecord_t *record, uint16_t *active_addr);

HAL_StatusTypeDef Bootloader_Recover(void);
HAL_StatusTypeDef Bootloader_PrepareInstall(const OTA_MetaInfo_t *meta);

#endif
