/* STM32F103 FP1 adapter. Requires the platform hooks declared below; this is
 * not a standalone flashable CubeMX project. ISR callbacks only enqueue input.
 * STEP/DIR pins below are reconstruction assumptions, NOT hardware-verified. */
#include "board_stm32f1.h"
#include "stm32f1xx_hal.h"
#include "../protocol/focus_protocol.h"
#include "focus_motor.h"
#include <string.h>

/* Supplied by the CubeMX integration: initialized, shared (not private copies)
 * of its peripheral handles. See board_stm32f1.h for integration obligations. */
extern UART_HandleTypeDef huart3;
extern ADC_HandleTypeDef hadc1;
extern TIM_HandleTypeDef htim3, htim4;
extern int focus_board_platform_init(void);
extern int focus_board_usb_send(const uint8_t *data, uint16_t length);
extern int focus_board_load_max(int32_t *maximum);
extern int focus_board_save_max(int32_t maximum);

static FocusMotor motor;
/* Never persist position: manual knob motion while powered off is invisible. */
static int calibration_state;
static int32_t saved_max, travel_max;
#define QUEUE_SLOTS 4u
#define STEP_PORT GPIOA
#define STEP_PIN GPIO_PIN_8
#define DIR_PORT GPIOA
#define DIR_PIN GPIO_PIN_9

typedef struct {
    FocusProtocol protocol;
    char lines[QUEUE_SLOTS][FP_MAX_LINE];
    unsigned read, write, used;
    unsigned length, dropping, errors;
    int usb;
} Input;
static Input uart_input, usb_input;
static uint8_t uart_byte;
static int ready;
static volatile int uart_rearm;

static uint32_t lock(void) { uint32_t mask = __get_PRIMASK(); __disable_irq(); return mask; }
static void unlock(uint32_t mask) { __set_PRIMASK(mask); }

