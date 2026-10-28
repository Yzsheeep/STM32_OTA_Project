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
uint8_t app_rx_buf[10] = {0}; // App 串口接收缓冲区
volatile uint8_t need_reboot_to_ota = 0; // 升级请求：回调置 1，主循环处理
volatile uint8_t need_report_version = 0;
uint8_t app_version_msg[] = "APP_VERSION=1.0.2\r\n";
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void App_TriggerOTA_Reboot(void);
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
	SCB->VTOR = FLASH_BASE | 0x4000; // App 中断向量表地址
	__enable_irq();					// 开启可屏蔽中断
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
  HAL_UART_Transmit(&huart1, (uint8_t *)"Hello! I am App in 2nd Floor!\r\n", 31, 1000);

  HAL_UARTEx_ReceiveToIdle_IT(&huart1, app_rx_buf, sizeof(app_rx_buf));
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    if (need_reboot_to_ota)
    {
      need_reboot_to_ota = 0;
      App_TriggerOTA_Reboot();
    }
    if (need_report_version)
    {
        need_report_version = 0; // 清除请求，避免每轮重复发送

        HAL_UART_Transmit(&huart1,app_version_msg,sizeof(app_version_msg) - 1U,1000);
    }
	  HAL_GPIO_TogglePin(GPIOC,GPIO_PIN_13);
	  HAL_Delay(1000);
   
    
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
void App_TriggerOTA_Reboot(void)
{
    uint8_t flag[4] = {0xAA, 0x55, 0xAA, 0x55}; // 升级标志 0x55AA55AA 的小端字节
    uint8_t readback[4] = {0}; // EEPROM 读回值，用于核对

    /* 1. 写入升级标志 */
    if (HAL_I2C_Mem_Write(&hi2c2,0xA0,0x00,I2C_MEMADD_SIZE_8BIT,flag,sizeof(flag),100) != HAL_OK)
    {
        printf("OTA flag write failed\r\n");
        return;
    }

    /* 2. 等待 EEPROM 写入完成 */
    uint32_t start = HAL_GetTick(); // 等待起始时间（ms）
    while (HAL_I2C_IsDeviceReady(&hi2c2, 0xA0, 1, 2) != HAL_OK)
    {
        if ((uint32_t)(HAL_GetTick() - start) >= 20U)
        {
            printf("EEPROM not ready\r\n");
            return;
        }
        HAL_Delay(1);
    }

    /* 3. 按字节读回并核对标志 */
    for (uint16_t i = 0; i < sizeof(readback); i++)
    {
        // EEPROM 地址 i 的字节，存到 readback[i]
        if (HAL_I2C_Mem_Read(&hi2c2, 0xA0, i, I2C_MEMADD_SIZE_8BIT, &readback[i], 1, 100) != HAL_OK)
        {
            printf("OTA flag read failed\r\n");
            return;
        }
    }

    if (memcmp(flag, readback, sizeof(flag)) != 0)
    {
        printf("OTA flag verify failed\r\n");
        return;
    }

    NVIC_SystemReset();
}

// huart：串口句柄；Size：本次接收字节数
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart,uint16_t Size)
{
    if (huart->Instance == USART1)
    {
        if (Size == 1 && app_rx_buf[0] == 'U')
        {
            need_reboot_to_ota = 1;
        }
        else if (Size == 1 && app_rx_buf[0] == 'V')
        {
            need_report_version = 1;
        }

        HAL_UARTEx_ReceiveToIdle_IT(&huart1, app_rx_buf, sizeof(app_rx_buf));
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
