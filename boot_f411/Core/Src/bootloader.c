#include "bootloader.h"
#include "i2c.h"
#include "w25q64.h"
#include "oled.h"
#include "usart.h"
#include <string.h>

uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) // 依次处理这段数据的每个字节
    {
        crc ^= data[i]; // 将当前字节合入 CRC

        for (uint8_t bit = 0; bit < 8; bit++) // 一个字节包含 8 位，处理 8 次
        {
            if ((crc & 1U) != 0U) // 检查 CRC 当前最低位是否为 1
            {
                crc = (crc >> 1) ^ 0xEDB88320U; // 右移一位，再与算法常数异或
            }
            else
            {
                crc >>= 1; // 最低位为 0，只右移一位
            }
        }
    }

    return crc; // 返回处理完这段数据后的 CRC，供下一段继续使用
}

// 只负责读取40字节，返回I2C通信结果
static HAL_StatusTypeDef BootRecord_Read(uint16_t mem_addr, OTA_BootRecord_t *record)
{
    uint8_t *data = (uint8_t *)record; // 同一块结构体RAM，按字节依次填入
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

// 检查已读出的记录：1表示有效，0表示无效
static uint8_t BootRecord_IsValid(const OTA_BootRecord_t *record)
{
    const uint8_t *data = (const uint8_t *)record; // 按字节读取结构体，不修改它
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

HAL_StatusTypeDef BootRecord_Load(OTA_BootRecord_t *record, uint16_t *active_addr)
{
    OTA_BootRecord_t record_a;
    OTA_BootRecord_t record_b;
    HAL_StatusTypeDef result;
    uint8_t valid_a;
    uint8_t valid_b;

    *active_addr = 0U; // 0表示没有选出有效记录

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
        return HAL_OK; // 通信成功，但两份记录都无效
    }

    if (valid_a != 0U && (valid_b == 0U || record_a.sequence >= record_b.sequence))
    {
        *record = record_a; // 将A的完整结构体复制到调用者的RAM
        *active_addr = OTA_RECORD_A_MEM_ADDR;
    }
    else
    {
        *record = record_b; // 将B的完整结构体复制到调用者的RAM
        *active_addr = OTA_RECORD_B_MEM_ADDR;
    }

    return HAL_OK;
}
HAL_StatusTypeDef Bootloader_EraseApp(void)
{
    FLASH_EraseInitTypeDef erase = {0}; // 创建擦除参数表，所有成员先清零
    uint32_t sector_error = 0;          // HAL 用它返回擦除失败的扇区编号
    HAL_StatusTypeDef result;           // 保存擦除结果

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;     // 按扇区擦除
    erase.Sector = FLASH_SECTOR_4;                // 从扇区 4 开始，即 0x08010000
    erase.NbSectors = 4;                          // 连续擦除扇区 4、5、6、7
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;   // 芯片供电范围为 2.7～3.6V

    if (HAL_FLASH_Unlock() != HAL_OK) // 擦除前先解锁，检查是否成功
    {
        return HAL_ERROR;            // 解锁失败，退出函数
    }

    result = HAL_FLASHEx_Erase(&erase, &sector_error); // 传入参数表地址，执行擦除并取得结果

    HAL_FLASH_Lock(); // 操作结束，重新锁住 Flash

    return result;   // 将擦除结果返回给调用者
}

HAL_StatusTypeDef Bootloader_WriteFlash(uint32_t flash_addr,uint8_t *p_data, uint32_t len)
{
    HAL_StatusTypeDef result = HAL_OK; // 保存写入结果

    if (p_data == NULL || len == 0) // 数据地址为空或没有数据，直接退出
    {
        return HAL_ERROR;
    }

    if (flash_addr % 4U != 0 || len % 4U != 0) // 写入地址和长度都必须是 4 的倍数
    {
        return HAL_ERROR;
    }

    if (flash_addr < APP_FLASH_ADDR || flash_addr >= 0x08080000U) // 起始地址必须在 APP 区内
    {
        return HAL_ERROR;
    }

    if (len > 0x08080000U - flash_addr) // 数据长度不能超过当前位置到 Flash 末尾的剩余空间
    {
        return HAL_ERROR;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) // 写入前先解锁
    {
        return HAL_ERROR;
    }

    for (uint32_t i = 0; i < len; i += 4) // 每轮处理 4 字节
    {
        uint32_t word_data;                   // 本轮准备写入的 4 字节
        memcpy(&word_data, &p_data[i], 4);     // 从 RAM 数组中取出连续 4 字节

        result = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,flash_addr + i, word_data); // 写入内部 Flash

        if (result != HAL_OK) // 本轮失败，停止后续写入
        {
            break;
        }
    }

    HAL_FLASH_Lock(); // 写入结束或中途失败，都重新上锁
    return result;   // 将结果交给调用者
}

