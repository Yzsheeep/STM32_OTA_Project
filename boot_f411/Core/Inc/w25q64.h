#ifndef __W25Q64_H
#define __W25Q64_H

#include "main.h"


HAL_StatusTypeDef W25Q64_Erase_Sector(uint32_t addr);
HAL_StatusTypeDef W25Q64_Read_Data(uint32_t addr, uint8_t *buf, uint16_t len);
HAL_StatusTypeDef W25Q64_Write_Page(uint32_t addr, uint8_t *buf, uint16_t len);


#endif
