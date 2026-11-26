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
#include "bootloader.h"
#include "w25q64.h"
#include "oled.h"
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
OTA_MetaInfo_t incoming_meta = {0};     // 待升级固件 16B 元数据

volatile uint8_t meta_waiting = 0;      // 正在等待接收 16B 元数据
uint32_t meta_start_tick = 0;

uint8_t rx_buffer[256] = {0};           // 串口接收缓冲
uint32_t current_write_addr = 0x000000; // W25Q64 暂存区当前写入偏移

// 0:空闲 1:置升级标志 2:准备收元数据 3:擦暂存区 4:准备安装 5:写分包 6:存元数据
volatile uint8_t cmd_flag = 0;
volatile uint16_t rx_len = 0;

volatile uint8_t uart_error_pending = 0;
volatile uint32_t uart_last_error = 0;

uint32_t led_tick = 0;
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
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  HAL_Delay(20);
  OLED_Init();

   HAL_UART_Transmit(&huart1, (uint8_t *)"Bootloader ready!\r\n", 19, 1000);


  if (Bootloader_Recover() != HAL_OK)
  {
      OLED_ShowStatus("Recovery stopped");
      HAL_UART_Transmit(&huart1, (uint8_t *)"OTA recovery stopped\r\n", sizeof("OTA recovery stopped\r\n") - 1U, 1000);
      Error_Handler();
  }
  uint32_t last_activity = HAL_GetTick();
  uint8_t have_meta = 0;
  uint8_t staging_ready = 0;
  uint32_t last_step = 0;
  OTA_MetaInfo_t current_meta;
  Bootloader_ReadMeta(&current_meta);
  

  if (current_meta.ota_flag == OTA_FLAG_UPDATE)
  {
      current_meta.ota_flag = OTA_FLAG_NORMAL; // 消费一次升级请求，避免超时复位后重复滞留 Boot
      if (Bootloader_WriteMeta(&current_meta) != HAL_OK)
      {
          OLED_ShowStatus("EEPROM error");
          Error_Handler();
      }
      OLED_ShowStatus("OTA READY");
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
    if (uart_error_pending)
    {
        uart_error_pending = 0;

        HAL_UART_AbortReceive(&huart1);
        __HAL_UART_CLEAR_OREFLAG(&huart1);

        meta_waiting = 0;
        cmd_flag = 0;
        rx_len = 0;

        if (HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer,sizeof(rx_buffer)) != HAL_OK)
        {
            uart_error_pending = 1;
        }
        else
        {
            OLED_ShowStatus("UART recovered");
        }
    }

    if ((uint32_t)(HAL_GetTick() - led_tick) >= 500U)
    {
        led_tick = HAL_GetTick();
        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
    }

    if (meta_waiting && (uint32_t)(HAL_GetTick() - meta_start_tick) >= 10000U)
    {
        HAL_UART_AbortReceive(&huart1);
        meta_waiting = 0;

     if (cmd_flag != 6)
     {
         uint8_t ack = 'N';
         OLED_ShowStatus("Meta timeout");
         HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, sizeof(rx_buffer));
         HAL_UART_Transmit(&huart1, &ack, 1, 100);
     }
    }
    if (cmd_flag != 0U)
        last_activity = HAL_GetTick();
    else if ((uint32_t)(HAL_GetTick() - last_activity) >= 60000U)
    {
        OLED_ShowStatus("OTA timeout");
        NVIC_SystemReset(); // 60s 无活动自动重启回原 App
    }

    switch (cmd_flag)
    {
        case 1: // 'U'
            {
                cmd_flag = 0;
                have_meta = 0;
                staging_ready = 0;
                OLED_ShowStatus("OTA READY");
                HAL_UART_Transmit(&huart1, (uint8_t *)"Set Flag OK!\r\n", 14, 100);
                break;
            }

        case 2: // 'H' 握手，切定长接收 16B 元数据
            {
                uint8_t ack = 'h';
                cmd_flag = 0;

                have_meta = 0;
                staging_ready = 0;
                OLED_ShowStatus("Reading metadata");
                meta_start_tick = HAL_GetTick();
                meta_waiting = 1;

                if (HAL_UART_Receive_IT(&huart1, (uint8_t *)&incoming_meta, sizeof(incoming_meta)) != HAL_OK)
                {
                    meta_waiting = 0;
                    ack = 'N';
                    OLED_ShowStatus("Meta RX error");
                    HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, sizeof(rx_buffer));
                }

                HAL_UART_Transmit(&huart1, &ack, 1, 100);
                break;
            }    

        case 3: // 'E' 擦除外部暂存区
            {
                uint8_t ack = 'N';
                HAL_StatusTypeDef result = HAL_OK;

                cmd_flag = 0;

                staging_ready = 0;
                if (!have_meta)
                {
                    OLED_ShowStatus("Metadata needed");
                    HAL_UART_Transmit(&huart1, &ack, 1, 100);
                    break;
                }
                OLED_ShowStatus("Erasing staging");
                for (uint32_t addr = 0; addr < OTA_STAGING_SIZE;
                    addr += 4096U)
                {
                    result = W25Q64_Erase_Sector(addr);

                    if (result != HAL_OK)
                    {
                        break;
                    }
                }

                if (result == HAL_OK)
                {
                    current_write_addr = 0;
                    last_step = 0;
                    OLED_ShowStatus("Receiving 0%");
                    staging_ready = 1;
                    ack = 'E';
                }
                else
                {
                    OLED_ShowStatus("Erase failed");
                }

                HAL_UART_Transmit(&huart1, &ack, 1, 100);
            }
            break;

        case 4: // 'C' 备份旧版并软复位进入安装
            {
                cmd_flag = 0;
                HAL_UART_Transmit(&huart1, (uint8_t *)"OTA managed\r\n", sizeof("OTA managed\r\n") - 1U, 1000);
                if (!staging_ready || current_write_addr != incoming_meta.firmware_size || Bootloader_PrepareInstall(&incoming_meta) != HAL_OK)
                {
                    OLED_ShowStatus("Prepare failed");
                    HAL_UART_Transmit(&huart1, (uint8_t *)"OTA prepare failed\r\n", sizeof("OTA prepare failed\r\n") - 1U, 1000);
                }
                NVIC_SystemReset();
            }
            break;
        case 5: // 写入固件分包
            {
                uint8_t ack = 'N';
                HAL_StatusTypeDef result = HAL_ERROR;

                cmd_flag = 0;

                if (staging_ready && current_write_addr < incoming_meta.firmware_size && rx_len <= incoming_meta.firmware_size - current_write_addr)
                {
                    result = W25Q64_Write_Page(current_write_addr,rx_buffer, rx_len);
                }

                if (result == HAL_OK)
                {
                    current_write_addr += rx_len;
                    uint32_t step = current_write_addr * 10U / incoming_meta.firmware_size;
                    if (step > last_step)
                    {
                        char text[17];
                        snprintf(text, sizeof(text), "Receiving %lu%%", (unsigned long)(step * 10U));
                        OLED_ShowStatus(text);
                        last_step = step;
                    }
                    ack = '0';
                }
                else
                {
                    OLED_ShowStatus("Packet failed");
                }

                if (HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer,sizeof(rx_buffer)) != HAL_OK)
                {
                    uart_error_pending = 1;
                    break;
                }

                HAL_UART_Transmit(&huart1, &ack, 1, 100);
            }
            break;

        case 6: // 校验并保存 16B 元数据
            {
                uint8_t ack = 'N';
                cmd_flag = 0;

                if (incoming_meta.ota_flag == OTA_FLAG_UPDATE && incoming_meta.firmware_size >= 8U && incoming_meta.firmware_size <= OTA_STAGING_SIZE && incoming_meta.firmware_size % 4U == 0U)
                {
                    incoming_meta.ota_flag = OTA_FLAG_NORMAL;
                    HAL_StatusTypeDef result = Bootloader_WriteMeta(&incoming_meta);

                    if (result == HAL_OK)
                    {
                        have_meta = 1;
                        ack = 'H';
                    }
                }

                if (ack == 'H')
                    OLED_ShowStatus("Metadata OK");
                else
                    OLED_ShowStatus("Metadata failed");
                HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, sizeof(rx_buffer));
                HAL_UART_Transmit(&huart1, &ack, 1, 100);
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 200;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
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

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART1)
    {
        if (Size <= 3 && rx_buffer[0] == 'U')
        {
            cmd_flag = 1;
        }
        else if (Size == 1 && rx_buffer[0] == 'H')
        {
            cmd_flag = 2;
            return; // 由主循环切换定长接收
        }
        else if (Size <= 3 && rx_buffer[0] == 'E')
        {
            cmd_flag = 3;
        }
        else if (Size <= 3 && rx_buffer[0] == 'C')
        {
            cmd_flag = 4;
        }
        else if (Size == 1 && rx_buffer[0] == 'V')
        {
            // Boot 阶段忽略版本查询
        }
        else
        {
            rx_len = Size;
            cmd_flag = 5;
            return; // 主循环写完 Flash 后再重新挂接接收
        }

        HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, sizeof(rx_buffer));
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1 && meta_waiting)
    {
        meta_waiting = 0;
        cmd_flag = 6;
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        uart_last_error = huart->ErrorCode;
        uart_error_pending = 1;
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
