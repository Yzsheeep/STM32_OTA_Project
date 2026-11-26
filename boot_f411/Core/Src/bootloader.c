#include "bootloader.h"
#include "i2c.h"
#include "w25q64.h"
#include "oled.h"
#include "usart.h"
#include <string.h>

// 分段流式计算 CRC32 (多项式 0xEDB88320)
uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; bit++)
        {
            if ((crc & 1U) != 0U)
            {
                crc = (crc >> 1) ^ 0xEDB88320U;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc;
}

// 从 EEPROM 指定地址读取 40B 启动记录
static HAL_StatusTypeDef BootRecord_Read(uint16_t mem_addr, OTA_BootRecord_t *record)
{
    uint8_t *data = (uint8_t *)record;
    HAL_StatusTypeDef result;

    for (uint16_t i = 0; i < sizeof(OTA_BootRecord_t); i++)
    {
        result = HAL_I2C_Mem_Read(&hi2c2, AT24C02_ADDR, mem_addr + i, I2C_MEMADD_SIZE_8BIT, &data[i], 1, 100);

        if (result != HAL_OK)
        {
            return result;
        }
    }

    return HAL_OK;
}

// 校验记录魔数、状态范围、CRC32 及镜像长度对齐
static uint8_t BootRecord_IsValid(const OTA_BootRecord_t *record)
{
    const uint8_t *data = (const uint8_t *)record;
    uint32_t crc;

    if (record->magic != OTA_RECORD_MAGIC || record->state > OTA_STATE_RESTORING)
    {
        return 0;
    }

    crc = CRC32_Update(0xFFFFFFFFU, data, sizeof(OTA_BootRecord_t) - sizeof(uint32_t));
    crc ^= 0xFFFFFFFFU;

    if (crc != record->record_crc32)
    {
        return 0;
    }

    if (record->app_size < 8U || record->app_size > OTA_BACKUP_SIZE || record->app_size % 4U != 0U)
    {
        return 0;
    }

    if (record->state != OTA_STATE_NORMAL && (record->update_size < 8U || record->update_size > OTA_STAGING_SIZE || record->update_size % 4U != 0U))
    {
        return 0;
    }

    return 1;
}

// 加载 A/B 双备份记录，优先选取有效且 sequence 较新的一份
HAL_StatusTypeDef BootRecord_Load(OTA_BootRecord_t *record, uint16_t *active_addr)
{
    OTA_BootRecord_t record_a;
    OTA_BootRecord_t record_b;
    HAL_StatusTypeDef result;
    uint8_t valid_a;
    uint8_t valid_b;

    *active_addr = 0U;

    result = BootRecord_Read(OTA_RECORD_A_MEM_ADDR, &record_a);
    if (result != HAL_OK)
    {
        return result;
    }

    result = BootRecord_Read(OTA_RECORD_B_MEM_ADDR, &record_b);
    if (result != HAL_OK)
    {
        return result;
    }

    valid_a = BootRecord_IsValid(&record_a);
    valid_b = BootRecord_IsValid(&record_b);

    if (valid_a == 0U && valid_b == 0U)
    {
        return HAL_OK;
    }

    if (valid_a != 0U && (valid_b == 0U || record_a.sequence >= record_b.sequence))
    {
        *record = record_a;
        *active_addr = OTA_RECORD_A_MEM_ADDR;
    }
    else
    {
        *record = record_b;
        *active_addr = OTA_RECORD_B_MEM_ADDR;
    }

    return HAL_OK;
}

// 擦除片内 App 区 (Sector 4~7, 0x08010000 - 0x0807FFFF)
HAL_StatusTypeDef Bootloader_EraseApp(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0;
    HAL_StatusTypeDef result;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = FLASH_SECTOR_4;
    erase.NbSectors = 4;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return HAL_ERROR;
    }

    result = HAL_FLASHEx_Erase(&erase, &sector_error);

    HAL_FLASH_Lock();

    return result;
}

