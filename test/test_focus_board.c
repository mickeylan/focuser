/* Offline production-path test: real board adapter + motor + FP1, fake HAL. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hal_fake/stm32f1xx_hal.h"
#include "../src/recovered/board_stm32f1.h"
UART_HandleTypeDef huart3;
ADC_HandleTypeDef hadc1;
TIM_HandleTypeDef htim3, htim4;
static uint8_t *rx;
static int pulses, rearms, irq, irq_mask;
static char uart_out[4096], usb_out[4096];
static int32_t stored_max = 1000;
static int save_fails, load_fails, saves;
int focus_board_load_max(int32_t *maximum) { assert(!irq); if (load_fails) return -1; *maximum = stored_max; return 0; }
int focus_board_save_max(int32_t maximum) { assert(!irq && !htim4.running); saves++; if (save_fails) return -1; stored_max = maximum; return 0; }
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *);
void HAL_UART_ErrorCallback(UART_HandleTypeDef *);
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *);
void TIM4_IRQHandler(void);
uint32_t __get_PRIMASK(void) { return (uint32_t)irq_mask; }
void __disable_irq(void) { irq_mask = 1; }
void __set_PRIMASK(uint32_t m) { irq_mask = (int)m; }
int focus_board_platform_init(void) { htim3.Init.Prescaler = htim4.Init.Prescaler = 71; return 0; }
int focus_board_usb_send(const uint8_t *data, uint16_t n) { assert(!irq); strncat(usb_out, (const char *)data, n); return 0; }
void HAL_GPIO_WritePin(void *p, uint16_t pin, int state) { (void)p; if (pin == GPIO_PIN_8 && state) pulses++; }
int HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *b, uint16_t n) { assert(h == &huart3 && n == 1); rx = b; rearms++; return HAL_OK; }
int HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *b, uint16_t n, uint32_t t) { (void)h; (void)t; assert(!irq); strncat(uart_out, (char *)b, n); return HAL_OK; }
void HAL_UART_IRQHandler(UART_HandleTypeDef *h) { (void)h; }
/* An IRQ without an update must not advance the motor. */
void HAL_TIM_IRQHandler(TIM_HandleTypeDef *h) { (void)h; }
int HAL_TIM_Base_Start_IT(TIM_HandleTypeDef *h) { h->running = 1; return HAL_OK; }
int HAL_TIM_Base_Stop_IT(TIM_HandleTypeDef *h) { h->running = 0; return HAL_OK; }
int HAL_ADC_Start(ADC_HandleTypeDef *h) { (void)h; assert(!irq); return HAL_OK; }
int HAL_ADC_PollForConversion(ADC_HandleTypeDef *h, uint32_t t) { (void)h; (void)t; return HAL_OK; }
uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef *h) { (void)h; return 2048; }
uint32_t HAL_RCC_GetPCLK1Freq(void) { return 36000000u; }
void HAL_RCC_GetClockConfig(RCC_ClkInitTypeDef *c, uint32_t *l) { c->APB1CLKDivider = 2; *l = 0; }
static void receive(const char *s)
{
    while (*s) { *rx = (uint8_t)*s++; irq = 1; HAL_UART_RxCpltCallback(&huart3); irq = 0; }
}
static void command(const char *s, const char *expected)
{
    uart_out[0] = 0; receive(s); assert(uart_out[0] == 0);
    focus_board_tick(); assert(strcmp(uart_out, expected) == 0);
}
int main(void)
{
    const FocusDevice *d = focus_device();
    focus_board_init(); assert(rearms == 1 && htim4.arr == 3999);
    command("PING\n", "PONG YacaoFocus 1.001\r\n");
    command("LIMIT?\n", "LIMIT state=0 max=1000\r\n");
    command("MOVE 1\n", "ERR STATE\r\n");
    command("GOTO 1\n", "ERR STATE\r\n");
    command("JOG 1\n", "ERR STATE\r\n");
    command("ZERO\n", "OK\r\n");
    command("LIMIT?\n", "LIMIT state=2 max=1000\r\n");
    command("RATE 100\n", "OK\r\n"); assert(htim4.arr == 9999 && htim3.arr == 9999);
    assert(d->is_moving(NULL) == 0);
    command("MOVE 2\n", "OK\r\n"); assert(d->is_moving(NULL) == 1 && htim4.running);
    TIM4_IRQHandler(); assert(pulses == 0);
    HAL_TIM_PeriodElapsedCallback(&htim3); assert(pulses == 0);
    HAL_TIM_PeriodElapsedCallback(&htim4); assert(pulses == 1);
    HAL_TIM_PeriodElapsedCallback(&htim4); assert(pulses == 2 && !htim4.running && !d->is_moving(NULL));
    HAL_TIM_PeriodElapsedCallback(&htim4); assert(pulses == 2);
    command("MOVE 100\n", "OK\r\n");
    command("GOTO 2\n", "OK\r\n"); assert(!htim4.running);
    command("MOVE 100\n", "OK\r\n");
    command("HALT\n", "OK\r\n"); assert(!d->is_moving(NULL) && d->rate(NULL) == 100);
    command("RATE 0\n", "OK\r\n"); assert(d->rate(NULL) == 0);
    command("MOVE 1\n", "ERR STATE\r\n");
    command("RATE 100\n", "OK\r\n");
    command("RATE 1\n", "ERR STATE\r\n"); assert(htim4.arr == 9999);
    command("RATE 1000001\n", "ERR STATE\r\n");
    command("GOTO 2147483647\n", "ERR STATE\r\n");
    command("GOTO -2147483648\n", "ERR STATE\r\n"); /* delta wider than int32 */
    command("HALT\n", "OK\r\n");
    command("MOVE 2147483647\n", "ERR STATE\r\n"); /* target overflows */
    command("MOVE 2147483648\n", "ERR RANGE\r\n");
    /* Independent UART/USB framing and response ownership. */
    uart_out[0] = usb_out[0] = 0;
    receive("PI"); focus_board_usb_receive((const uint8_t *)"POS?\n", 5);
    focus_board_tick(); assert(uart_out[0] == 0 && strcmp(usb_out, "POS 2\r\n") == 0);
    receive("NG\n"); focus_board_tick(); assert(strcmp(uart_out, "PONG YacaoFocus 1.001\r\n") == 0);
    /* UART loss invalidates the entire partial command and retries RX. */
    receive("MOVE 1"); HAL_UART_ErrorCallback(&huart3); receive("0\n");
    uart_out[0] = 0; focus_board_tick(); assert(strcmp(uart_out, "ERR LINE\r\n") == 0 && !d->is_moving(NULL));
    /* Bounded full-line queue: overflow cannot execute a command suffix. */
    receive("PING\nPING\nPING\nPING\nMOVE 999\n");
    for (int i = 0; i < 6; i++) focus_board_tick();
    assert(!d->is_moving(NULL) && d->position(NULL) == 2);
    /* Two-point manual calibration through the real wire/board path. */
    command("CALBEGIN extra\n", "ERR RANGE\r\n");
    command("CALBEGIN\n", "OK\r\n"); assert(d->position(NULL) == 0 && d->rate(NULL) == 100);
    command("LIMIT?\n", "LIMIT state=1 max=0\r\n");
    command("MAXSET\n", "ERR STATE\r\n");
    command("MOVE 1\n", "ERR STATE\r\n");
    command("GOTO 1\n", "ERR STATE\r\n");
    command("JOG -1\n", "ERR STATE\r\n");
    command("JOG 101\n", "ERR RANGE\r\n");
    command("RATE 101\n", "ERR STATE\r\n");
    command("JOG 3\n", "OK\r\n");
    command("ZERO\n", "ERR STATE\r\n");
    command("CALBEGIN\n", "ERR STATE\r\n");
    command("MAXSET\n", "ERR STATE\r\n");
    command("JOG 1\n", "ERR STATE\r\n");
    for (int i = 0; i < 3; i++) HAL_TIM_PeriodElapsedCallback(&htim4);
    save_fails = 1;
    command("MAXSET\n", "ERR STATE\r\n"); assert(stored_max == 1000 && d->calibration_state(NULL) == 1);
    save_fails = 0;
    command("MAXSET\n", "OK\r\n"); assert(stored_max == 3 && saves == 2);
    command("LIMIT?\n", "LIMIT state=2 max=3\r\n");
    {
        int before = pulses;
        command("MOVE 1\n", "ERR STATE\r\n");
        command("GOTO 4\n", "ERR STATE\r\n");
        HAL_TIM_PeriodElapsedCallback(&htim4); assert(pulses == before);
        command("GOTO 0\n", "OK\r\n");
        for (int i = 0; i < 3; i++) HAL_TIM_PeriodElapsedCallback(&htim4);
        assert(d->position(NULL) == 0 && pulses == before + 3);
        command("MOVE -1\n", "ERR STATE\r\n");
        command("GOTO 3\n", "OK\r\n");
        for (int i = 0; i < 5; i++) HAL_TIM_PeriodElapsedCallback(&htim4);
        assert(d->position(NULL) == 3 && pulses == before + 6);
    }
    /* Simulated restart remembers travel, not physical position trust. */
    focus_board_init();
    command("LIMIT?\n", "LIMIT state=0 max=3\r\n");
    command("MOVE 1\n", "ERR STATE\r\n");
    command("ZERO\n", "OK\r\n");
    command("GOTO 4\n", "ERR STATE\r\n");
    command("GOTO 3\n", "OK\r\n");
    command("HALT\n", "OK\r\n");
    /* A failed/corrupt storage load cannot enable unbounded ordinary moves. */
    load_fails = 1; focus_board_init();
    command("LIMIT?\n", "LIMIT state=0 max=0\r\n");
    command("ZERO\n", "OK\r\n");
    command("LIMIT?\n", "LIMIT state=1 max=0\r\n");
    command("MOVE 1\n", "ERR STATE\r\n");
    printf("board production-path assertions passed (RX rearms=%d, physical pulses=%d)\n", rearms, pulses);
    return 0;
}