// App 入口函数指针类型
typedef void (*pFunction)(void);

// app_addr：App 向量表起点
void Bootloader_JumpToApp(uint32_t app_addr)
{
    // 1. 读取初始栈顶
    uint32_t msp_val = *(volatile uint32_t *)app_addr; // App 初始栈顶地址

    // 2. 仅检查栈顶地址范围
    if (msp_val >= 0x20000000U && msp_val <= 0x20020000U)
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)"Jumping to App...\r\n", 19, 1000);

        // 3. 关闭可屏蔽中断和 SysTick
        __disable_irq();
        SysTick->CTRL = 0;
        SysTick->LOAD = 0;
        SysTick->VAL  = 0;

        // 4. 读取 App 复位入口
        uint32_t jump_addr = *(volatile uint32_t *)(app_addr + 4); // Reset_Handler 地址，含 Thumb 标记
        pFunction Jump_To_Application = (pFunction)jump_addr; // App 入口函数指针

        // 5. 设置栈顶并跳转；当前编译结果需确认换栈后不再访问旧栈
        __set_MSP(msp_val);
        Jump_To_Application();
        while (1) // App 正常不会返回；保留调用后的执行路径，避免提前执行函数收尾
        {
        }
    }
    else
    {
        // 栈顶地址无效
        HAL_UART_Transmit(&huart1, (uint8_t *)"No Valid App!\r\n", 15, 1000);
    }
}


/* USER CODE BEGIN ReadMeta */
void Bootloader_ReadMeta(OTA_MetaInfo_t *meta)
{
    uint8_t *data = (uint8_t *)meta; // 把结构体起始地址当作字节数组起点，逐字节填入 EEPROM 数据

    for (uint16_t i = 0; i < sizeof(OTA_MetaInfo_t); i++) // 固件信息占 16 字节，依次读取
    {
        if (HAL_I2C_Mem_Read(&hi2c2, AT24C02_ADDR, OTA_META_MEM_ADDR + i,I2C_MEMADD_SIZE_8BIT, &data[i], 1, 1000) != HAL_OK) // EEPROM 第 i 字节存入结构体第 i 字节
        {
            HAL_UART_Transmit(&huart1, (uint8_t *)"Meta Read Err!\r\n", 16, 1000);
            OLED_ShowStatus("Meta read error");
            Error_Handler(); // 与原实现一致：读取失败时停止启动流程
        }
    }
}
/* USER CODE END ReadMeta */

// 调用者提供页对齐的起点，长度为 8 的倍数。
static HAL_StatusTypeDef EEPROM_WritePages(uint16_t address, uint8_t *data, uint16_t length)
{
    HAL_StatusTypeDef result;
    for (uint16_t offset = 0; offset < length; offset += 8)
    {
        result = HAL_I2C_Mem_Write(&hi2c2, AT24C02_ADDR,address + offset,I2C_MEMADD_SIZE_8BIT, data + offset, 8, 100); //AT24C02 的页写入一次最多处理同一页内的 8 字节
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

HAL_StatusTypeDef Bootloader_WriteMeta(OTA_MetaInfo_t *meta)
{
    OTA_MetaInfo_t readback = {0};
    HAL_StatusTypeDef result = EEPROM_WritePages(OTA_META_MEM_ADDR, (uint8_t *)meta, sizeof(*meta));
    if (result != HAL_OK)
    {
        return result;
    }

    /* &readback 指向整块结构体；转为字节指针后依次填入 16 个字节。 */
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
/* USER CODE END Implementation */

// 两个独立的 EEPROM 字节。迁移标记只写一次，试运行时 Boot 和 App 共用 0x90。
#define TRIAL_MARK_ADDR  0x90U
#define MIGRATED_ADDR    0x98U
#define TRIAL_PENDING    0x59U
#define TRIAL_CONFIRMED  0xA6U
#define MIGRATED         0xD3U

static OTA_BootRecord_t boot_record;
static uint16_t active_addr;
static uint8_t image_page[256]; // 启动与安装串行执行，避免占用启动栈

static void Boot_Message(const char *message)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)message, strlen(message), 1000);
}

