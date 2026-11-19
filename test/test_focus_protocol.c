/*
 * test_focus_protocol.c - host-side checks for the FP1 command parser.
 * Build: gcc -std=c99 -Wall -Wextra -Isrc -o test_focus_protocol.exe \
 *            test/test_focus_protocol.c src/protocol/focus_protocol.c
 */
#include <stdio.h>
#include <string.h>

#include "../src/protocol/focus_protocol.h"

typedef struct {
    int32_t position;
    int32_t rate;
    int32_t voltage;
    char output[1024];
    size_t written;
    FocusProtocol protocol;
} MockDevice;

static int32_t mock_position(void *ctx)
{
    return ((MockDevice *)ctx)->position;
}

static int32_t mock_move_relative(void *ctx, int32_t steps)
{
    MockDevice *device = (MockDevice *)ctx;
    device->position += steps;
    return 0;
}

static int32_t mock_move_absolute(void *ctx, int32_t steps)
{
    MockDevice *device = (MockDevice *)ctx;
    device->position = steps;
    return 0;
}

static int32_t mock_halt(void *ctx)
{
    (void)ctx; /* HALT retains configured rate, as the real motor does. */
    return 0;
}

static int32_t mock_moving(void *ctx) { (void)ctx; return 0; }

static int32_t mock_rate(void *ctx)
{
    return ((MockDevice *)ctx)->rate;
}

static int32_t mock_set_rate(void *ctx, int32_t rate)
{
    ((MockDevice *)ctx)->rate = rate;
    return 0;
}

static int32_t mock_voltage(void *ctx)
{
    return ((MockDevice *)ctx)->voltage;
}

static const char *mock_version(void *ctx)
{
    (void)ctx;
    return "1.001";
}

static const char *mock_model(void *ctx)
{
    (void)ctx;
    return "YacaoFocus";
}

static void mock_send(void *ctx, const char *text, size_t length)
{
    MockDevice *device = (MockDevice *)ctx;
    if (device->written + length >= sizeof device->output) return;
    memcpy(device->output + device->written, text, length);
    device->written += length;
    device->output[device->written] = '\0';
}

static int failures = 0;

static void expect(MockDevice *device, const char *command, const char *expected)
{
    device->written = 0;
    device->output[0] = '\0';
    focus_protocol_execute(&device->protocol, command);
    if (strcmp(device->output, expected) != 0) {
        printf("FAIL  %-28s expected %-34s got %s\n", command, expected, device->output);
        failures++;
        return;
    }
    printf("ok    %-28s -> %s", command, device->output);
}

static void expect_feed(MockDevice *device, const char *bytes, const char *expected)
{
    device->written = 0;
    device->output[0] = '\0';
    focus_protocol_feed(&device->protocol, bytes, strlen(bytes));
    if (strcmp(device->output, expected) != 0) {
        printf("FAIL  feed %-23s expected %-34s got %s\n", bytes, expected, device->output);
        failures++;
        return;
    }
    printf("ok    feed %-23s -> %s", bytes, device->output);
}

int main(void)
{
    MockDevice device;
    FocusDevice operations;

    memset(&device, 0, sizeof device);
    device.position = 1234;
    device.rate = 0;
    device.voltage = 12000;

    memset(&operations, 0, sizeof operations);
    operations.ctx = &device;
    operations.position = mock_position;
    operations.move_relative = mock_move_relative;
    operations.move_absolute = mock_move_absolute;
    operations.halt = mock_halt;
    operations.rate = mock_rate;
    operations.set_rate = mock_set_rate;
    operations.voltage_mv = mock_voltage;
    operations.version = mock_version;
    operations.model = mock_model;
    operations.send = mock_send;
    operations.is_moving = mock_moving;

    focus_protocol_init(&device.protocol, &operations);

    expect(&device, "PING", "PONG YacaoFocus 1.001\r\n");
    expect(&device, "ping", "PONG YacaoFocus 1.001\r\n");
    expect(&device, "VER?", "VER 1.001\r\n");
    expect(&device, "POS?", "POS 1234\r\n");
    expect(&device, "STATUS?", "STATUS pos=1234 rate=0 volt=12000 moving=0\r\n");
    expect(&device, "VOLT?", "VOLT 12000\r\n");
    expect(&device, "MOVE 100", "OK\r\n");
    if (device.position != 1334) {
        printf("FAIL  MOVE did not move the motor (position=%ld)\n", (long)device.position);
        failures++;
    }
    expect(&device, "GOTO 500", "OK\r\n");
    if (device.position != 500) {
        printf("FAIL  GOTO did not set the position (position=%ld)\n", (long)device.position);
        failures++;
    }
    expect(&device, "RATE 250", "OK\r\n");
    expect(&device, "RATE?", "RATE 250\r\n");
    expect(&device, "HALT", "OK\r\n");
    expect(&device, "RATE?", "RATE 250\r\n");
    expect(&device, "STATUS?", "STATUS pos=500 rate=250 volt=12000 moving=0\r\n");
    expect(&device, "MOVE 2147483648", "ERR RANGE\r\n");
    expect(&device, "GOTO -2147483649", "ERR RANGE\r\n");
    expect(&device, "RATE 4294967297", "ERR RANGE\r\n");
    expect(&device, "BOGUS", "ERR UNKNOWN\r\n");
    expect(&device, "MOVE abc", "ERR RANGE\r\n");
    expect(&device, "MOVE", "ERR RANGE\r\n");
    expect(&device, "RATE -1", "ERR RANGE\r\n");
    expect(&device, "MOVE 12x", "ERR RANGE\r\n");

    /* Framing: a command split across two reads is still one command. */
    expect_feed(&device, "PI", "");
    expect_feed(&device, "NG\r\n", "PONG YacaoFocus 1.001\r\n");
    expect_feed(&device, "MOVE 7\nPOS?\n", "OK\r\nPOS 507\r\n");

    /* An over-long line is discarded whole, and the next line still works. */
    {
        char long_line[FP_MAX_LINE + 64];
        memset(long_line, 'x', FP_MAX_LINE + 10);
        long_line[FP_MAX_LINE + 10] = '\0';
        strcat(long_line, "\nPING\n");
        expect_feed(&device, long_line, "ERR LINE\r\nPONG YacaoFocus 1.001\r\n");
    }

    /* An unknown voltage is a state error, not a bogus reading. */
    device.voltage = -1;
    expect(&device, "VOLT?", "ERR STATE\r\n");
    device.voltage = 12000;

    if (failures == 0) {
        printf("\nall focus protocol checks passed\n");
        return 0;
    }
    printf("\n%d check(s) failed\n", failures);
    return 1;
}
