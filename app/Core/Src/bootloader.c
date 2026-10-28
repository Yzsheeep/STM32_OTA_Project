#include "bootloader.h"
#include "usart.h"

// 定义无参数、无返回值的函数指针类型，用于调用 APP 复位入口
typedef void (*pFunction)(void);

void Bootloader_JumpToApp(uint32_t app_addr)
{
    // 1. 读取 APP 向量表首项：APP 启动时的初始 MSP 值
    uint32_t msp_val = *(volatile uint32_t *)app_addr;

    // 2. 仅检查 MSP 数值是否在 20 KB SRAM 范围内（含末端栈顶）；未检查对齐、复位入口或镜像完整性
    if (msp_val >= 0x20000000 && msp_val <= 0x20005000)
    {
        HAL_UART_Transmit(&huart1, (uint8_t *)"Jumping to App...\r\n", 19, 1000);

        // 3. 用 PRIMASK 屏蔽普通可屏蔽中断并停止 SysTick；未清理 NVIC 使能、挂起位或其他外设状态
        __disable_irq();
        SysTick->CTRL = 0;
        SysTick->LOAD = 0;
        SysTick->VAL  = 0;

        // 4. 读取 APP 向量表第二项：Reset_Handler 入口地址，并转换为函数指针
        uint32_t jump_addr = *(volatile uint32_t *)(app_addr + 4);
        pFunction Jump_To_Application = (pFunction)jump_addr;

        // 5. 设置 APP 的 MSP 后调用复位入口；修改 MSP 后普通 C 调用仍有栈访问风险，交接实现需单独验证
        __set_MSP(msp_val);
        Jump_To_Application();
    }
    else
    {
        // MSP 未通过上述范围检查时打印提示，不能据此区分空镜像与其他无效内容
        HAL_UART_Transmit(&huart1, (uint8_t *)"No Valid App!\r\n", 15, 1000);
    }
}