static HAL_StatusTypeDef ReadMark(uint16_t addr, uint8_t *value)
{
    return HAL_I2C_Mem_Read(&hi2c2, AT24C02_ADDR, addr, I2C_MEMADD_SIZE_8BIT, value, 1, 100);
}

static HAL_StatusTypeDef WriteMark(uint16_t addr, uint8_t value)
{
    uint8_t check;
    if (HAL_I2C_Mem_Write(&hi2c2, AT24C02_ADDR, addr, I2C_MEMADD_SIZE_8BIT, &value, 1, 100) != HAL_OK)
        return HAL_ERROR;
    HAL_Delay(10);
    if (ReadMark(addr, &check) != HAL_OK || check != value)
        return HAL_ERROR;
    return HAL_OK;
}

static HAL_StatusTypeDef BootRecord_Save(uint32_t state)
{
    OTA_BootRecord_t next = boot_record;
    OTA_BootRecord_t check;
    uint16_t addr = OTA_RECORD_A_MEM_ADDR;
    if (active_addr == OTA_RECORD_A_MEM_ADDR)
        addr = OTA_RECORD_B_MEM_ADDR;
    if (next.sequence == UINT32_MAX)
        return HAL_ERROR;

    next.sequence++;
    next.state = state;
    next.record_crc32 = CRC32_Update(UINT32_MAX, (const uint8_t *)&next, sizeof(next) - sizeof(uint32_t)) ^ UINT32_MAX;
    if (!BootRecord_IsValid(&next))
        return HAL_ERROR;
    if (EEPROM_WritePages(addr, (uint8_t *)&next, sizeof(next)) != HAL_OK)
        return HAL_ERROR;
    if (BootRecord_Read(addr, &check) != HAL_OK || memcmp(&next, &check, sizeof(next)) != 0)
        return HAL_ERROR;
    boot_record = next;
    active_addr = addr;
    return HAL_OK;
}

static uint8_t ImageVectorsValid(const uint8_t *image, uint32_t size)
{
    uint32_t stack, entry;
    memcpy(&stack, image, 4);
    memcpy(&entry, image + 4, 4);
    return stack > 0x20000000U && stack <= 0x20020000U && stack % 8U == 0U &&
           (entry & 1U) && entry >= APP_FLASH_ADDR && entry - APP_FLASH_ADDR < size;
}

static uint8_t AppMatches(uint32_t size, uint32_t crc)
{
    const uint8_t *app = (const uint8_t *)APP_FLASH_ADDR;
    return size >= 8U && size <= OTA_STAGING_SIZE && ImageVectorsValid(app, size) &&
           (CRC32_Update(UINT32_MAX, app, size) ^ UINT32_MAX) == crc;
}

static uint16_t PageLength(uint32_t remaining)
{
    return remaining > sizeof(image_page) ? sizeof(image_page) : (uint16_t)remaining;
}

static HAL_StatusTypeDef CheckExternalImage(uint32_t addr, uint32_t size, uint32_t expected)
{
    uint32_t crc = UINT32_MAX;
    for (uint32_t offset = 0; offset < size; offset += sizeof(image_page))
    {
        uint16_t count = PageLength(size - offset);
        if (W25Q64_Read_Data(addr + offset, image_page, count) != HAL_OK)
            return HAL_ERROR;
        if (offset == 0U && !ImageVectorsValid(image_page, size))
            return HAL_ERROR;
        crc = CRC32_Update(crc, image_page, count);
    }
    return (crc ^ UINT32_MAX) == expected ? HAL_OK : HAL_ERROR;
}

