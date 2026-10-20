#include "bootloader.h"
#include "usart.h"
#include "i2c.h"

// 定义函数指针pFunction
typedef void (*pFunction)(void);

void Bootloader_JumpToApp(uint32_t app_addr)
{
    // 1. 取出二楼开头的第一个字（栈顶地址 MSP）
    uint32_t msp_val = *(volatile uint32_t *)app_addr;

    // 2. 检查栈顶地址是否合法（是否在 0x20000000 开头的 RAM 区内）
    if (msp_val >= 0x20000000 && msp_val <= 0x20005000)
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)"Jumping to App...\r\n", 19, 1000);

        // 3. 关闭所有中断和滴答定时器，干干净净地移交控制权
        __disable_irq();
        SysTick->CTRL = 0;
        SysTick->LOAD = 0;
        SysTick->VAL  = 0;

        // 4. 取出二楼的第二个字（app_addr + 4），即复位入口函数的地址
        uint32_t jump_addr = *(volatile uint32_t *)(app_addr + 4);
        pFunction Jump_To_Application = (pFunction)jump_addr;

        // 5. 设置二楼的栈顶指针，并直接起跳！
        __set_MSP(msp_val);
        Jump_To_Application();
    }
    else
    {
        // 如果二楼还是空的，打印提示
        HAL_UART_Transmit(&huart1, (uint8_t *)"No Valid App!\r\n", 15, 1000);
    }
}

void Bootloader_EraseApp(void)
{
    FLASH_EraseInitTypeDef EraseInitStruct;
    uint32_t PageError = 0;

    HAL_UART_Transmit(&huart1, (uint8_t *)"Erasing App Flash...\r\n", 22, 1000);

    // 1. 解锁 Flash
    HAL_FLASH_Unlock();

    // 2. 填写“拆迁申请表”
    EraseInitStruct.TypeErase   = FLASH_TYPEERASE_PAGES; // 按页擦除
    EraseInitStruct.PageAddress = APP_FLASH_ADDR;        // 起始地址：0x08004000
    EraseInitStruct.NbPages     = APP_FLASH_PAGES;       // 擦除页数：48页

    // 3. 执行擦除，并判断返回值是不是 HAL_OK
    if (HAL_FLASHEx_Erase(&EraseInitStruct, &PageError) == HAL_OK)
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)"Erase Success!\r\n", 16, 1000);
    }
    else
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)"Erase Failed!\r\n", 15, 1000);
    }

    // 4. 重新锁上 Flash
    HAL_FLASH_Lock();
}

void Bootloader_WriteFlash(uint32_t flash_addr, uint8_t *p_data, uint32_t len)
{
    // 1. 解锁 Flash
    HAL_FLASH_Unlock();

    // 2. 循环写入：因为每次写 4 个字节，所以 i 每次加 4！
    for (uint32_t i = 0; i < len; i += 4)
    {
        // 从数组里取出连续的 4 个字节，打包成一个 32 位整数
        uint32_t word_data = *(uint32_t *)(p_data + i);

        // 写进 Flash 对应的地址 (flash_addr + i)
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, flash_addr + i, word_data);
    }

    // 3. 重新上锁
    HAL_FLASH_Lock();
}

uint8_t Bootloader_ReadFlag(void)
{
    uint8_t flag = 0;
    if (HAL_I2C_Mem_Read(&hi2c2, AT24C02_ADDR, OTA_FLAG_MEM_ADDR, I2C_MEMADD_SIZE_8BIT, &flag, 1, 1000) != HAL_OK)
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)"AT24C02 Read Err!\r\n", 19, 1000);
    }
    return flag;
}

/**
  * @brief  往 AT24C02 (hi2c2) 写入升级标志位
  */
void Bootloader_WriteFlag(uint8_t flag)
{
    if (HAL_I2C_Mem_Write(&hi2c2, AT24C02_ADDR, OTA_FLAG_MEM_ADDR, I2C_MEMADD_SIZE_8BIT, &flag, 1, 1000) != HAL_OK)
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)"AT24C02 Write Err!\r\n", 20, 1000);
    }
    for (volatile uint32_t i = 0; i < 72000; i++);
}
