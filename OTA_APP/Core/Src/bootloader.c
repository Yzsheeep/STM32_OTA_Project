#include "bootloader.h"
#include "usart.h"

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