// 按 32-bit Word 编程写入片内 App 区
HAL_StatusTypeDef Bootloader_WriteFlash(uint32_t flash_addr,uint8_t *p_data, uint32_t len)
{
    HAL_StatusTypeDef result = HAL_OK;

    if (p_data == NULL || len == 0)
    {
        return HAL_ERROR;
    }

    if (flash_addr % 4U != 0 || len % 4U != 0)
    {
        return HAL_ERROR;
    }

    if (flash_addr < APP_FLASH_ADDR || flash_addr >= 0x08080000U)
    {
        return HAL_ERROR;
    }

    if (len > 0x08080000U - flash_addr)
    {
        return HAL_ERROR;
    }

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return HAL_ERROR;
    }

    for (uint32_t i = 0; i < len; i += 4)
    {
        uint32_t word_data;
        memcpy(&word_data, &p_data[i], 4);

        result = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,flash_addr + i, word_data);

        if (result != HAL_OK)
        {
            break;
        }
    }

    HAL_FLASH_Lock();
    return result;
}

typedef void (*pFunction)(void);

// 关闭中断与 SysTick，重设 MSP 并跳转至 App Reset_Handler
void Bootloader_JumpToApp(uint32_t app_addr)
{
    // 1. 检查向量表首项 MSP 是否在 128KB SRAM 范围内
    uint32_t msp_val = *(volatile uint32_t *)app_addr;

    if (msp_val >= 0x20000000U && msp_val <= 0x20020000U)
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)"Jumping to App...\r\n", 19, 1000);

        // 2. 关中断及 SysTick，防止跳入 App 初始化前触发旧中断
        __disable_irq();
        SysTick->CTRL = 0;
        SysTick->LOAD = 0;
        SysTick->VAL  = 0;

        // 3. 取 +4 处复位向量，切换主栈指针并跳转
        uint32_t jump_addr = *(volatile uint32_t *)(app_addr + 4);
        pFunction Jump_To_Application = (pFunction)jump_addr;

        __set_MSP(msp_val);
        Jump_To_Application();
        while (1)
        {
        }
    }
    else
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)"No Valid App!\r\n", 15, 1000);
    }
}

// 读取 EEPROM 0x00 处的 16B 升级元数据
void Bootloader_ReadMeta(OTA_MetaInfo_t *meta)
{
    uint8_t *data = (uint8_t *)meta;

    for (uint16_t i = 0; i < sizeof(OTA_MetaInfo_t); i++)
    {
        if (HAL_I2C_Mem_Read(&hi2c2, AT24C02_ADDR, OTA_META_MEM_ADDR + i,I2C_MEMADD_SIZE_8BIT, &data[i], 1, 1000) != HAL_OK)
        {
            HAL_UART_Transmit(&huart1, (uint8_t *)"Meta Read Err!\r\n", 16, 1000);
            OLED_ShowStatus("Meta read error");
            Error_Handler();
        }
    }
}

// AT24C02 按 8B 页写入并轮询内部写周期结束
static HAL_StatusTypeDef EEPROM_WritePages(uint16_t address, uint8_t *data, uint16_t length)
{
    HAL_StatusTypeDef result;
    for (uint16_t offset = 0; offset < length; offset += 8)
    {
        result = HAL_I2C_Mem_Write(&hi2c2, AT24C02_ADDR,address + offset,I2C_MEMADD_SIZE_8BIT, data + offset, 8, 100);
        if (result != HAL_OK)
        {
            return result;
        }

        uint32_t start = HAL_GetTick();
        while (HAL_I2C_IsDeviceReady(&hi2c2, AT24C02_ADDR, 1, 2) != HAL_OK)
        {
            if ((uint32_t)(HAL_GetTick() - start) >= 20U)
            {
                return HAL_TIMEOUT;
            }
            HAL_Delay(1);
        }
    }

    return HAL_OK;
}

