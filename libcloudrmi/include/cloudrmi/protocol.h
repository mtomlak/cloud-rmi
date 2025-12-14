#ifndef CLOUDRMI_PROTOCOL_H
#define CLOUDRMI_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include "version.h"
#include "types.h"
#include "error.h"

typedef struct rmi_protocol_ctx rmi_protocol_ctx_t;

rmi_protocol_ctx_t *rmi_protocol_create(void);
void rmi_protocol_destroy(rmi_protocol_ctx_t *ctx);
void rmi_protocol_reset(rmi_protocol_ctx_t *ctx);

// Feed raw bytes into protocol engine (never blocks)
rmi_status_t rmi_protocol_feed(
    rmi_protocol_ctx_t *ctx,
    const uint8_t *data,
    size_t len,
    rmi_error_t *err
);

// Retrieve next parsed event, if available
rmi_status_t rmi_protocol_next_event(
    rmi_protocol_ctx_t *ctx,
    rmi_event_t *out,
    rmi_error_t *err
);

/*
 * Ownership and lifetime:
 * rmi_event_t carries fixed-size buffers for parsed values. Data written by
 * rmi_protocol_next_event() lives in the caller-provided structure and remains
 * valid until the caller overwrites it.
 */

#endif /* CLOUDRMI_PROTOCOL_H */
