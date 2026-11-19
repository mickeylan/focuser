/* Test-only pipe device. Production FP1 + motor; @TICK is a test clock, not FP1. */
#include <stdio.h>
#include <string.h>
#include "../src/protocol/focus_protocol.h"
#include "../src/recovered/focus_motor.h"
static FocusMotor motor;
static int32_t position(void *ctx) { (void)ctx; return motor.position; }
static int32_t moving(void *ctx) { (void)ctx; return motor.direction != FOCUS_DIR_STOP; }
static int32_t rate(void *ctx) { (void)ctx; return motor.rate; }
static int32_t halt(void *ctx) { (void)ctx; focus_motor_stop(&motor); return 0; }
static int32_t relative(void *ctx, int32_t n) {
    int64_t target = (int64_t)motor.position + n; (void)ctx;
    if (target < INT32_MIN || target > INT32_MAX) return -1;
    focus_motor_move_steps(&motor, n); return 0;
}
static int32_t absolute(void *ctx, int32_t target) {
    int64_t delta = (int64_t)target - motor.position;
    if (delta < INT32_MIN || delta > INT32_MAX) return -1;
    return relative(ctx, (int32_t)delta);
}
static int32_t set_rate(void *ctx, int32_t n) { (void)ctx; focus_motor_set_rate(&motor, n); if (!n) focus_motor_stop(&motor); return 0; }
static int32_t voltage(void *ctx) { (void)ctx; return 12000; }
static const char *version(void *ctx) { (void)ctx; return "1.001"; }
static const char *model(void *ctx) { (void)ctx; return "YacaoFocus"; }
static void send(void *ctx, const char *data, size_t n) { (void)ctx; fwrite(data, 1, n, stdout); fflush(stdout); }
int main(void) {
    FocusDevice device = { NULL, position, relative, absolute, halt, rate, set_rate, voltage, version, model, send, moving, NULL, NULL, NULL, NULL };
    FocusProtocol protocol;
    char line[512];
    focus_motor_init(&motor, NULL, NULL); focus_motor_set_rate(&motor, 250);
    focus_protocol_init(&protocol, &device);
    while (fgets(line, sizeof line, stdin)) {
        if (strcmp(line, "@TICK\n") == 0) {
            focus_motor_tick_counted(&motor); send(NULL, "OK\r\n", 4);
        } else focus_protocol_feed(&protocol, line, strlen(line));
    }
    return 0;
}