// 写入 16B 元数据并逐字节读回核验
HAL_StatusTypeDef Bootloader_WriteMeta(OTA_MetaInfo_t *meta)
{
    OTA_MetaInfo_t readback = {0};
    HAL_StatusTypeDef result = EEPROM_WritePages(OTA_META_MEM_ADDR, (uint8_t *)meta, sizeof(*meta));
    if (result != HAL_OK)
    {
        return result;
    }

    uint8_t *read_data = (uint8_t *)&readback;
    for (uint16_t i = 0; i < sizeof(readback); i++)
    {
        result = HAL_I2C_Mem_Read(&hi2c2, AT24C02_ADDR, OTA_META_MEM_ADDR + i,I2C_MEMADD_SIZE_8BIT, read_data + i, 1, 100);
        if (result != HAL_OK)
        {
            return result;
        }
    }

    return memcmp(meta, &readback, sizeof(readback)) == 0 ? HAL_OK : HAL_ERROR;
}

#define TRIAL_MARK_ADDR  0x90U // 试运行确认字节地址
#define TRIAL_PENDING    0x59U // 待 App 确认
#define TRIAL_CONFIRMED  0xA6U // App 已确认

static OTA_BootRecord_t boot_record; // 当前生效的启动记录
static uint16_t active_addr;         // 当前记录地址 (0x20 或 0x60)
static uint8_t image_page[256];      // 外部 Flash 分块读写缓冲

// 写入 0x90 试运行标记并读回确认
static HAL_StatusTypeDef WriteTrialMark(uint8_t value)
{
    uint8_t check = 0;

    if (HAL_I2C_Mem_Write(&hi2c2, AT24C02_ADDR, TRIAL_MARK_ADDR, I2C_MEMADD_SIZE_8BIT, &value, 1, 100) != HAL_OK)
    {
        return HAL_ERROR;
    }

    HAL_Delay(10); // 等待 EEPROM 内部写周期

    if (HAL_I2C_Mem_Read(&hi2c2, AT24C02_ADDR, TRIAL_MARK_ADDR, I2C_MEMADD_SIZE_8BIT, &check, 1, 100) != HAL_OK)
    {
        return HAL_ERROR;
    }

    return (check == value) ? HAL_OK : HAL_ERROR;
}

// 交替写入非活动副本，读回校验通过后再更新 active_addr
static HAL_StatusTypeDef BootRecord_Save(uint32_t state)
{
    OTA_BootRecord_t next = boot_record;
    OTA_BootRecord_t check;
    uint16_t addr = OTA_RECORD_A_MEM_ADDR;

    if (active_addr == OTA_RECORD_A_MEM_ADDR)
    {
        addr = OTA_RECORD_B_MEM_ADDR;
    }

    if (next.sequence == 0xFFFFFFFFU)
    {
        return HAL_ERROR;
    }

    next.sequence++;
    next.state = state;
    next.record_crc32 = CRC32_Update(0xFFFFFFFFU, (const uint8_t *)&next, sizeof(next) - sizeof(uint32_t)) ^ 0xFFFFFFFFU;

    if (!BootRecord_IsValid(&next))
    {
        return HAL_ERROR;
    }

    if (EEPROM_WritePages(addr, (uint8_t *)&next, sizeof(next)) != HAL_OK)
    {
        return HAL_ERROR;
    }

    if (BootRecord_Read(addr, &check) != HAL_OK || memcmp(&next, &check, sizeof(next)) != 0)
    {
        return HAL_ERROR;
    }

    boot_record = next;
    active_addr = addr;
    return HAL_OK;
}

