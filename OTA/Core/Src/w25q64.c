#include "w25q64.h"
#include "spi.h"

// /**
//   * @brief  内部辅助函数：等待 W25Q64 内部擦写完成（查 BUSY 位）
//   */
// static void W25Q64_WaitBusy(void)
// {
//     uint8_t cmd = W25X_ReadStatusReg1;
//     uint8_t status = 0;

//     W25Q64_CS_LOW();
//     HAL_SPI_Transmit(&hspi1, &cmd, 1, 1000);
//     do
//     {
//         HAL_SPI_Receive(&hspi1, &status, 1, 1000);
//     } while ((status & 0x01) == 0x01); // 最低位 BUSY 为 1 说明还在忙
//     W25Q64_CS_HIGH();
// }

// /**
//   * @brief  内部辅助函数：开启写使能（每次擦除或写入前必须调用！）
//   */
// static void W25Q64_WriteEnable(void)
// {
//     uint8_t cmd = W25X_WriteEnable;
//     W25Q64_CS_LOW();
//     HAL_SPI_Transmit(&hspi1, &cmd, 1, 1000);
//     W25Q64_CS_HIGH();
// }

// /**
//   * @brief  1. 读取芯片 ID（正常应返回 0xEF16）
//   */
// uint16_t W25Q64_ReadID(void)
// {
//     uint8_t tx_buf[4] = {W25X_ManufactDeviceID, 0x00, 0x00, 0x00};
//     uint8_t rx_buf[2] = {0};

//     W25Q64_CS_LOW();
//     HAL_SPI_Transmit(&hspi1, tx_buf, 4, 1000); // 发送 0x90 + 24位地址0
//     HAL_SPI_Receive(&hspi1, rx_buf, 2, 1000);  // 接收 2 字节 ID
//     W25Q64_CS_HIGH();

//     return (rx_buf[0] << 8) | rx_buf[1];
// }

// /**
//   * @brief  2. 擦除一个扇区（4KB = 4096 字节）
//   * @param  sector_addr: 扇区起始地址（如 0x000000, 0x001000, 0x002000...）
//   */
// void W25Q64_EraseSector(uint32_t sector_addr)
// {
//     uint8_t cmd[4];
//     cmd[0] = W25X_SectorErase;
//     cmd[1] = (sector_addr >> 16) & 0xFF; // 24位地址高8位
//     cmd[2] = (sector_addr >> 8) & 0xFF;  // 24位地址中8位
//     cmd[3] = sector_addr & 0xFF;         // 24位地址低8位

//     W25Q64_WriteEnable();
//     W25Q64_WaitBusy();

//     W25Q64_CS_LOW();
//     HAL_SPI_Transmit(&hspi1, cmd, 4, 1000);
//     W25Q64_CS_HIGH();

//     W25Q64_WaitBusy(); // 等待擦除完成（约几十毫秒）
// }

// /**
//   * @brief  3. 页写入（单次最多写 256 字节，恰好完美匹配串口每包 256 字节！）
//   */
// void W25Q64_WritePage(uint32_t addr, uint8_t *p_data, uint16_t len)
// {
//     uint8_t cmd[4];
//     cmd[0] = W25X_PageProgram;
//     cmd[1] = (addr >> 16) & 0xFF;
//     cmd[2] = (addr >> 8) & 0xFF;
//     cmd[3] = addr & 0xFF;

//     W25Q64_WriteEnable();

//     W25Q64_CS_LOW();
//     HAL_SPI_Transmit(&hspi1, cmd, 4, 1000);
//     HAL_SPI_Transmit(&hspi1, p_data, len, 1000);
//     W25Q64_CS_HIGH();

//     W25Q64_WaitBusy(); // 等待写入完成（仅需 1~2 毫秒！）
// }

// /**
//   * @brief  4. 读取任意长度数据（一楼用来从快递柜取件）
//   */
// void W25Q64_ReadBuffer(uint32_t addr, uint8_t *p_buf, uint32_t len)
// {
//     uint8_t cmd[4];
//     cmd[0] = W25X_ReadData;
//     cmd[1] = (addr >> 16) & 0xFF;
//     cmd[2] = (addr >> 8) & 0xFF;
//     cmd[3] = addr & 0xFF;

//     W25Q64_CS_LOW();
//     HAL_SPI_Transmit(&hspi1, cmd, 4, 1000);
//     HAL_SPI_Receive(&hspi1, p_buf, len, 1000);
//     W25Q64_CS_HIGH();
// }
// 1. 擦除扇区函数：传入要擦除的扇区地址 addr
void W25Q64_Erase_Sector(uint32_t addr)
{
    uint8_t write_en = 0x06;
    uint8_t erase_cmd[4] = {0x20, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};

      // 1. 擦除前必须先发一次 0x06 解锁！
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi1, &write_en, 1, 1000);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    
    // 2. 发送 0x20 擦除指令 + 地址
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi1, erase_cmd, 4, 1000); 
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    
    // 3. 必须等待 100ms 让芯片把扇区彻底擦干净！
    HAL_Delay(100); 
}

// 2. 写数据函数：往 addr 地址写入 buf 数组里的 len 个字节
void W25Q64_Write_Page(uint32_t addr, uint8_t *buf, uint16_t len)
{
    uint8_t write_en = 0x06;
    uint8_t write_cmd[4] = {0x02, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};

    // 1. 擦除完芯片会自动重新锁上，所以写数据前必须再发一次 0x06 解锁！
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi1, &write_en, 1, 1000);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    // 2. 发送 0x02 写指令 + 地址 + 要写入的数据
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi1, write_cmd, 4, 1000);
    HAL_SPI_Transmit(&hspi1, buf, len, 1000); 
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    // 3. 等待 10ms 烧写完成
    HAL_Delay(10); 
    
}

// 3. 读数据函数：从 addr 地址读出 len 个字节存进 buf 数组里
void W25Q64_Read_Data(uint32_t addr, uint8_t *buf, uint16_t len)
{
    uint8_t read_cmd[4] = {0x03, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&hspi1, read_cmd, 4, 1000);
    HAL_SPI_Receive(&hspi1, buf, len, 1000);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    
}
