/*
 * focus_protocol.c - FP1 command parser.
 *
 * The dispatch table is the single place where commands are registered, so a
 * vendor-specific alias (for example a third-party controller's command set)
 * is one table entry rather than a new code path.
 */
#include "focus_protocol.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    FP_OK = 0,
    FP_ERR_UNKNOWN,
    FP_ERR_RANGE,
    FP_ERR_STATE
} FpStatus;

typedef FpStatus (*FpHandler)(FocusProtocol *protocol, const char *argument);

typedef struct {
    const char *keyword;
    FpHandler handler;
} FpCommand;

static void reply(FocusProtocol *protocol, const char *format, ...)
{
    char buffer[FP_MAX_LINE];
    va_list arguments;
    int written;

    if (protocol->device.send == NULL) return;
    va_start(arguments, format);
    written = vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
    if (written < 0) return;
    if ((size_t)written >= sizeof buffer) written = (int)sizeof buffer - 1;
    protocol->device.send(protocol->device.ctx, buffer, (size_t)written);
}

/* Strict decimal parser: no trailing garbage, no overflow. */
static int parse_long(const char *text, long *value)
{
    char *end = NULL;
    long parsed;

    if (text == NULL) return -1;
    while (*text == ' ' || *text == '\t') text++;
    if (*text == '\0') return -1;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || parsed < INT32_MIN || parsed > INT32_MAX) return -1;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return -1;
    *value = parsed;
    return 0;
}

static const char *skip_spaces(const char *text)
{
    while (*text == ' ' || *text == '\t') text++;
    return text;
}

static int equals_ignore_case(const char *left, const char *right)
{
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        char a = *left;
        char b = *right;
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return *left == '\0' && *right == '\0';
}

/* ----------------------------------------------------------- command handlers */

static FpStatus command_ping(FocusProtocol *protocol, const char *argument)
{
    const char *model = "focuser";
    const char *version = "0";

    (void)argument;
    if (protocol->device.model != NULL) model = protocol->device.model(protocol->device.ctx);
    if (protocol->device.version != NULL) version = protocol->device.version(protocol->device.ctx);
    reply(protocol, "PONG %s %s\r\n", model, version);
    return FP_OK;
}

static FpStatus command_version(FocusProtocol *protocol, const char *argument)
{
    const char *version = "0";

    (void)argument;
    if (protocol->device.version != NULL) version = protocol->device.version(protocol->device.ctx);
    reply(protocol, "VER %s\r\n", version);
    return FP_OK;
}

static FpStatus command_position(FocusProtocol *protocol, const char *argument)
{
    int32_t position = 0;

    (void)argument;
    if (protocol->device.position != NULL)
        position = protocol->device.position(protocol->device.ctx);
    reply(protocol, "POS %ld\r\n", (long)position);
    return FP_OK;
}

static FpStatus command_status(FocusProtocol *protocol, const char *argument)
{
    int32_t position = 0;
    int32_t rate = 0;
    int32_t voltage = -1;
    int32_t moving;

    (void)argument;
    if (protocol->device.is_moving == NULL) {
        reply(protocol, "ERR STATE\r\n");
        return FP_ERR_STATE;
    }
    moving = protocol->device.is_moving(protocol->device.ctx);
    if (protocol->device.position != NULL)
        position = protocol->device.position(protocol->device.ctx);
    if (protocol->device.rate != NULL) rate = protocol->device.rate(protocol->device.ctx);
    if (protocol->device.voltage_mv != NULL)
        voltage = protocol->device.voltage_mv(protocol->device.ctx);
    reply(protocol, "STATUS pos=%ld rate=%ld volt=%ld moving=%d\r\n",
          (long)position, (long)rate, (long)voltage, moving != 0);
    return FP_OK;
}

static FpStatus command_voltage(FocusProtocol *protocol, const char *argument)
{
    int32_t voltage = -1;

    (void)argument;
    if (protocol->device.voltage_mv != NULL)
        voltage = protocol->device.voltage_mv(protocol->device.ctx);
    if (voltage < 0) {
        reply(protocol, "ERR STATE\r\n");
        return FP_ERR_STATE;
    }
    reply(protocol, "VOLT %ld\r\n", (long)voltage);
    return FP_OK;
}

