#ifndef RMI_PROTOCOL_LINE_H
#define RMI_PROTOCOL_LINE_H

#include <cloudrmi/protocol.h>

int rmi_protocol_handle_line(
    rmi_protocol_ctx_t *ctx,
    const char *line,
    rmi_event_t *out_event,
    rmi_error_t *err);

#endif /* RMI_PROTOCOL_LINE_H */
