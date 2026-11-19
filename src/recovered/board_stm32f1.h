/*
 * board_stm32f1.h - board layer entry points.
 */
#ifndef BOARD_STM32F1_H
#define BOARD_STM32F1_H

#include <stddef.h>
#include <stdint.h>

#include "../port/focus_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Integration contract (not a flashable CubeMX project):
 * supply focus_board_platform_init(), returning 0 after HAL/clock/GPIO/UART3/
 * ADC1/TIM3/TIM4 setup, and external huart3/hadc1/htim3/htim4 handles.
 * Configure TIM3/4 base update IRQs, disable OC IRQs, ARR preload disabled;
 * UART IRQs enabled. Do not duplicate the IRQ handlers/callbacks in board.c.
 * Wire CDC_Receive_FS to focus_board_usb_receive() and rearm USB RX normally.
 * Supply focus_board_usb_send(data,length): copy/finish before returning;
 * return 0 on success, negative on failure; never retain the caller's pointer.
 * Validate STEP=PA8, DIR=PA9 and pulse width against the actual board FIRST.
 * Supply voltage sensing and USB support are optional platform integrations.
 * Bring up peripherals, motor state and command queues. */
void focus_board_init(void);

/* Must run repeatedly in foreground; parses queued commands and sends replies.
 * No blocking HAL transmit/ADC or protocol parsing runs in receive ISRs. */
void focus_board_tick(void);

/* Queue UART bytes; the UART completion callback calls this and rearms RX. */
void focus_board_receive(const uint8_t *data, uint16_t length);
/* Call from the generated USB CDC receive callback; replies stay on USB. */
void focus_board_usb_receive(const uint8_t *data, uint16_t length);

/* Platform nonvolatile storage hooks (required, not supplied by this adapter).
 * load: return 0 only for a validated positive maximum; otherwise negative.
 * save: atomically replace and verify the maximum, retaining old valid data on
 * failure/power loss; return 0 only after durable success. Reserve storage in the
 * platform linker map, never erase guessed flash pages. No current position is
 * persisted. Hooks run in foreground with motor stopped. A failing save keeps
 * the device in manual calibration, never silently claims persistence. */
int focus_board_load_max(int32_t *maximum);
int focus_board_save_max(int32_t maximum);

/* Device operations exposed to the protocol layer. */
const FocusDevice *focus_device(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_STM32F1_H */