// 仅在候选区与片内旧版都通过检查后刷新备份。此时片内旧版仍可启动。
HAL_StatusTypeDef Bootloader_PrepareInstall(const OTA_MetaInfo_t *meta)
{
    if (boot_record.state != OTA_STATE_NORMAL || meta->firmware_size < 8U ||
        meta->firmware_size > OTA_STAGING_SIZE || meta->firmware_size % 4U != 0U)
        return HAL_ERROR;
    OLED_ShowStatus("Checking images");
    if (!AppMatches(boot_record.app_size, boot_record.app_crc32) ||
        CheckExternalImage(0, meta->firmware_size, meta->firmware_crc32) != HAL_OK)
        return HAL_ERROR;

    OLED_ShowStatus("Backing up App");
    for (uint32_t offset = 0; offset < boot_record.app_size; offset += 4096U)
    {
        if (W25Q64_Erase_Sector(OTA_BACKUP_ADDR + offset) != HAL_OK)
            return HAL_ERROR;
    }
    for (uint32_t offset = 0; offset < boot_record.app_size; offset += sizeof(image_page))
    {
        uint16_t count = PageLength(boot_record.app_size - offset);
        if (W25Q64_Write_Page(OTA_BACKUP_ADDR + offset, (uint8_t *)(APP_FLASH_ADDR + offset), count) != HAL_OK)
            return HAL_ERROR;
    }
    if (CheckExternalImage(OTA_BACKUP_ADDR, boot_record.app_size, boot_record.app_crc32) != HAL_OK)
        return HAL_ERROR;

    boot_record.update_size = meta->firmware_size;
    boot_record.update_crc32 = meta->firmware_crc32;
    boot_record.update_version = meta->version;
    return BootRecord_Save(OTA_STATE_INSTALLING);
}

static HAL_StatusTypeDef CopyToApp(uint32_t source, uint32_t size, uint32_t crc)
{
    if (Bootloader_EraseApp() != HAL_OK)
        return HAL_ERROR;
    for (uint32_t offset = 0; offset < size; offset += sizeof(image_page))
    {
        uint16_t count = PageLength(size - offset);
        if (W25Q64_Read_Data(source + offset, image_page, count) != HAL_OK ||
            Bootloader_WriteFlash(APP_FLASH_ADDR + offset, image_page, count) != HAL_OK)
            return HAL_ERROR;
    }
    return AppMatches(size, crc) ? HAL_OK : HAL_ERROR;
}

static HAL_StatusTypeDef FinishNormal(void)
{
    // 提交确认/回滚结果时，原 16 字节信息也应描述最终运行的 App。
    // 首次迁移保留旧 App 刚写入的 U 请求，由 main 消费一次。
    if (active_addr != 0U)
    {
        OTA_MetaInfo_t meta = {OTA_FLAG_NORMAL, boot_record.app_size, boot_record.app_crc32, boot_record.app_version};
        if (Bootloader_WriteMeta(&meta) != HAL_OK)
            return HAL_ERROR;
    }
    boot_record.update_size = 0;
    boot_record.update_crc32 = 0;
    boot_record.update_version = 0;
    return BootRecord_Save(OTA_STATE_NORMAL);
}

static HAL_StatusTypeDef RestoreApp(void)
{
    if (CheckExternalImage(OTA_BACKUP_ADDR, boot_record.app_size, boot_record.app_crc32) != HAL_OK)
        return HAL_ERROR;
    if (boot_record.state != OTA_STATE_RESTORING && BootRecord_Save(OTA_STATE_RESTORING) != HAL_OK)
        return HAL_ERROR;
    Boot_Message("OTA restoring\r\n");
    OLED_ShowStatus("Restoring...");
    if (CopyToApp(OTA_BACKUP_ADDR, boot_record.app_size, boot_record.app_crc32) != HAL_OK ||
        WriteMark(TRIAL_MARK_ADDR, 0) != HAL_OK || FinishNormal() != HAL_OK)
        return HAL_ERROR;
    Boot_Message("OTA rolled back\r\n");
    OLED_ShowStatus("Rolled back");
    return HAL_OK;
}

