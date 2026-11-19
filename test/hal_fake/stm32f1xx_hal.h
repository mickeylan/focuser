#ifndef TEST_HAL_H
#define TEST_HAL_H
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t Prescaler; } TIM_Base_InitTypeDef;
typedef struct { TIM_Base_InitTypeDef Init; uint32_t arr, counter, compare; int running; } TIM_HandleTypeDef;
typedef struct { int unused; } UART_HandleTypeDef;
typedef struct { int unused; } ADC_HandleTypeDef;
typedef struct { uint32_t APB1CLKDivider; } RCC_ClkInitTypeDef;
#define HAL_OK 0
#define HAL_ERROR 1
#define TIM_CHANNEL_1 1
#define HAL_TIM_ACTIVE_CHANNEL_1 1
#define GPIOA ((void *)0)
#define GPIO_PIN_8 256
#define GPIO_PIN_9 512
#define GPIO_PIN_SET 1
#define GPIO_PIN_RESET 0
#define RCC_HCLK_DIV1 1
#define __HAL_TIM_SET_AUTORELOAD(t,v) ((t)->arr=(v))
#define __HAL_TIM_SET_COUNTER(t,v) ((t)->counter=(v))
#define __HAL_TIM_SET_COMPARE(t,c,v) ((t)->compare=(v))
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t value);
void HAL_GPIO_WritePin(void *port, uint16_t pin, int state);
int HAL_UART_Receive_IT(UART_HandleTypeDef *, uint8_t *, uint16_t);
int HAL_UART_Transmit(UART_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
void HAL_UART_IRQHandler(UART_HandleTypeDef *);
void HAL_TIM_IRQHandler(TIM_HandleTypeDef *);
int HAL_TIM_Base_Start_IT(TIM_HandleTypeDef *);
int HAL_TIM_Base_Stop_IT(TIM_HandleTypeDef *);
int HAL_ADC_Start(ADC_HandleTypeDef *);
int HAL_ADC_PollForConversion(ADC_HandleTypeDef *, uint32_t);
uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef *);
uint32_t HAL_RCC_GetPCLK1Freq(void);
void HAL_RCC_GetClockConfig(RCC_ClkInitTypeDef *, uint32_t *);
#endif
