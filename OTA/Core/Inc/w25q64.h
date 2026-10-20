#ifndef __W25Q64_H
#define __W25Q64_H

#include "main.h"

// // --- 片选引脚 PA4 宏定义（拉低选中，拉高释放） ---
// #define W25Q64_CS_LOW()   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET)
// #define W25Q64_CS_HIGH()  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET)

// // --- W25Q64 常用硬件指令表 ---
// #define W25X_WriteEnable      0x06  // 写使能
// #define W25X_ReadStatusReg1   0x05  // 读状态寄存器1（查忙）
// #define W25X_ReadData         0x03  // 读数据
// #define W25X_PageProgram      0x02  // 页编程（每次最多写 256 字节）
// #define W25X_SectorErase      0x20  // 扇区擦除（每次擦 4KB = 4096 字节）
// #define W25X_ManufactDeviceID 0x90  // 读取芯片 ID

// // --- 核心函数声明 ---
// uint16_t W25Q64_ReadID(void);
// void W25Q64_EraseSector(uint32_t sector_addr);
// void W25Q64_WritePage(uint32_t addr, uint8_t *p_data, uint16_t len);
// void W25Q64_ReadBuffer(uint32_t addr, uint8_t *p_buf, uint32_t len);
void W25Q64_Erase_Sector(uint32_t addr);
void W25Q64_Read_Data(uint32_t addr, uint8_t *buf, uint16_t len);
void W25Q64_Write_Page(uint32_t addr, uint8_t *buf, uint16_t len);

#endif
