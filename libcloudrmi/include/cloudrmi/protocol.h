#ifndef CLOUDRMI_PROTOCOL_H
#define CLOUDRMI_PROTOCOL_H

#include "version.h"
#include "types.h"
#include "error.h"

int rmi_protocol_handle_line(rmi_protocol_ctx_t *ctx, const char *line, rmi_event_t *out_event, rmi_error_t *err);

#endif /* CLOUDRMI_PROTOCOL_H */