static FpStatus command_move(FocusProtocol *protocol, const char *argument)
{
    long steps = 0;

    if (parse_long(argument, &steps) != 0) {
        reply(protocol, "ERR RANGE\r\n");
        return FP_ERR_RANGE;
    }
    if (protocol->device.move_relative == NULL) {
        reply(protocol, "ERR STATE\r\n");
        return FP_ERR_STATE;
    }
    if (protocol->device.move_relative(protocol->device.ctx, (int32_t)steps) != 0) {
        reply(protocol, "ERR STATE\r\n");
        return FP_ERR_STATE;
    }
    reply(protocol, "OK\r\n");
    return FP_OK;
}

static FpStatus command_goto(FocusProtocol *protocol, const char *argument)
{
    long steps = 0;

    if (parse_long(argument, &steps) != 0) {
        reply(protocol, "ERR RANGE\r\n");
        return FP_ERR_RANGE;
    }
    if (protocol->device.move_absolute == NULL) {
        reply(protocol, "ERR STATE\r\n");
        return FP_ERR_STATE;
    }
    if (protocol->device.move_absolute(protocol->device.ctx, (int32_t)steps) != 0) {
        reply(protocol, "ERR STATE\r\n");
        return FP_ERR_STATE;
    }
    reply(protocol, "OK\r\n");
    return FP_OK;
}

static FpStatus command_halt(FocusProtocol *protocol, const char *argument)
{
    (void)argument;
    if (protocol->device.halt == NULL) {
        reply(protocol, "ERR STATE\r\n");
        return FP_ERR_STATE;
    }
    if (protocol->device.halt(protocol->device.ctx) != 0) {
        reply(protocol, "ERR STATE\r\n");
        return FP_ERR_STATE;
    }
    reply(protocol, "OK\r\n");
    return FP_OK;
}

static FpStatus command_set_rate(FocusProtocol *protocol, const char *argument)
{
    long rate = 0;

    if (parse_long(argument, &rate) != 0 || rate < 0) {
        reply(protocol, "ERR RANGE\r\n");
        return FP_ERR_RANGE;
    }
    if (protocol->device.set_rate == NULL) {
        reply(protocol, "ERR STATE\r\n");
        return FP_ERR_STATE;
    }
    if (protocol->device.set_rate(protocol->device.ctx, (int32_t)rate) != 0) {
        reply(protocol, "ERR STATE\r\n");
        return FP_ERR_STATE;
    }
    reply(protocol, "OK\r\n");
    return FP_OK;
}

static FpStatus command_get_rate(FocusProtocol *protocol, const char *argument)
{
    int32_t rate = 0;

    (void)argument;
    if (protocol->device.rate != NULL) rate = protocol->device.rate(protocol->device.ctx);
    reply(protocol, "RATE %ld\r\n", (long)rate);
    return FP_OK;
}

static FpStatus calibration(FocusProtocol *p, const char *argument, int action)
{
    if (*skip_spaces(argument) != '\0') { reply(p, "ERR RANGE\r\n"); return FP_ERR_RANGE; }
    if (!p->device.calibrate || p->device.calibrate(p->device.ctx, action)) {
        reply(p, "ERR STATE\r\n"); return FP_ERR_STATE;
    }
    reply(p, "OK\r\n"); return FP_OK;
}
static FpStatus command_zero(FocusProtocol *p, const char *a) { return calibration(p, a, 0); }
static FpStatus command_calbegin(FocusProtocol *p, const char *a) { return calibration(p, a, 1); }
static FpStatus command_maxset(FocusProtocol *p, const char *a) { return calibration(p, a, 2); }
static FpStatus command_limits(FocusProtocol *p, const char *a)
{
    if (*skip_spaces(a) != '\0') { reply(p, "ERR RANGE\r\n"); return FP_ERR_RANGE; }
    if (!p->device.calibration_state || !p->device.max_position) {
        reply(p, "ERR STATE\r\n"); return FP_ERR_STATE;
    }
    reply(p, "LIMIT state=%ld max=%ld\r\n", (long)p->device.calibration_state(p->device.ctx),
          (long)p->device.max_position(p->device.ctx)); return FP_OK;
}
static FpStatus command_jog(FocusProtocol *p, const char *a)
{
    long steps;
    if (parse_long(a, &steps) || steps == 0 || steps < -100 || steps > 100) {
        reply(p, "ERR RANGE\r\n"); return FP_ERR_RANGE;
    }
    if (!p->device.jog || p->device.jog(p->device.ctx, (int32_t)steps)) {
        reply(p, "ERR STATE\r\n"); return FP_ERR_STATE;
    }
    reply(p, "OK\r\n"); return FP_OK;
}