// 校验片内 App 向量表与整包 CRC32
static uint8_t CheckInternalApp(uint32_t size, uint32_t expected_crc)
{
    const uint8_t *app = (const uint8_t *)APP_FLASH_ADDR;
    uint32_t stack = 0;
    uint32_t entry = 0;
    uint32_t crc = 0;

    if (size < 8U || size > OTA_STAGING_SIZE)
    {
        return 0;
    }

    memcpy(&stack, app, 4);
    memcpy(&entry, app + 4, 4);

    // MSP 需落在 SRAM 内且 8B 对齐，Reset_Handler 需带 Thumb 标志位
    if (stack <= 0x20000000U || stack > 0x20020000U || stack % 8U != 0U)
    {
        return 0;
    }

    if ((entry & 1U) == 0U || entry < APP_FLASH_ADDR || (entry - APP_FLASH_ADDR) >= size)
    {
        return 0;
    }

    crc = CRC32_Update(0xFFFFFFFFU, app, size) ^ 0xFFFFFFFFU;
    return (crc == expected_crc) ? 1U : 0U;
}

// 分块校验 W25Q64 内镜像向量表与 CRC32
static HAL_StatusTypeDef CheckExternalImage(uint32_t addr, uint32_t size, uint32_t expected_crc)
{
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t offset = 0;

    if (size < 8U || size > OTA_STAGING_SIZE)
    {
        return HAL_ERROR;
    }

    for (offset = 0; offset < size; offset += sizeof(image_page))
    {
        uint16_t count = (size - offset > sizeof(image_page)) ? sizeof(image_page) : (uint16_t)(size - offset);

        if (W25Q64_Read_Data(addr + offset, image_page, count) != HAL_OK)
        {
            return HAL_ERROR;
        }

        if (offset == 0U) // 首块顺带检查栈顶和复位向量
        {
            uint32_t stack = 0;
            uint32_t entry = 0;
            memcpy(&stack, image_page, 4);
            memcpy(&entry, image_page + 4, 4);

            if (stack <= 0x20000000U || stack > 0x20020000U || stack % 8U != 0U)
            {
                return HAL_ERROR;
            }
            if ((entry & 1U) == 0U || entry < APP_FLASH_ADDR || (entry - APP_FLASH_ADDR) >= size)
            {
                return HAL_ERROR;
            }
        }

        crc = CRC32_Update(crc, image_page, count);
    }

    crc ^= 0xFFFFFFFFU;
    return (crc == expected_crc) ? HAL_OK : HAL_ERROR;
}

// 擦除片内 App 区并从 W25Q64 指定区域搬运镜像（安装与回滚复用）
static HAL_StatusTypeDef CopyW25Q64ToApp(uint32_t source_addr, uint32_t size, uint32_t expected_crc)
{
    uint32_t offset = 0;

    if (Bootloader_EraseApp() != HAL_OK)
    {
        return HAL_ERROR;
    }

    for (offset = 0; offset < size; offset += sizeof(image_page))
    {
        uint16_t count = (size - offset > sizeof(image_page)) ? sizeof(image_page) : (uint16_t)(size - offset);

        if (W25Q64_Read_Data(source_addr + offset, image_page, count) != HAL_OK)
        {
            return HAL_ERROR;
        }

        if (Bootloader_WriteFlash(APP_FLASH_ADDR + offset, image_page, count) != HAL_OK)
        {
            return HAL_ERROR;
        }
    }

    return CheckInternalApp(size, expected_crc) ? HAL_OK : HAL_ERROR;
}

