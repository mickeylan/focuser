/*
 * focus_port.h - device operations the protocol layer drives.
 *
 * The protocol layer never touches hardware directly: the board layer supplies
 * these callbacks, which keeps the command parser host-testable.
 */
#ifndef FOCUS_PORT_H
#define FOCUS_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every operation returns 0 on success and a negative value on failure. */
typedef struct FocusDevice {
    void *ctx;
    /* Absolute focus position, in motor steps. */
    int32_t (*position)(void *ctx);
    int32_t (*move_relative)(void *ctx, int32_t steps);
    int32_t (*move_absolute)(void *ctx, int32_t steps);
    int32_t (*halt)(void *ctx);
    /* Configured step rate, retained after HALT; not a motion indicator. */
    int32_t (*rate)(void *ctx);
    int32_t (*set_rate)(void *ctx, int32_t steps_per_second);
    /* Supply voltage in millivolts, or a negative value when unknown. */
    int32_t (*voltage_mv)(void *ctx);
    const char *(*version)(void *ctx);
    const char *(*model)(void *ctx);
    /* Push response bytes to the host. */
    void (*send)(void *ctx, const char *text, size_t length);
    /* Actual motor motion, independent of the configured rate. */
    int32_t (*is_moving)(void *ctx);
    /* Manual calibration: 0=confirm zero, 1=begin new two-point calibration,
     * 2=save current position as maximum. All require a stopped motor. */
    int32_t (*calibrate)(void *ctx, int action);
    /* 0=zero unconfirmed, 1=manual calibration, 2=ready. */
    int32_t (*calibration_state)(void *ctx);
    int32_t (*max_position)(void *ctx);
    int32_t (*jog)(void *ctx, int32_t steps);
} FocusDevice;

#ifdef __cplusplus
}
#endif

#endif /* FOCUS_PORT_H */
