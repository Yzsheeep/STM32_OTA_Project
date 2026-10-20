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
uint8_t rx_buffer[256] = {0};
uint32_t current_write_addr = 0x000000;
uint8_t cmd_flag = 0;
uint16_t rx_len = 0;
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


  

  uint8_t flag = Bootloader_ReadFlag();
  if (flag == OTA_FLAG_UPDATE)
  {
      // 如果是 0xAA，说明需要升级，停在一楼不走！
      HAL_UART_Transmit(&huart1, (uint8_t *)"Wait for OTA update...\r\n", 24, 1000);
  }
  else
  {
      // 如果不是 0xAA（正常模式），直接尝试跳去二楼！
      // （注：万一二楼是空的，JumpToApp 会打印 No Valid App! 然后乖乖退回一楼继续往下走）
      HAL_UART_Transmit(&huart1, (uint8_t *)"Normal boot, try jump...\r\n", 26, 1000);
      Bootloader_JumpToApp(APP_FLASH_ADDR);
  }

  HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, sizeof(rx_buffer));
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    switch (cmd_flag)
    {
        case 1: // 1号指令：擦除外部 Flash
            printf("Start Erasing W25Q64...\r\n");
            W25Q64_Erase_Sector(0x000000);
            current_write_addr = 0x000000;
            printf("W25Q64 Erase OK!\r\n");
            cmd_flag = 0; // 处理完毕，恢复空闲状态
            break;

        case 2: // 2号指令：分包写入外部 Flash
            W25Q64_Write_Page(current_write_addr, rx_buffer, rx_len);
            current_write_addr += rx_len;
            printf("Write %d bytes OK! Next addr: 0x%06X\r\n", rx_len, current_write_addr);
            cmd_flag = 0;
            break;

        case 3: // 3号指令：读取验证
            {
                uint8_t verify_buf[16] = {0};
                W25Q64_Read_Data(0x000000, verify_buf, 10);
                printf("Read from W25Q64: %s\r\n", verify_buf);
                cmd_flag = 0;
            }
            break;
        case 4: // 4号指令：把 W25Q64 的数据搬迁到内部 Flash
            {
                printf("Step 1: Erasing Internal Flash...\r\n");
                Bootloader_EraseApp(); // 1. 先把片内 App 区擦干净

                printf("Step 2: Copying from W25Q64 to Flash...\r\n");
                uint8_t move_buf[256];
                uint32_t src_addr = 0x000000;         // W25Q64 起点
                uint32_t dst_addr = APP_FLASH_ADDR;   // 内部 Flash 起点 (0x08004000)
                uint32_t total_size = current_write_addr; // 之前写入 W25Q64 的总字节数
                uint32_t copied = 0;

                // 2. 循环搬运：每次搬一小块
                while (copied < total_size)
                {
                    uint16_t chunk = (total_size - copied > 256) ? 256 : (total_size - copied);

                    // 从外部 Flash 读一勺
                    W25Q64_Read_Data(src_addr, move_buf, chunk);

                    // 往内部 Flash 倒一勺
                    Bootloader_WriteFlash(dst_addr, move_buf, chunk);

                    // 两边的地址指针一起往后走
                    src_addr += chunk;
                    dst_addr += chunk;
                    copied   += chunk;
                }

                printf("Copy Done! Total: %lu bytes\r\n", copied);

                // 3. 搬迁完成，清除升级标志并复位跳转
                Bootloader_WriteFlag(OTA_FLAG_NORMAL);
                cmd_flag = 0;

                printf("Rebooting to New App...\r\n");
                HAL_Delay(100);
                NVIC_SystemReset(); // 软复位，重启后自动进入新 App
            }
            break;    
        default:
            // 0 或未定义状态：空闲等待，什么都不做
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
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART1)
    {
        if(Size <= 3 && rx_buffer[0]=='J')
        {
          Bootloader_WriteFlag(OTA_FLAG_NORMAL);
          NVIC_SystemReset();
        }
        else if (Size <= 3 && rx_buffer[0] == 'E') // <--- 新增：收到 'E' 就擦除二楼！
        {
          cmd_flag = 1;
        }
        else if (Size <= 3 && rx_buffer[0] == 'U')
        {
          Bootloader_WriteFlag(OTA_FLAG_UPDATE);
          HAL_UART_Transmit(&huart1, (uint8_t *)"Set Flag 0xAA OK!\r\n", 19, 1000);
        }
        else if (Size <= 3 && rx_buffer[0] == 'R')
        {
          cmd_flag = 3;
        }
        else if (Size <= 3 && rx_buffer[0] == 'C')
        {
            cmd_flag = 4; // 举 4 号旗子：叫 while(1) 开始搬家
        }
        else
        {
          rx_len = Size; // 记住这次收到了几个字节
          cmd_flag = 2;  // 举起 2 号旗子，叫 while(1) 来写 W25Q64！
        }

        HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, sizeof(rx_buffer));
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
