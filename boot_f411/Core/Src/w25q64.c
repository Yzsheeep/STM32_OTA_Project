#include "w25q64.h"
#include "spi.h"

/* USER CODE BEGIN PV */
/* USER CODE END PV */

static HAL_StatusTypeDef W25Q64_ReadStatus(uint8_t *status)
{
    uint8_t tx[2] = {0x05, 0x00};
    uint8_t rx[2] = {0};
    HAL_StatusTypeDef result;

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);

    result = HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2, 1000);

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    if (result != HAL_OK)
    {
        return result;
    }

    *status = rx[1]; // 把状态字节写到调用者提供的地址
    return HAL_OK;
}

static HAL_StatusTypeDef W25Q64_WaitReady(uint32_t timeout_ms)
{
    uint8_t status;
    HAL_StatusTypeDef result;
    uint32_t start = HAL_GetTick(); // 记录开始等待的时间

    while (1)
    {
        result = W25Q64_ReadStatus(&status);

        if (result != HAL_OK)
        {
            return result; // 连状态都没读成功，停止等待
        }

        if ((status & 0x01U) == 0U)
        {
            return HAL_OK; // BUSY=0，Flash 已空闲
        }

        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
        {
            return HAL_TIMEOUT; // 持续忙，超过等待预算
        }
    }
}

// 擦除 4 KB 扇区
HAL_StatusTypeDef W25Q64_Erase_Sector(uint32_t addr)
{
    uint8_t write_en = 0x06;
    uint8_t erase_cmd[4] = {0x20, (uint8_t)(addr >> 16),(uint8_t)(addr >> 8), (uint8_t)addr};
    uint8_t status;
    HAL_StatusTypeDef result;
    uint8_t check_buf[32]; // 每次读回32字节，反复使用这块RAM

    if (addr >= 0x800000U || addr % 4096U != 0U)
    {
        return HAL_ERROR; // 地址必须有效，且位于4KB扇区起点
    }

    result = W25Q64_WaitReady(500);
    if (result != HAL_OK)
    {
        return result;
    }

    // 1. 发送写使能
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    result = HAL_SPI_Transmit(&hspi1, &write_en, 1, 1000);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    if (result != HAL_OK)
    {
        return result;
    }

    // 2. 确认 WEL=1、BUSY=0
    result = W25Q64_ReadStatus(&status);
    if (result != HAL_OK)
    {
        return result;
    }

    if ((status & 0x03U) != 0x02U)
    {
        return HAL_ERROR;
    }

    // 3. 发送擦除命令和地址
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    result = HAL_SPI_Transmit(&hspi1, erase_cmd, 4, 1000);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    if (result != HAL_OK)
    {
        return result;
    }

    result = W25Q64_WaitReady(500);

    if (result != HAL_OK)
    {
        return result;
    }

    // 遍历整个4KB扇区
    for (uint16_t offset = 0; offset < 4096U;
        offset += sizeof(check_buf))
    {
        result = W25Q64_Read_Data(addr + offset,
                                check_buf, sizeof(check_buf));

        if (result != HAL_OK)
        {
            return result;
        }

        for (uint16_t i = 0; i < sizeof(check_buf); i++)
        {
            if (check_buf[i] != 0xFFU)
            {
                return HAL_ERROR; // 发现未擦干净的字节
            }
        }
    }

    return HAL_OK;
}

// 页编程，调用者需保证不跨 256 字节页
// addr：写入地址；buf：数据；len：字节数
HAL_StatusTypeDef W25Q64_Write_Page(uint32_t addr, uint8_t *buf, uint16_t len)
{
    uint8_t write_en = 0x06; // 写使能指令
    uint8_t write_cmd[4] = {0x02, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr}; // 页编程指令 + 24 位地址
    HAL_StatusTypeDef result;
    uint8_t status; 
    static uint8_t readback[256]; // 固定保留 256 字节 RAM，反复用于读回校验
    if (buf == NULL || len == 0U || len > sizeof(readback))
    {
        return HAL_ERROR; // 拒绝空指针、空数据或超出读回数组容量的数据
    }
    if (addr >= 0x800000U || len > 0x800000U - addr)
    {
        return HAL_ERROR; // 超出 W25Q64 的 8MB 地址范围
    }

    if (len > 256U - (addr % 256U))
    {
        return HAL_ERROR; // 超出当前页剩余空间
    }
    // 1. 重新发送写使能
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    result = HAL_SPI_Transmit(&hspi1, &write_en, 1, 1000);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    if (result != HAL_OK)
    {
        return result; // 发送失败，退出函数，并把失败结果交给调用者
    }
   
    result = W25Q64_ReadStatus(&status);

    if (result != HAL_OK)
    {
        return result;
    }
    if ((status & 0x03U) != 0x02U)
    {
        return HAL_ERROR; // 写入许可未开启，或芯片仍忙
    }
    // 2. 发送编程指令、地址和数据
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    result = HAL_SPI_Transmit(&hspi1, write_cmd, 4, 1000);
    if (result == HAL_OK)
    {
        result = HAL_SPI_Transmit(&hspi1, buf, len, 1000);
    }
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    if (result != HAL_OK)
    {
        return result;
    }
    // 3. 固定等待，尚未检查编程结果
    result = W25Q64_WaitReady(20);
    if (result != HAL_OK)
    {
        return result;
    }

    // 从刚才写入的 Flash 地址，读回相同数量的字节
    result = W25Q64_Read_Data(addr, readback, len);

    if (result != HAL_OK)
    {
        return result;
    }

    for (uint16_t i = 0; i < len; i++)
    {
        if (readback[i] != buf[i])
        {
            return HAL_ERROR; // 有一个字节不一致，就判定写入失败
        }
    }

    return HAL_OK;
    
}

// 读取外部 Flash，尚未检查通信结果
// addr：读取地址；buf：接收缓冲区；len：字节数
HAL_StatusTypeDef W25Q64_Read_Data(uint32_t addr, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef result;
    for (uint16_t i = 0; i < len; i++)
    {
        uint32_t read_addr = addr + i; // 依次读取每个地址
        uint8_t read_cmd[4] = {0x03, (uint8_t)(read_addr >> 16), (uint8_t)(read_addr >> 8), (uint8_t)read_addr};

        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);

        result = HAL_SPI_Transmit(&hspi1, read_cmd, 4, 1000);

        if (result == HAL_OK)
        {
            result = HAL_SPI_Receive(&hspi1, &buf[i], 1, 1000);
        }

        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

        if (result != HAL_OK)
        {
            return result;
        }
    }
    return HAL_OK; // 所有字节都读取成功
}


