/*
 * test_focus_motor.c - checks for the reconstructed focus motor behaviour.
 * The expectations mirror the firmware: continuous motion follows the
 * direction variable, and a counted move stops itself when it runs out.
 */
#include <stdio.h>
#include <string.h>

#include "../src/recovered/focus_motor.h"

static int steps_emitted;

static void count_step(void *ctx)
{
    (void)ctx;
    steps_emitted++;
}

static int failures;

static void check(int condition, const char *description)
{
    if (condition) {
        printf("ok    %s\n", description);
        return;
    }
    printf("FAIL  %s\n", description);
    failures++;
}

int main(void)
{
    FocusMotor motor;

    focus_motor_init(&motor, count_step, NULL);
    steps_emitted = 0;

    /* Continuous motion outwards: the position grows with every tick. */
    focus_motor_start(&motor, FOCUS_DIR_OUT);
    focus_motor_tick_continuous(&motor);
    focus_motor_tick_continuous(&motor);
    check(focus_motor_position(&motor) == 2, "continuous motion counts outwards");
    check(steps_emitted == 2, "continuous motion emits one step per tick");

    /* Continuous motion inwards: the position shrinks. */
    focus_motor_start(&motor, FOCUS_DIR_IN);
    focus_motor_tick_continuous(&motor);
    check(focus_motor_position(&motor) == 1, "continuous motion counts inwards");

    /* Stopped motor does not move. */
    focus_motor_stop(&motor);
    focus_motor_tick_continuous(&motor);
    check(focus_motor_position(&motor) == 1, "a stopped motor does not move");
    check(steps_emitted == 3, "stopped continuous tick emits no physical pulse");

    /* Counted move: N steps, then the tick reports that the move is done. */
    focus_motor_move_steps(&motor, 3);
    check(focus_motor_tick_counted(&motor) == 0, "counted move continues");
    check(focus_motor_tick_counted(&motor) == 0, "counted move continues");
    check(focus_motor_tick_counted(&motor) == 1, "counted move reports completion");
    check(focus_motor_position(&motor) == 4, "counted move advanced three steps");

    /* Counted move inwards uses the same counter with a negative value. */
    focus_motor_move_steps(&motor, -2);
    focus_motor_tick_counted(&motor);
    check(focus_motor_tick_counted(&motor) == 1, "inwards counted move completes");
    check(focus_motor_position(&motor) == 2, "inwards counted move moved back");

    {
        int before = steps_emitted;
        focus_motor_tick_counted(&motor);
        check(steps_emitted == before && motor.direction == FOCUS_DIR_STOP,
              "completed counted move cannot emit another pulse");
        focus_motor_move_steps(&motor, 20);
        focus_motor_move_steps(&motor, 0);
        focus_motor_tick_counted(&motor);
        check(steps_emitted == before && motor.direction == FOCUS_DIR_STOP,
              "zero-step replacement cancels motion without a pulse");
        motor.position = INT32_MAX;
        focus_motor_start(&motor, FOCUS_DIR_OUT);
        focus_motor_tick_continuous(&motor);
        check(motor.position == INT32_MAX && steps_emitted == before,
              "positive position limit stops without overflow or pulse");
        motor.position = INT32_MIN;
        focus_motor_move_steps(&motor, -1);
        focus_motor_tick_counted(&motor);
        check(motor.position == INT32_MIN && steps_emitted == before,
              "negative position limit rejects overflowing move");
        motor.position = 0;
        focus_motor_move_steps(&motor, INT32_MIN);
        check(motor.remaining == INT32_MIN, "INT32_MIN step count never negated");
        focus_motor_stop(&motor);
    }

    /* Rate handling. */
    focus_motor_set_rate(&motor, 400);
    check(focus_motor_rate(&motor) == 400, "rate is stored");
    focus_motor_set_rate(&motor, -1);
    check(focus_motor_rate(&motor) == 0, "a negative rate is rejected");

    if (failures == 0) {
        printf("\nall focus motor checks passed\n");
        return 0;
    }
    printf("\n%d check(s) failed\n", failures);
    return 1;
}
