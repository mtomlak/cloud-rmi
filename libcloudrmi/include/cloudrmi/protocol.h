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
 * rmi_event_t fields returned by rmi_protocol_next_event() remain valid until
 * the next call to rmi_protocol_feed(), rmi_protocol_reset(), or destruction
 * of the corresponding rmi_protocol_ctx_t. Callers must copy event data if it
 * needs to outlive those operations.
 */

#endif /* CLOUDRMI_PROTOCOL_H */