// 从 0x4000 备份区恢复旧版 App（支持断电重入）
static HAL_StatusTypeDef RestoreBackupApp(void)
{
    OTA_MetaInfo_t normal_meta = {0};

    // 1. 核验外部备份区镜像完整性
    if (CheckExternalImage(OTA_BACKUP_ADDR, boot_record.app_size, boot_record.app_crc32) != HAL_OK)
    {
        return HAL_ERROR;
    }

    // 2. 擦写片内前先落盘 RESTORING 状态
    if (boot_record.state != OTA_STATE_RESTORING)
    {
        if (BootRecord_Save(OTA_STATE_RESTORING) != HAL_OK)
        {
            return HAL_ERROR;
        }
    }

    // 3. 将备份镜像写回片内并校验
    HAL_UART_Transmit(&huart1, (uint8_t *)"OTA restoring\r\n", 15, 1000);
    OLED_ShowStatus("Restoring...");
    if (CopyW25Q64ToApp(OTA_BACKUP_ADDR, boot_record.app_size, boot_record.app_crc32) != HAL_OK)
    {
        return HAL_ERROR;
    }

    // 4. 清除试运行标记，同步元数据并恢复 NORMAL 状态
    if (WriteTrialMark(0) != HAL_OK)
    {
        return HAL_ERROR;
    }

    normal_meta.ota_flag = OTA_FLAG_NORMAL;
    normal_meta.firmware_size = boot_record.app_size;
    normal_meta.firmware_crc32 = boot_record.app_crc32;
    normal_meta.version = boot_record.app_version;
    if (Bootloader_WriteMeta(&normal_meta) != HAL_OK)
    {
        return HAL_ERROR;
    }

    boot_record.update_size = 0;
    boot_record.update_crc32 = 0;
    boot_record.update_version = 0;
    if (BootRecord_Save(OTA_STATE_NORMAL) != HAL_OK)
    {
        return HAL_ERROR;
    }

    HAL_UART_Transmit(&huart1, (uint8_t *)"OTA rolled back\r\n", 17, 1000);
    OLED_ShowStatus("Rolled back");
    return HAL_OK;
}

// 收到 'C' 后执行：校验新旧镜像 -> 备份当前 App 到 0x4000 -> 提交 INSTALLING
HAL_StatusTypeDef Bootloader_PrepareInstall(const OTA_MetaInfo_t *meta)
{
    uint32_t offset = 0;

    if (boot_record.state != OTA_STATE_NORMAL)
    {
        return HAL_ERROR;
    }

    if (meta->firmware_size < 8U || meta->firmware_size > OTA_STAGING_SIZE || meta->firmware_size % 4U != 0U)
    {
        return HAL_ERROR;
    }

    // 1. 校验片内现行版本与暂存区候选版本
    OLED_ShowStatus("Checking images");
    if (!CheckInternalApp(boot_record.app_size, boot_record.app_crc32))
    {
        return HAL_ERROR;
    }
    if (CheckExternalImage(0, meta->firmware_size, meta->firmware_crc32) != HAL_OK)
    {
        return HAL_ERROR;
    }

    // 2. 备份片内旧版至 W25Q64 0x4000 并读回校验
    OLED_ShowStatus("Backing up App");
    for (offset = 0; offset < boot_record.app_size; offset += 4096U)
    {
        if (W25Q64_Erase_Sector(OTA_BACKUP_ADDR + offset) != HAL_OK)
        {
            return HAL_ERROR;
        }
    }

    for (offset = 0; offset < boot_record.app_size; offset += sizeof(image_page))
    {
        uint16_t count = (boot_record.app_size - offset > sizeof(image_page)) ? sizeof(image_page) : (uint16_t)(boot_record.app_size - offset);
        if (W25Q64_Write_Page(OTA_BACKUP_ADDR + offset, (uint8_t *)(APP_FLASH_ADDR + offset), count) != HAL_OK)
        {
            return HAL_ERROR;
        }
    }

    if (CheckExternalImage(OTA_BACKUP_ADDR, boot_record.app_size, boot_record.app_crc32) != HAL_OK)
    {
        return HAL_ERROR;
    }

    // 3. 记录待升级镜像信息，切换为 INSTALLING
    boot_record.update_size = meta->firmware_size;
    boot_record.update_crc32 = meta->firmware_crc32;
    boot_record.update_version = meta->version;
    return BootRecord_Save(OTA_STATE_INSTALLING);
}