static HAL_StatusTypeDef LaunchTrial(void)
{
    if (!AppMatches(boot_record.update_size, boot_record.update_crc32))
        return RestoreApp();
    if (WriteMark(TRIAL_MARK_ADDR, TRIAL_PENDING) != HAL_OK || BootRecord_Save(OTA_STATE_TRIAL_RUNNING) != HAL_OK)
        return HAL_ERROR;

    Boot_Message("OTA trial\r\n");
    OLED_ShowStatus("Trial boot");
    IWDG->KR = 0xCCCCU; // RM0383: 启动、允许配置、装载计数器
    IWDG->KR = 0x5555U;
    IWDG->PR = 6U;      // LSI / 256，标称 32 kHz 时约 12 秒
    IWDG->RLR = 1499U;
    while (IWDG->SR != 0U) {}
    IWDG->KR = 0xAAAAU;
    Bootloader_JumpToApp(APP_FLASH_ADDR);
    return HAL_ERROR;
}

static HAL_StatusTypeDef LoadOrMigrate(void)
{
    uint8_t migrated;
    if (BootRecord_Load(&boot_record, &active_addr) != HAL_OK || ReadMark(MIGRATED_ADDR, &migrated) != HAL_OK)
        return HAL_ERROR;
    if (active_addr == 0U)
    {
        // 首次迁移只接受项目备份内已知的 1.0.3 BIN，不信任上位机提供的候选描述。
        if (migrated == MIGRATED || !AppMatches(9752U, 0x3F6720C5U))
        {
            Boot_Message("OTA baseline unavailable\r\n");
            return HAL_ERROR;
        }
        boot_record.magic = OTA_RECORD_MAGIC;
        boot_record.app_size = 9752U;
        boot_record.app_crc32 = 0x3F6720C5U;
        boot_record.app_version = 0x00010003U;
        if (WriteMark(TRIAL_MARK_ADDR, 0) != HAL_OK || FinishNormal() != HAL_OK)
            return HAL_ERROR;
    }
    if (migrated != MIGRATED)
        return WriteMark(MIGRATED_ADDR, MIGRATED);
    return HAL_OK;
}

// 在开启 UART 接收前完成恢复，确保升级命令无法覆盖候选或备份。
HAL_StatusTypeDef Bootloader_Recover(void)
{
    uint8_t mark;
    if ((FLASH->OPTCR & FLASH_OPTCR_WDG_SW) == 0U || LoadOrMigrate() != HAL_OK)
        return HAL_ERROR;
    switch (boot_record.state)
    {
        case OTA_STATE_NORMAL:
            OLED_ShowStatus("Normal boot");
            return AppMatches(boot_record.app_size, boot_record.app_crc32) ? HAL_OK : HAL_ERROR;
        case OTA_STATE_INSTALLING:
            Boot_Message("OTA installing\r\n");
            OLED_ShowStatus("Installing...");
            if (CheckExternalImage(OTA_BACKUP_ADDR, boot_record.app_size, boot_record.app_crc32) != HAL_OK)
                return HAL_ERROR;
            if (CheckExternalImage(0, boot_record.update_size, boot_record.update_crc32) != HAL_OK ||
                CopyToApp(0, boot_record.update_size, boot_record.update_crc32) != HAL_OK)
                return RestoreApp();
            if (BootRecord_Save(OTA_STATE_TRIAL_READY) != HAL_OK)
                return HAL_ERROR;
            return LaunchTrial();
        case OTA_STATE_TRIAL_READY:
            return LaunchTrial();
        case OTA_STATE_TRIAL_RUNNING:
            if (ReadMark(TRIAL_MARK_ADDR, &mark) != HAL_OK)
                return HAL_ERROR;
            if (mark != TRIAL_CONFIRMED || !AppMatches(boot_record.update_size, boot_record.update_crc32))
                return RestoreApp();
            boot_record.app_size = boot_record.update_size;
            boot_record.app_crc32 = boot_record.update_crc32;
            boot_record.app_version = boot_record.update_version;
            if (FinishNormal() != HAL_OK)
                return HAL_ERROR;
            Boot_Message("OTA confirmed\r\n");
            OLED_ShowStatus("OTA confirmed");
            return HAL_OK;
        case OTA_STATE_RESTORING:
            return RestoreApp();
        default:
            return HAL_ERROR;
    }
}
