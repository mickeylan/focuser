/*
 * focus_motor.h - focus motor state machine reconstructed from the firmware.
 *
 * Recovered behaviour (see docs/RECONSTRUCTION.md):
 *   - Two timer interrupts drive the TMC2225 STEP input. The original used one
 *     timer for continuous motion (direction in {-1, 0, +1}) and a second timer
 *     for "move N steps and stop".
 *   - The STEP pulse is emitted inside the interrupt: pin high, a short busy
 *     delay, pin low.
 *   - The position is counted in full steps.
 *
 * The module is deliberately free of hardware dependencies so that it can be
 * unit tested on the host; the board layer supplies the pulse callback.
 */
#ifndef FOCUS_MOTOR_H
#define FOCUS_MOTOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FOCUS_DIR_STOP = 0,
    FOCUS_DIR_IN = -1,
    FOCUS_DIR_OUT = 1
} FocusDirection;

typedef struct FocusMotor {
    /* Continuous-motion direction; 0 means stopped. */
    int8_t direction;
    /* Position in full steps since power-up. */
    int32_t position;
    /* Remaining steps of an "move N steps" request; sign carries direction. */
    int32_t remaining;
    /* Configured step rate in steps per second; retained after stop.
     * direction == STOP is the motion-state authority. */
    int32_t rate;
    /* Called once per step to emit the STEP pulse. */
    void (*emit_step)(void *ctx);
    void *ctx;
} FocusMotor;

void focus_motor_init(FocusMotor *motor, void (*emit_step)(void *ctx), void *ctx);

/* Continuous motion in the given direction until focus_motor_stop(). */
void focus_motor_start(FocusMotor *motor, FocusDirection direction);

/* Move a number of steps and stop automatically; negative counts move inwards. */
void focus_motor_move_steps(FocusMotor *motor, int32_t steps);

/* Stop continuous motion and cancel a pending counted move. */
void focus_motor_stop(FocusMotor *motor);

int32_t focus_motor_position(const FocusMotor *motor);
void focus_motor_set_rate(FocusMotor *motor, int32_t steps_per_second);
int32_t focus_motor_rate(const FocusMotor *motor);

/* Timer interrupt hook: continuous mode, mirrors the original TIM3 handler. */
void focus_motor_tick_continuous(FocusMotor *motor);

/* Timer interrupt hook: counted mode, mirrors the original TIM4 handler.
 * Returns 1 when the move finished and the timer must be stopped. */
int focus_motor_tick_counted(FocusMotor *motor);

#ifdef __cplusplus
}
#endif

#endif /* FOCUS_MOTOR_H */
