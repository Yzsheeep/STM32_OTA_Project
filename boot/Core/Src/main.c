/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "i2c.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include "bootloader.h"
#include "w25q64.h"
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
OTA_MetaInfo_t incoming_meta = {0}; // 上位机发来的固件信息（16 字节）

volatile uint8_t meta_waiting = 0; // 固件信息接收中：1=是，0=否
uint32_t meta_start_tick = 0; // 固件信息接收起始时间（ms）

uint8_t rx_buffer[256] = {0}; // 串口接收缓冲区
uint32_t current_write_addr = 0x000000; // W25Q64 下次写入地址

// 任务编号：0=空闲，1=擦外部 Flash，2=写数据，3=接收固件信息
// 4=搬运固件，5=保存固件信息，6=设置升级标志
volatile uint8_t cmd_flag = 0; // 待执行任务，由串口回调设置
volatile uint16_t rx_len = 0; // 本次接收长度（字节）


/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_I2C2_Init();
  MX_USART1_UART_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  HAL_UART_Transmit(&huart1, (uint8_t *)"Bootloader ready!\r\n", 19, 1000);


  OTA_MetaInfo_t current_meta; // 启动时读取的 EEPROM 固件信息
  Bootloader_ReadMeta(&current_meta);
  

  if (current_meta.ota_flag == OTA_FLAG_UPDATE)
  {
      HAL_UART_Transmit(&huart1, (uint8_t *)"Wait for OTA update...\r\n", 24, 1000);
  }
  else
  {
      HAL_UART_Transmit(&huart1, (uint8_t *)"Normal boot, try jump...\r\n", 26, 1000);
      Bootloader_JumpToApp(APP_FLASH_ADDR);
  }

  HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, sizeof(rx_buffer));
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    if (meta_waiting && (uint32_t)(HAL_GetTick() - meta_start_tick) >= 10000U)
    {
        HAL_UART_AbortReceive(&huart1);
        meta_waiting = 0;

     if (cmd_flag != 5)
     {
         uint8_t ack = 'N'; // 失败应答
         HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, sizeof(rx_buffer));
         HAL_UART_Transmit(&huart1, &ack, 1, 100);
     }
    }
    switch (cmd_flag)
    {
        case 1: // 擦除外部 Flash
            {
                // 固定擦除 16 KB，尚未按固件长度计算
                // addr：扇区地址，步长 4 KB
                for (uint32_t addr = 0; addr < 0x004000; addr += 0x1000)
                {
                    W25Q64_Erase_Sector(addr);
                }
                current_write_addr = 0x000000;

                // 擦除调用结束后应答，未校验结果
                uint8_t ack = 'E'; // 擦除流程应答
                HAL_UART_Transmit(&huart1, &ack, 1, 100);
                cmd_flag = 0;
            }
            break;

        case 2: // 写入收到的固件数据
            {
                W25Q64_Write_Page(current_write_addr, rx_buffer, rx_len);
                current_write_addr += rx_len;
                
                // 写入调用结束后应答，未读回校验
                uint8_t ack = '0'; // 数据应答：字符 '0'
                HAL_UART_Transmit(&huart1, &ack, 1, 100);
                cmd_flag = 0;
            }
            break;

        case 3:
            {
                uint8_t ack = 'h'; // 小写 h：准备接收固件信息
                cmd_flag = 0;

                meta_start_tick = HAL_GetTick();
                meta_waiting = 1;

                if (HAL_UART_Receive_IT(&huart1, (uint8_t *)&incoming_meta, sizeof(incoming_meta)) != HAL_OK)
                {
                    meta_waiting = 0;
                    ack = 'N';
                    HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, sizeof(rx_buffer));
                }

                HAL_UART_Transmit(&huart1, &ack, 1, 100);
                break;
            }    

        case 4: // 将外部固件搬运到 App 分区
            {
                OTA_MetaInfo_t meta; // EEPROM 固件信息
                Bootloader_ReadMeta(&meta);

                // 仅检查长度，尚未校验 CRC
                if (meta.firmware_size == 0 || meta.firmware_size > (48 * 1024))
                {
                    cmd_flag = 0;
                    break;
                }
                uint8_t move_buf[256];                      // 每次读回最多256字节
                uint32_t total_size = meta.firmware_size;   // 固件总字节数
                uint32_t check_addr = 0;                    // 外部Flash读取地址
                uint32_t crc = 0xFFFFFFFFU;                 // CRC32初始值
               while (check_addr < total_size)
                {
                    uint16_t chunk = 256; // 默认读取256字节

                    if (total_size - check_addr < 256)
                    {
                        chunk = total_size - check_addr; // 最后一块只读剩余字节
                    }

                    W25Q64_Read_Data(check_addr, move_buf, chunk);

                    for (uint16_t i = 0; i < chunk; i++)
                    {
                        crc ^= move_buf[i]; // 合入当前字节

                        for (uint8_t bit = 0; bit < 8; bit++)
                        {
                            if ((crc & 1U) != 0)
                            {
                                crc = (crc >> 1) ^ 0xEDB88320U;
                            }
                            else
                            {
                                crc >>= 1;
                            }
                        }
                    }

                    check_addr += chunk; // 下一块的起始地址
                }
                crc ^= 0xFFFFFFFFU; // 整份固件处理完，最终取反

                if (crc != meta.firmware_crc32)
                {
                    HAL_UART_Transmit(&huart1, (uint8_t *)"CRC check failed\r\n", 18, 1000);
                    cmd_flag = 0;
                    break; // 退出当前case，不执行下面的擦除
                }

                if (Bootloader_EraseApp() != HAL_OK)
                {
                    cmd_flag = 0;
                    break; // 擦除失败，退出当前 case，不继续搬运
                }

                
                uint32_t src_addr = 0x000000; // W25Q64 读取地址
                uint32_t dst_addr = APP_FLASH_ADDR; // 内部 Flash 写入地址
                 // 固件总长度（字节）
                uint32_t copied = 0; // 累计搬运字节数，未校验结果

                while (copied < total_size)
                {
                    uint16_t chunk = (total_size - copied > 256) ? 256 : (total_size - copied); // 本轮搬运字节数，最多 256
                    W25Q64_Read_Data(src_addr, move_buf, chunk);
                    Bootloader_WriteFlash(dst_addr, move_buf, chunk);
                    src_addr += chunk;
                    dst_addr += chunk;
                    copied   += chunk;
                }
                crc = 0xFFFFFFFFU; // 开始新的 CRC 计算，重新设置初始值
                const uint8_t *app_data = (const uint8_t *)APP_FLASH_ADDR; // 按字节读取内部 APP
                for (uint32_t i = 0; i < total_size; i++)
                {
                    crc ^= app_data[i]; // 读取内部 APP 的一个字节，合入 CRC

                    for (uint8_t bit = 0; bit < 8; bit++)
                    {
                        if ((crc & 1U) != 0)
                        {
                            crc = (crc >> 1) ^ 0xEDB88320U;
                        }
                        else
                        {
                            crc >>= 1;
                        }
                    }
                }

                crc ^= 0xFFFFFFFFU; // 全部字节处理完，最终取反
                if (crc != meta.firmware_crc32)
                {
                    HAL_UART_Transmit(&huart1, (uint8_t *)"APP CRC failed\r\n", 16, 1000);
                    cmd_flag = 0;
                    break; // 退出当前 case，不清升级标志，也不执行后面的复位
                }
                // 搬运后清标志
                meta.ota_flag = OTA_FLAG_NORMAL;
                Bootloader_WriteMeta(&meta);

                cmd_flag = 0;
                HAL_Delay(100);
                NVIC_SystemReset();
            }
            break;
            case 5:
            {
                uint8_t ack = 'N'; // 失败应答
                cmd_flag = 0;

                

                if (incoming_meta.ota_flag == OTA_FLAG_UPDATE && incoming_meta.firmware_size >= 8U && incoming_meta.firmware_size <= 48U * 1024U)
                {
                    HAL_StatusTypeDef result = Bootloader_WriteMeta(&incoming_meta); // 保存及读回校验结果

                    if (result == HAL_OK)
                    {
                        ack = 'H';
                    }
                }

                HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, sizeof(rx_buffer));
                HAL_UART_Transmit(&huart1, &ack, 1, 100);
                break;
            }
            case 6:
            {
                OTA_MetaInfo_t meta = {0}; // 读取原记录，仅修改升级标志
                cmd_flag = 0;

                Bootloader_ReadMeta(&meta);
                meta.ota_flag = OTA_FLAG_UPDATE;
                Bootloader_WriteMeta(&meta);

                HAL_UART_Transmit(&huart1, (uint8_t *)"Set Flag OK!\r\n", 14, 100);
                break;
            }

        default:
            break;
    }
   
    
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
// huart：串口句柄；Size：本次接收字节数
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
        if (huart->Instance == USART1)
        {
            if (Size == 1 && rx_buffer[0] == 'H')
        {
            cmd_flag = 3;
            return;
        }
        else if (Size <= 3 && rx_buffer[0] == 'U')
        {
            cmd_flag = 6;
        }
        else if (Size <= 3 && rx_buffer[0] == 'E')
        {
            cmd_flag = 1; // 提交擦除任务
        }
        else if (Size <= 3 && rx_buffer[0] == 'C')
        {
            cmd_flag = 4; // 提交搬运任务
        }
        else if (Size == 1 && rx_buffer[0] == 'V')
        {
            // 收到版本查询，不做任何操作
        }
        else
        {
            rx_len = Size; // 本次接收长度，可能不足一包
            cmd_flag = 2;  // 提交写入任务，缓冲区仍被接收复用
        }

        HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, sizeof(rx_buffer));
        }
}
// 固件信息接收完成，提交保存任务
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1 && meta_waiting)
    {
        meta_waiting = 0;
        cmd_flag = 5;
    }
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
