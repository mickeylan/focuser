/*
 * focus_protocol.h - FP1, the line-oriented command protocol of the focuser.
 *
 * FP1 is deliberately small and text-based so that the same parser serves the
 * USB CDC virtual COM port and the USART3 header, and so that a host bridge can
 * speak it over either transport.
 *
 * Framing: one command per line, terminated by '\n' ('\r' is ignored).
 * Responses are one line terminated by "\r\n".
 */
#ifndef FOCUS_PROTOCOL_H
#define FOCUS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "../port/focus_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FP_MAX_LINE 128

typedef struct FocusProtocol {
    FocusDevice device;
    char line[FP_MAX_LINE];
    size_t used;
    /* Set while an over-long line is being discarded up to the next newline. */
    int discarding;
} FocusProtocol;

/* Prepare a protocol instance bound to one device. */
void focus_protocol_init(FocusProtocol *protocol, const FocusDevice *device);

/* Consume received bytes; complete lines are dispatched as they arrive. */
void focus_protocol_feed(FocusProtocol *protocol, const char *data, size_t length);

/* Dispatch a line that is already complete; used by tests and by the
 * line-oriented transports. The line must not contain the terminator. */
void focus_protocol_execute(FocusProtocol *protocol, const char *line);

#ifdef __cplusplus
}
#endif

#endif /* FOCUS_PROTOCOL_H */
