// #include "int_bootloader.h"


// uint8_t rx_buffer[BOOTLOADER_UART_REC_BUFF_LEN] = {0};
// uint16_t rx_len = 0;
// uint16_t rx_full_len = 0;
// //偏移量
// uint16_t flash_write_offset = 0;

// void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
// {
//     if (huart->Instance == USART1)
//     {
//         rx_len = Size;
//         rx_full_len += Size;

//         //解锁
//         HAL_FLASH_Unlock();

//         uint8_t is_erase = 0;
//         for(uint16_t i = 0; i < rx_len; i++)
//         {
//             uint8_t data = *(volatile uint8_t*)(APP_START_ADDR + i + flash_write_offset);
//             if (data != 0xff)
//             {
//                 is_erase = 1;
//                 break;
//             }
//         }       
//         if(is_erase)
//         {
            
//         }

//         HAL_FLASH_Lock();


//     }
//     memset(rx_buffer, 0, sizeof(rx_buffer));

//     HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, BOOTLOADER_UART_REC_BUFF_LEN);
// }

// void Int_bootloader_init(void)
// {
//     __HAL_UART_CLEAR_OREFLAG(&huart1);
//     __HAL_UART_CLEAR_IDLEFLAG(&huart1);

//     HAL_UARTEx_ReceiveToIdle_IT(&huart1, rx_buffer, BOOTLOADER_UART_REC_BUFF_LEN);

    
// }

