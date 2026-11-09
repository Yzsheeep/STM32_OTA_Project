#include "bootloader.h"
#include "i2c.h"
#include "usart.h"
#include <string.h>

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
            Error_Handler(); // 与原实现一致：读取失败时停止启动流程
        }
    }
}
/* USER CODE END ReadMeta */

/* 沿用原来的两页写入、等待写完成、逐字节读回和比较。 */
HAL_StatusTypeDef Bootloader_WriteMeta(OTA_MetaInfo_t *meta)
{
    uint8_t *data = (uint8_t *)meta;
    OTA_MetaInfo_t readback = {0};
    HAL_StatusTypeDef result;

    for (uint16_t offset = 0; offset < sizeof(OTA_MetaInfo_t); offset += 8)
    {
        result = HAL_I2C_Mem_Write(&hi2c2, AT24C02_ADDR,OTA_META_MEM_ADDR + offset,I2C_MEMADD_SIZE_8BIT, data + offset, 8, 100); //AT24C02 的页写入一次最多处理同一页内的 8 字节
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