static void pulse(void *ctx)
{
    (void)ctx;
    HAL_GPIO_WritePin(STEP_PORT, STEP_PIN, GPIO_PIN_SET);
    for (volatile int i = 0; i < 22; ++i) { }
    HAL_GPIO_WritePin(STEP_PORT, STEP_PIN, GPIO_PIN_RESET);
}
static void stop(void)
{
    HAL_TIM_Base_Stop_IT(&htim3);
    HAL_TIM_Base_Stop_IT(&htim4);
    focus_motor_stop(&motor);
}
static int32_t position(void *ctx) { (void)ctx; return motor.position; }
static int32_t moving(void *ctx) { (void)ctx; return motor.direction != FOCUS_DIR_STOP; }
static int32_t rate(void *ctx) { (void)ctx; return motor.rate; }
static int32_t halt(void *ctx)
{
    uint32_t mask = lock(); (void)ctx; stop(); unlock(mask); return 0;
}
/* Caller holds IRQ mask across snapshot, replacement and timer start. */
static int32_t move_locked(int64_t delta)
{
    int64_t target = (int64_t)motor.position + delta;
    if (delta < INT32_MIN || delta > INT32_MAX || target < INT32_MIN || target > INT32_MAX) return -1;
    if (target < 0 || (calibration_state == 2 && target > travel_max)) return -1;
    if (delta != 0 && motor.rate <= 0) return -1;
    stop();
    if (delta == 0) return 0;
    HAL_GPIO_WritePin(DIR_PORT, DIR_PIN, delta < 0 ? GPIO_PIN_RESET : GPIO_PIN_SET);
    focus_motor_move_steps(&motor, (int32_t)delta);
    __HAL_TIM_SET_COUNTER(&htim4, 0);
    if (HAL_TIM_Base_Start_IT(&htim4) != HAL_OK) { stop(); return -1; }
    return 0;
}
static int32_t relative(void *ctx, int32_t steps)
{
    uint32_t mask = lock(); int32_t result; (void)ctx;
    result = calibration_state == 2 ? move_locked(steps) : -1; unlock(mask); return result;
}
static int32_t absolute(void *ctx, int32_t steps)
{
    uint32_t mask = lock(); int32_t result; (void)ctx;
    result = calibration_state == 2 ? move_locked((int64_t)steps - motor.position) : -1; unlock(mask); return result;
}
static int timer_period(TIM_HandleTypeDef *timer, uint32_t hz, uint32_t *period)
{
    RCC_ClkInitTypeDef config; uint32_t latency;
    uint64_t clock;
    HAL_RCC_GetClockConfig(&config, &latency);
    clock = HAL_RCC_GetPCLK1Freq();
    if (config.APB1CLKDivider != RCC_HCLK_DIV1) clock *= 2;
    clock /= (uint64_t)timer->Init.Prescaler + 1;
    if (hz == 0 || hz > clock) return -1;
    *period = (uint32_t)((clock + hz / 2) / hz);
    return *period >= 1 && *period <= 65536u ? 0 : -1;
}
static int32_t set_rate(void *ctx, int32_t hz)
{
    uint32_t p3, p4, mask; (void)ctx;
    if (hz < 0 || (calibration_state == 1 && hz > 100)) return -1;
    if (hz == 0) { halt(ctx); focus_motor_set_rate(&motor, 0); return 0; }
    if (timer_period(&htim3, (uint32_t)hz, &p3) || timer_period(&htim4, (uint32_t)hz, &p4)) return -1;
    mask = lock();
    __HAL_TIM_SET_AUTORELOAD(&htim3, p3 - 1);
    __HAL_TIM_SET_AUTORELOAD(&htim4, p4 - 1);
    __HAL_TIM_SET_COUNTER(&htim3, 0);
    __HAL_TIM_SET_COUNTER(&htim4, 0);
    focus_motor_set_rate(&motor, hz);
    unlock(mask); return 0;
}
static int32_t voltage(void *ctx)
{
    (void)ctx;
    if (HAL_ADC_Start(&hadc1) != HAL_OK || HAL_ADC_PollForConversion(&hadc1, 10) != HAL_OK) return -1;
    return (int32_t)((uint64_t)HAL_ADC_GetValue(&hadc1) * 3300u * 6u / 4095u);
}
static const char *version(void *ctx) { (void)ctx; return "1.001"; }
static const char *model(void *ctx) { (void)ctx; return "YacaoFocus"; }
static void send(void *ctx, const char *text, size_t length)
{
    Input *input = (Input *)ctx;
    /* Foreground only. USB hook must copy/finish before return (no stack DMA). */
    if (input != NULL && input->usb) focus_board_usb_send((const uint8_t *)text, (uint16_t)length);
    else HAL_UART_Transmit(&huart3, (uint8_t *)text, (uint16_t)length, 100);
}
static int32_t calibration(void *ctx, int action)
{
    uint32_t mask; (void)ctx;
    if (motor.direction != FOCUS_DIR_STOP) return -1;
    if (action == 2) {
        int32_t maximum = motor.position;
        if (calibration_state != 1 || maximum <= 0) return -1;
        /* Foreground, stopped: platform must atomically save+verify; failure
         * leaves calibration active and must not report a saved maximum. */
        if (focus_board_save_max(maximum) != 0) return -1;
        saved_max = travel_max = maximum; calibration_state = 2;
        return 0;
    }
    if (action != 0 && action != 1) return -1;
    mask = lock(); stop(); motor.position = 0;
    travel_max = action == 1 ? 0 : saved_max;
    calibration_state = travel_max > 0 ? 2 : 1;
    unlock(mask);
    /* Calibration is deliberately slow and only permits bounded explicit JOG. */
    if (calibration_state == 1 && set_rate(NULL, 100) != 0) {
        calibration_state = 0; return -1;
    }
    return 0;
}
static int32_t cal_state(void *ctx) { (void)ctx; return calibration_state; }
static int32_t max_position(void *ctx) { (void)ctx; return travel_max; }
static int32_t jog(void *ctx, int32_t steps)
{
    int32_t result; uint32_t mask = lock(); (void)ctx;
    /* No queuing/replacement while calibrating: each click must finish first. */
    if (calibration_state != 1 || motor.direction != FOCUS_DIR_STOP ||
        steps == 0 || steps < -100 || steps > 100) { unlock(mask); return -1; }
    result = move_locked(steps); unlock(mask); return result;
}
const FocusDevice *focus_device(void)
{
    static const FocusDevice device = {
        NULL, position, relative, absolute, halt, rate, set_rate, voltage,
        version, model, send, moving, calibration, cal_state, max_position, jog
    };
    return &device;
}
static void init_input(Input *input, int usb)
{
    FocusDevice device = *focus_device();
    memset(input, 0, sizeof *input); input->usb = usb; device.ctx = input;
    focus_protocol_init(&input->protocol, &device);
}
void focus_board_init(void)
{
    ready = 0;
    calibration_state = 0; saved_max = travel_max = 0;
    if (focus_board_platform_init() != 0) return;
    focus_motor_init(&motor, pulse, NULL);
    {
        int32_t maximum = 0;
        if (focus_board_load_max(&maximum) == 0 && maximum > 0)
            saved_max = travel_max = maximum;
    }
    init_input(&uart_input, 0); init_input(&usb_input, 1);
    if (set_rate(NULL, 250) != 0) return;
    ready = 1;
    uart_rearm = HAL_UART_Receive_IT(&huart3, &uart_byte, 1) != HAL_OK;
}
static void enqueue(Input *input, const uint8_t *data, uint16_t length)
{
    uint32_t mask;
    if (!ready || data == NULL) return;
    mask = lock();
    for (uint16_t i = 0; i < length; ++i) {
        char c = (char)data[i];
        if (c == '\r') continue;
        if (input->used == QUEUE_SLOTS) input->dropping = 1;
        if (c == '\n') {
            if (input->dropping) { input->errors++; }
            else {
                input->lines[input->write][input->length] = '\0';
                input->write = (input->write + 1) % QUEUE_SLOTS; input->used++;
            }
            input->length = 0; input->dropping = 0;
        } else if (!input->dropping) {
            if (input->length + 1 >= FP_MAX_LINE) input->dropping = 1;
            else input->lines[input->write][input->length++] = c;
        }
    }
    unlock(mask);
}
void focus_board_receive(const uint8_t *data, uint16_t length) { enqueue(&uart_input, data, length); }
void focus_board_usb_receive(const uint8_t *data, uint16_t length) { enqueue(&usb_input, data, length); }
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
    if (uart != &huart3 || !ready) return;
    focus_board_receive(&uart_byte, 1);
    uart_rearm = HAL_UART_Receive_IT(&huart3, &uart_byte, 1) != HAL_OK;
}
void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    if (uart == &huart3) {
        /* An overrun may have lost bytes: never execute a truncated command. */
        uart_input.dropping = 1;
        uart_rearm = 1;
    }
}
static void drain(Input *input)
{
    char line[FP_MAX_LINE]; uint32_t mask = lock();
    int available = input->used != 0;
    unsigned errors = input->errors; input->errors = 0;
    if (available) {
        memcpy(line, input->lines[input->read], sizeof line);
        input->read = (input->read + 1) % QUEUE_SLOTS; input->used--;
    }
    unlock(mask);
    if (available) focus_protocol_execute(&input->protocol, line);
    if (errors) send(input, "ERR LINE\r\n", 10);
}
void focus_board_tick(void)
{
    if (!ready) return;
    if (uart_rearm) uart_rearm = HAL_UART_Receive_IT(&huart3, &uart_byte, 1) != HAL_OK;
    drain(&uart_input); drain(&usb_input);
}
/* Dispatch steps only for an actual timer update, never an arbitrary IRQ. */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *timer)
{
    if (!ready) return;
    /* FP1 exposes counted motion only; TIM3 must not advance a TIM4 move. */
    if (timer == &htim4) {
        if (calibration_state == 0 ||
            (motor.direction == FOCUS_DIR_IN && motor.position <= 0) ||
            (calibration_state == 2 && motor.direction == FOCUS_DIR_OUT && motor.position >= travel_max)) {
            stop(); return;
        }
        if (focus_motor_tick_counted(&motor)) stop();
    }
}
void TIM3_IRQHandler(void) { HAL_TIM_IRQHandler(&htim3); }
void TIM4_IRQHandler(void) { HAL_TIM_IRQHandler(&htim4); }
void USART3_IRQHandler(void) { HAL_UART_IRQHandler(&huart3); }
