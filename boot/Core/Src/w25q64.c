#include "w25q64.h"
#include "spi.h"

/* USER CODE BEGIN PV */
volatile HAL_StatusTypeDef write_status[3]; // 三次发送结果
volatile uint16_t write_left; // 数据发送剩余字节数
/* USER CODE END PV */

// 擦除 4 KB 扇区，尚未检查 BUSY
// addr：W25Q64 扇区地址
void W25Q64_Erase_Sector(uint32_t addr)
{
    uint8_t write_en = 0x06; // 写使能指令
    uint8_t erase_cmd[4] = {0x20, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr}; // 擦除指令 + 24 位地址

      // 1. 发送写使能
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi1, &write_en, 1, 1000);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    
    // 2. 发送擦除指令和地址
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi1, erase_cmd, 4, 1000); 
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    
    // 3. 固定等待，尚未轮询 BUSY
    HAL_Delay(100); 
}

// 页编程，调用者需保证不跨 256 字节页
// addr：写入地址；buf：数据；len：字节数
void W25Q64_Write_Page(uint32_t addr, uint8_t *buf, uint16_t len)
{
    uint8_t write_en = 0x06; // 写使能指令
    uint8_t write_cmd[4] = {0x02, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr}; // 页编程指令 + 24 位地址

    // 1. 重新发送写使能
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
	write_status[0] = HAL_SPI_Transmit(&hspi1, &write_en, 1, 1000);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    // 2. 发送编程指令、地址和数据
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    write_status[1] = HAL_SPI_Transmit(&hspi1, write_cmd, 4, 1000);
	write_status[2] = HAL_SPI_Transmit(&hspi1, buf, len, 1000);
	write_left = hspi1.TxXferCount; // 记录还有多少字节没发完
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    // 3. 固定等待，尚未检查编程结果
    HAL_Delay(10); 
    
}

// 读取外部 Flash，尚未检查通信结果
// addr：读取地址；buf：接收缓冲区；len：字节数
void W25Q64_Read_Data(uint32_t addr, uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        uint32_t read_addr = addr + i; // 依次读取每个地址
        uint8_t read_cmd[4] = {0x03, (uint8_t)(read_addr >> 16), (uint8_t)(read_addr >> 8), (uint8_t)read_addr};

        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
        HAL_SPI_Transmit(&hspi1, read_cmd, 4, 1000);
        HAL_SPI_Receive(&hspi1, &buf[i], 1, 1000); // 存入数组第 i 个字节
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    }
}