static FpStatus command_help(FocusProtocol *protocol, const char *argument)
{
    (void)argument;
    reply(protocol, "HELP PING VER? POS? STATUS? VOLT? MOVE GOTO HALT RATE RATE? ZERO CALBEGIN JOG MAXSET LIMIT?\r\n");
    return FP_OK;
}

static const FpCommand fp_commands[] = {
    { "PING", command_ping },
    { "VER?", command_version },
    { "VERSION?", command_version },
    { "POS?", command_position },
    { "POSITION?", command_position },
    { "STATUS?", command_status },
    { "VOLT?", command_voltage },
    { "MOVE", command_move },
    { "STEP", command_move },
    { "GOTO", command_goto },
    { "HALT", command_halt },
    { "STOP", command_halt },
    { "RATE", command_set_rate },
    { "RATE?", command_get_rate },
    { "HELP", command_help },
    { "ZERO", command_zero },
    { "CALBEGIN", command_calbegin },
    { "MAXSET", command_maxset },
    { "LIMIT?", command_limits },
    { "JOG", command_jog },
};

/* ------------------------------------------------------------------ dispatch */

void focus_protocol_init(FocusProtocol *protocol, const FocusDevice *device)
{
    if (protocol == NULL) return;
    memset(protocol, 0, sizeof *protocol);
    if (device != NULL) protocol->device = *device;
}

void focus_protocol_execute(FocusProtocol *protocol, const char *line)
{
    char keyword[32];
    const char *argument;
    size_t keyword_length = 0;
    size_t index;

    if (protocol == NULL || line == NULL) return;
    argument = skip_spaces(line);
    while (argument[keyword_length] != '\0' && argument[keyword_length] != ' ' &&
           argument[keyword_length] != '\t' && keyword_length + 1 < sizeof keyword) {
        keyword[keyword_length] = argument[keyword_length];
        keyword_length++;
    }
    keyword[keyword_length] = '\0';
    if (keyword_length == 0) return;
    argument = skip_spaces(argument + keyword_length);

    for (index = 0; index < sizeof fp_commands / sizeof fp_commands[0]; index++) {
        if (equals_ignore_case(keyword, fp_commands[index].keyword)) {
            fp_commands[index].handler(protocol, argument);
            return;
        }
    }
    reply(protocol, "ERR UNKNOWN\r\n");
}

void focus_protocol_feed(FocusProtocol *protocol, const char *data, size_t length)
{
    size_t index;

    if (protocol == NULL || data == NULL) return;
    for (index = 0; index < length; index++) {
        char byte = data[index];

        if (byte == '\n') {
            if (protocol->discarding) {
                protocol->discarding = 0;
                protocol->used = 0;
                continue;
            }
            protocol->line[protocol->used] = '\0';
            focus_protocol_execute(protocol, protocol->line);
            protocol->used = 0;
            continue;
        }
        if (byte == '\r') continue;
        if (protocol->discarding) continue;
        if (protocol->used + 1 >= sizeof protocol->line) {
            /* Over-long line: drop it whole instead of executing a fragment. */
            protocol->discarding = 1;
            protocol->used = 0;
            reply(protocol, "ERR LINE\r\n");
            continue;
        }
        protocol->line[protocol->used++] = byte;
    }
}