// 上电状态机：正常校验 / 断电续装 / 试运行确认或看门狗超时回滚
HAL_StatusTypeDef Bootloader_Recover(void)
{
    uint8_t mark = 0;
    OTA_MetaInfo_t normal_meta = {0};

    if ((FLASH->OPTCR & FLASH_OPTCR_WDG_SW) == 0U) // 需保持软件看门狗模式
    {
        return HAL_ERROR;
    }

    if (BootRecord_Load(&boot_record, &active_addr) != HAL_OK || active_addr == 0U)
    {
        return HAL_ERROR;
    }

    switch (boot_record.state)
    {
        case OTA_STATE_NORMAL: // 常规启动：校验片内 App 后放行
            OLED_ShowStatus("Normal boot");
            return CheckInternalApp(boot_record.app_size, boot_record.app_crc32) ? HAL_OK : HAL_ERROR;

        case OTA_STATE_INSTALLING: // 搬运新固件并启动试运行（支持断电续装）
            HAL_UART_Transmit(&huart1, (uint8_t *)"OTA installing\r\n", 16, 1000);
            OLED_ShowStatus("Installing...");

            if (CheckExternalImage(OTA_BACKUP_ADDR, boot_record.app_size, boot_record.app_crc32) != HAL_OK)
            {
                return HAL_ERROR;
            }
            if (CheckExternalImage(0, boot_record.update_size, boot_record.update_crc32) != HAL_OK ||
                CopyW25Q64ToApp(0, boot_record.update_size, boot_record.update_crc32) != HAL_OK)
            {
                return RestoreBackupApp();
            }

            if (WriteTrialMark(TRIAL_PENDING) != HAL_OK)
            {
                return HAL_ERROR;
            }
            if (BootRecord_Save(OTA_STATE_TRIAL_RUNNING) != HAL_OK)
            {
                return HAL_ERROR;
            }

            // 跳转前开启 IWDG (LSI/256, 约 12s 超时)
            HAL_UART_Transmit(&huart1, (uint8_t *)"OTA trial\r\n", 11, 1000);
            OLED_ShowStatus("Trial boot");
            IWDG->KR  = 0xCCCCU;
            IWDG->KR  = 0x5555U;
            IWDG->PR  = 6U;
            IWDG->RLR = 1499U;
            while (IWDG->SR != 0U) {}
            IWDG->KR  = 0xAAAAU;
            Bootloader_JumpToApp(APP_FLASH_ADDR);
            return HAL_ERROR;

        case OTA_STATE_TRIAL_RUNNING: // 核查 0x90 确认标记：通过则转正，未确认则回滚
            if (HAL_I2C_Mem_Read(&hi2c2, AT24C02_ADDR, TRIAL_MARK_ADDR, I2C_MEMADD_SIZE_8BIT, &mark, 1, 100) != HAL_OK)
            {
                return HAL_ERROR;
            }

            if (mark != TRIAL_CONFIRMED || !CheckInternalApp(boot_record.update_size, boot_record.update_crc32))
            {
                return RestoreBackupApp();
            }

            boot_record.app_size = boot_record.update_size;
            boot_record.app_crc32 = boot_record.update_crc32;
            boot_record.app_version = boot_record.update_version;
            boot_record.update_size = 0;
            boot_record.update_crc32 = 0;
            boot_record.update_version = 0;

            normal_meta.ota_flag = OTA_FLAG_NORMAL;
            normal_meta.firmware_size = boot_record.app_size;
            normal_meta.firmware_crc32 = boot_record.app_crc32;
            normal_meta.version = boot_record.app_version;
            if (Bootloader_WriteMeta(&normal_meta) != HAL_OK)
            {
                return HAL_ERROR;
            }
            if (BootRecord_Save(OTA_STATE_NORMAL) != HAL_OK)
            {
                return HAL_ERROR;
            }
            if (WriteTrialMark(0) != HAL_OK)
            {
                return HAL_ERROR;
            }

            HAL_UART_Transmit(&huart1, (uint8_t *)"OTA confirmed\r\n", 15, 1000);
            OLED_ShowStatus("OTA confirmed");
            return HAL_OK;

        case OTA_STATE_RESTORING: // 回滚中途断电后继续恢复
            return RestoreBackupApp();

        default:
            return HAL_ERROR;
    }
}

