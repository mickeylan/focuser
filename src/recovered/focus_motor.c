/*
 * focus_motor.c - focus motor state machine reconstructed from the firmware.
 */
#include "focus_motor.h"

void focus_motor_init(FocusMotor *motor, void (*emit_step)(void *ctx), void *ctx)
{
    if (motor == NULL) return;
    motor->direction = FOCUS_DIR_STOP;
    motor->position = 0;
    motor->remaining = 0;
    motor->rate = 0;
    motor->emit_step = emit_step;
    motor->ctx = ctx;
}

static void emit(FocusMotor *motor)
{
    if (motor->emit_step != NULL) motor->emit_step(motor->ctx);
}

void focus_motor_start(FocusMotor *motor, FocusDirection direction)
{
    if (motor == NULL) return;
    motor->direction = (direction == FOCUS_DIR_IN || direction == FOCUS_DIR_OUT)
        ? (int8_t)direction : FOCUS_DIR_STOP;
    motor->remaining = 0;
}

void focus_motor_move_steps(FocusMotor *motor, int32_t steps)
{
    if (motor == NULL) return;
    int64_t target;
    if (steps == 0) { focus_motor_stop(motor); return; }
    target = (int64_t)motor->position + steps;
    if (target < INT32_MIN || target > INT32_MAX) {
        focus_motor_stop(motor);
        return;
    }
    motor->direction = steps < 0 ? FOCUS_DIR_IN : FOCUS_DIR_OUT;
    motor->remaining = steps;
}

void focus_motor_stop(FocusMotor *motor)
{
    if (motor == NULL) return;
    motor->direction = FOCUS_DIR_STOP;
    motor->remaining = 0;
}

int32_t focus_motor_position(const FocusMotor *motor)
{
    return motor == NULL ? 0 : motor->position;
}

void focus_motor_set_rate(FocusMotor *motor, int32_t steps_per_second)
{
    if (motor == NULL) return;
    motor->rate = steps_per_second > 0 ? steps_per_second : 0;
}

int32_t focus_motor_rate(const FocusMotor *motor)
{
    return motor == NULL ? 0 : motor->rate;
}

void focus_motor_tick_continuous(FocusMotor *motor)
{
    if (motor == NULL || motor->direction == FOCUS_DIR_STOP) return;
    if ((motor->direction == FOCUS_DIR_IN && motor->position == INT32_MIN) ||
        (motor->direction == FOCUS_DIR_OUT && motor->position == INT32_MAX)) {
        focus_motor_stop(motor);
        return;
    }
    if (motor->direction == FOCUS_DIR_IN) {
        motor->position -= 1;
    } else if (motor->direction == FOCUS_DIR_OUT) {
        motor->position += 1;
    }
    emit(motor);
}

int focus_motor_tick_counted(FocusMotor *motor)
{
    if (motor == NULL) return 1;
    if (motor->direction == FOCUS_DIR_STOP || motor->remaining == 0) {
        focus_motor_stop(motor);
        return 1;
    }
    if ((motor->remaining < 0 && motor->position == INT32_MIN) ||
        (motor->remaining > 0 && motor->position == INT32_MAX)) {
        focus_motor_stop(motor);
        return 1;
    }
    if (motor->remaining < 0) {
        motor->remaining += 1;
        motor->position -= 1;
    } else {
        motor->remaining -= 1;
        motor->position += 1;
    }
    emit(motor);
    if (motor->remaining == 0) { focus_motor_stop(motor); return 1; }
    return 0;
}
