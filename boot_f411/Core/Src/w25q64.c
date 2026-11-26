#include "w25q64.h"
#include "spi.h"

// 读状态寄存器1 (0x05)
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

    *status = rx[1];
    return HAL_OK;
}

// 轮询等待 BUSY 位清零
static HAL_StatusTypeDef W25Q64_WaitReady(uint32_t timeout_ms)
{
    uint8_t status;
    HAL_StatusTypeDef result;
    uint32_t start = HAL_GetTick();

    while (1)
    {
        result = W25Q64_ReadStatus(&status);

        if (result != HAL_OK)
        {
            return result;
        }

        if ((status & 0x01U) == 0U)
        {
            return HAL_OK; // BUSY 清零
        }

        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms)
        {
            return HAL_TIMEOUT;
        }
    }
}

// 擦除 4KB 扇区并做 0xFF 空检
HAL_StatusTypeDef W25Q64_Erase_Sector(uint32_t addr)
{
    uint8_t write_en = 0x06;
    uint8_t erase_cmd[4] = {0x20, (uint8_t)(addr >> 16),(uint8_t)(addr >> 8), (uint8_t)addr};
    uint8_t status;
    HAL_StatusTypeDef result;
    uint8_t check_buf[32];

    if (addr >= 0x800000U || addr % 4096U != 0U)
    {
        return HAL_ERROR;
    }

    result = W25Q64_WaitReady(500);
    if (result != HAL_OK)
    {
        return result;
    }

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    result = HAL_SPI_Transmit(&hspi1, &write_en, 1, 1000);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    if (result != HAL_OK)
    {
        return result;
    }

    // 确认 WEL=1, BUSY=0
    result = W25Q64_ReadStatus(&status);
    if (result != HAL_OK)
    {
        return result;
    }

    if ((status & 0x03U) != 0x02U)
    {
        return HAL_ERROR;
    }

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

    // 全扇区读回空检
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
                return HAL_ERROR;
            }
        }
    }

    return HAL_OK;
}

// 页编程（不跨 256B 页边界），写后读回校验
HAL_StatusTypeDef W25Q64_Write_Page(uint32_t addr, uint8_t *buf, uint16_t len)
{
    uint8_t write_en = 0x06;
    uint8_t write_cmd[4] = {0x02, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    HAL_StatusTypeDef result;
    uint8_t status; 
    static uint8_t readback[256];
    if (buf == NULL || len == 0U || len > sizeof(readback))
    {
        return HAL_ERROR;
    }
    if (addr >= 0x800000U || len > 0x800000U - addr)
    {
        return HAL_ERROR;
    }

    if (len > 256U - (addr % 256U))
    {
        return HAL_ERROR; // 禁止跨页
    }

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    result = HAL_SPI_Transmit(&hspi1, &write_en, 1, 1000);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    if (result != HAL_OK)
    {
        return result;
    }
   
    result = W25Q64_ReadStatus(&status);

    if (result != HAL_OK)
    {
        return result;
    }
    if ((status & 0x03U) != 0x02U)
    {
        return HAL_ERROR;
    }

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

    result = W25Q64_WaitReady(20);
    if (result != HAL_OK)
    {
        return result;
    }

    // 读回比对
    result = W25Q64_Read_Data(addr, readback, len);

    if (result != HAL_OK)
    {
        return result;
    }

    for (uint16_t i = 0; i < len; i++)
    {
        if (readback[i] != buf[i])
        {
            return HAL_ERROR;
        }
    }

    return HAL_OK;
    
}

HAL_StatusTypeDef W25Q64_Read_Data(uint32_t addr, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef result;
    for (uint16_t i = 0; i < len; i++)
    {
        uint32_t read_addr = addr + i;
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
    return HAL_OK;
}


