#include "bootloader.h"
#include <string.h>
#include "usart.h"
#include "i2c.h"

uint8_t last_byte = 0; // 临时观察 EEPROM 末字节

// App 入口函数指针类型
typedef void (*pFunction)(void);

// app_addr：App 向量表起点
void Bootloader_JumpToApp(uint32_t app_addr)
{
    // 1. 读取初始栈顶
    uint32_t msp_val = *(volatile uint32_t *)app_addr; // App 初始栈顶地址

    // 2. 仅检查栈顶地址范围
    if (msp_val >= 0x20000000 && msp_val <= 0x20005000)
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

        // 5. 设置栈顶并跳转；C 调用仍有栈风险
        __set_MSP(msp_val);
        Jump_To_Application();
    }
    else
    {
        // 栈顶地址无效
        HAL_UART_Transmit(&huart1, (uint8_t *)"No Valid App!\r\n", 15, 1000);
    }
}

HAL_StatusTypeDef Bootloader_EraseApp(void)
{
    FLASH_EraseInitTypeDef EraseInitStruct; // Flash 擦除参数
    uint32_t PageError = 0; // HAL 输出的擦除失败页地址

    HAL_UART_Transmit(&huart1, (uint8_t *)"Erasing App Flash...\r\n", 22, 1000);

    // 1. 解锁 Flash
    HAL_FLASH_Unlock();

    // 2. 配置擦除范围
    EraseInitStruct.TypeErase   = FLASH_TYPEERASE_PAGES; // 按页擦除
    EraseInitStruct.PageAddress = APP_FLASH_ADDR;        // 起始地址：0x08004000
    EraseInitStruct.NbPages     = APP_FLASH_PAGES;       // 48 页，每页 1 KB

    // 3. 打印擦除结果，未返回状态
    HAL_StatusTypeDef result = HAL_FLASHEx_Erase(&EraseInitStruct, &PageError);

    if (result == HAL_OK)
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)"Erase Success!\r\n", 16, 1000);
    }
    else
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)"Erase Failed!\r\n", 15, 1000);
    }

    // 4. 重新锁上 Flash
    HAL_FLASH_Lock();
    return result;
}

// flash_addr：写入地址；p_data：数据；len：字节数
HAL_StatusTypeDef Bootloader_WriteFlash(uint32_t flash_addr, uint8_t *p_data, uint32_t len)
{
    HAL_StatusTypeDef result = HAL_OK;
    // 1. 解锁 Flash
    HAL_FLASH_Unlock();

    // 2. 按 4 字节写入，尚未处理不足 4 字节的尾部
    // i：本批数据的字节偏移
    for (uint32_t i = 0; i < len; i += 4)
    {
        // 取出 4 字节，需保证对齐和可读范围
        uint32_t word_data = *(uint32_t *)(p_data + i); // 本轮写入的 4 字节数据

        // 写入内部 Flash
        result = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,flash_addr + i,word_data);

        if (result != HAL_OK)
        {
            break; // 本次写入失败，停止后续写入
        }
    }

    // 3. 重新上锁
    HAL_FLASH_Lock();
    return result;
}



// 读取 EEPROM 固件信息到 meta，失败则停机
void Bootloader_ReadMeta(OTA_MetaInfo_t *meta)
{
    uint8_t *data = (uint8_t *)meta; // 按字节填写结构体内存

    for (uint16_t i = 0; i < sizeof(OTA_MetaInfo_t); i++)
    {
        if (HAL_I2C_Mem_Read(&hi2c2, AT24C02_ADDR, OTA_META_MEM_ADDR + i, I2C_MEMADD_SIZE_8BIT, &data[i], 1, 1000) != HAL_OK)
        {
            HAL_UART_Transmit(&huart1, (uint8_t *)"Meta Read Err!\r\n", 16, 1000);
            Error_Handler();
        }
    }
}

// 保存 meta 并读回校验，成功返回 HAL_OK
HAL_StatusTypeDef Bootloader_WriteMeta(OTA_MetaInfo_t *meta)
{
    uint8_t *data = (uint8_t *)meta; // 按字节访问固件信息
    OTA_MetaInfo_t readback = {0}; // EEPROM 读回记录
    HAL_StatusTypeDef result; // HAL 读写返回状态

    // 每页写 8 字节，共两页
    // offset：页偏移，依次为 0、8
    for (uint16_t offset = 0; offset < sizeof(OTA_MetaInfo_t); offset += 8)
    {
        result = HAL_I2C_Mem_Write(&hi2c2, AT24C02_ADDR, OTA_META_MEM_ADDR + offset, I2C_MEMADD_SIZE_8BIT, data + offset, 8, 100);
        if (result != HAL_OK)
        {
            return result;
        }

        // 等待本页写入完成
        uint32_t start = HAL_GetTick(); // 本页等待起始时间（ms）
        while (HAL_I2C_IsDeviceReady(&hi2c2, AT24C02_ADDR, 1, 2) != HAL_OK)
        {
            if ((uint32_t)(HAL_GetTick() - start) >= 20U)
            {
                return HAL_TIMEOUT;
            }
            HAL_Delay(1);
        }
    }

    /* USER CODE BEGIN EEPROM_CHECK */
	uint8_t *read_data = (uint8_t *)&readback; // 按字节填写读回结构体

	for (uint16_t i = 0; i < sizeof(readback); i++)
	{
		result = HAL_I2C_Mem_Read(&hi2c2, AT24C02_ADDR, OTA_META_MEM_ADDR + i, I2C_MEMADD_SIZE_8BIT, read_data + i, 1, 100);
		if (result != HAL_OK)
		{
			return result;
		}
	}
	/* USER CODE END EEPROM_CHECK */
    return memcmp(meta, &readback, sizeof(readback)) == 0 ? HAL_OK : HAL_ERROR;
}
