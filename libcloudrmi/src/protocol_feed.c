#include <stdlib.h>
#include <string.h>

#include <cloudrmi/protocol.h>

#include "protocol_line.h"

#define RMI_MAX_LINE RMI_BUF_SZ
#define RMI_MAX_BUFFER (RMI_BUF_SZ * 8)

struct rmi_protocol_ctx {
    uint8_t *buffer;
    size_t len;
    size_t cap;
    rmi_protocol_state_t state;
    size_t expected_payload_len;
    char pending_iface[RMI_BUF_SZ + 1];
    char pending_method[RMI_BUF_SZ + 1];
    char pending_encoding[RMI_BUF_SZ + 1];
    uint8_t *payload_buf;
    size_t payload_cap;
};

static int ensure_capacity(rmi_protocol_ctx_t *ctx, size_t needed)
{
    if (ctx->cap >= needed)
        return 1;

    size_t new_cap = ctx->cap ? ctx->cap : (size_t)RMI_BUF_SZ;
    while (new_cap < needed)
        new_cap *= 2;

    uint8_t *new_buf = realloc(ctx->buffer, new_cap);
    if (new_buf == NULL)
        return 0;

    ctx->buffer = new_buf;
    ctx->cap = new_cap;
    return 1;
}

static int ensure_payload_capacity(rmi_protocol_ctx_t *ctx, size_t needed)
{
    if (ctx->payload_cap >= needed)
        return 1;

    size_t new_cap = ctx->payload_cap ? ctx->payload_cap : (size_t)RMI_BUF_SZ;
    while (new_cap < needed)
        new_cap *= 2;

    uint8_t *new_buf = realloc(ctx->payload_buf, new_cap);
    if (new_buf == NULL)
        return 0;

    ctx->payload_buf = new_buf;
    ctx->payload_cap = new_cap;
    return 1;
}

static void reset_pending_call(rmi_protocol_ctx_t *ctx)
{
    if (ctx == NULL)
        return;

    memset(ctx->pending_iface, 0, sizeof(ctx->pending_iface));
    memset(ctx->pending_method, 0, sizeof(ctx->pending_method));
    memset(ctx->pending_encoding, 0, sizeof(ctx->pending_encoding));
    ctx->expected_payload_len = 0;
}

rmi_protocol_ctx_t *rmi_protocol_create(void)
{
    rmi_protocol_ctx_t *ctx = calloc(1, sizeof(rmi_protocol_ctx_t));
    if (ctx == NULL)
        return NULL;

    ctx->cap = RMI_BUF_SZ;
    ctx->buffer = malloc(ctx->cap);
    if (ctx->buffer == NULL) {
        free(ctx);
        return NULL;
    }

    ctx->len = 0;
    ctx->state = RMI_PROTOCOL_IDLE;
    ctx->expected_payload_len = 0;
    ctx->payload_buf = NULL;
    ctx->payload_cap = 0;
    reset_pending_call(ctx);
    return ctx;
}

void rmi_protocol_destroy(rmi_protocol_ctx_t *ctx)
{
    if (ctx == NULL)
        return;

    free(ctx->buffer);
    free(ctx->payload_buf);
    free(ctx);
}

void rmi_protocol_reset(rmi_protocol_ctx_t *ctx)
{
    if (ctx == NULL)
        return;

    ctx->len = 0;
    ctx->state = RMI_PROTOCOL_IDLE;
    reset_pending_call(ctx);
}

/*
 * Overflow policy:
 *
 * - RMI_MAX_BUFFER: hard ceiling for accumulated buffered bytes. If exceeded
 *   at any point, the context is reset and RMI_E_OVERFLOW is returned.
 * - RMI_MAX_LINE: maximum allowed size for a single protocol line before a
 *   newline is seen. If no newline is present and buffered data grows past
 *   this limit, the context is reset and RMI_E_OVERFLOW is returned.
 */
rmi_status_t rmi_protocol_feed(
    rmi_protocol_ctx_t *ctx,
    const uint8_t *data,
    size_t len,
    rmi_error_t *err)
{
    if (err != NULL) {
        err->code = RMI_ERROR_NONE;
        err->message = NULL;
    }

    if (ctx == NULL || data == NULL) {
        if (err != NULL) {
            err->code = RMI_ERROR_BAD_REQUEST;
            err->message = "Invalid arguments";
        }
        return RMI_E_ERR;
    }

    if (len == 0)
        return RMI_OK;

    size_t needed = ctx->len + len;

    if (needed > RMI_MAX_BUFFER) {
        if (err != NULL) {
            err->code = RMI_ERROR_BAD_REQUEST;
            err->message = "Buffer overflow";
        }
        rmi_protocol_reset(ctx);
        return RMI_E_OVERFLOW;
    }

    if (!ensure_capacity(ctx, needed)) {
        if (err != NULL) {
            err->code = RMI_ERROR_BAD_REQUEST;
            err->message = "Buffer allocation failed";
        }
        return RMI_E_ERR;
    }

    memcpy(ctx->buffer + ctx->len, data, len);
    ctx->len = needed;

    if (ctx->state != RMI_PROTOCOL_READING_PAYLOAD &&
        memchr(ctx->buffer, '\n', ctx->len) == NULL &&
        ctx->len > RMI_MAX_LINE) {
        if (err != NULL) {
            err->code = RMI_ERROR_BAD_REQUEST;
            err->message = "Line too long";
        }
        rmi_protocol_reset(ctx);
        return RMI_E_OVERFLOW;
    }

    return RMI_OK;
}


// Retrieve next parsed event, if available.
//
// Status codes communicate transport/framing state (e.g. RMI_E_OVERFLOW or
// RMI_E_ERR). Protocol semantic issues are surfaced through err->code even when
// the returned status is RMI_OK, as long as a syntactically valid event was
// parsed.
rmi_status_t rmi_protocol_next_event(
    rmi_protocol_ctx_t *ctx,
    rmi_event_t *out,
    rmi_error_t *err)
{
    if (err != NULL) {
        err->code = RMI_ERROR_NONE;
        err->message = NULL;
    }

    if (ctx == NULL || out == NULL) {
        if (err != NULL) {
            err->code = RMI_ERROR_BAD_REQUEST;
            err->message = "Invalid arguments";
        }
        return RMI_E_ERR;
    }

    while (1) {
        if (ctx->state == RMI_PROTOCOL_READING_PAYLOAD) {
            if (ctx->len < ctx->expected_payload_len)
                return RMI_E_AGAIN;

            if (!ensure_payload_capacity(ctx, ctx->expected_payload_len + 1)) {
                if (err != NULL) {
                    err->code = RMI_ERROR_BAD_REQUEST;
                    err->message = "Buffer allocation failed";
                }
                rmi_protocol_reset(ctx);
                return RMI_E_ERR;
            }

            memcpy(ctx->payload_buf, ctx->buffer, ctx->expected_payload_len);
            ctx->payload_buf[ctx->expected_payload_len] = '\0';

            memset(out, 0, sizeof(*out));
            strncpy(out->cmd, "call", RMI_BUF_SZ);
            strncpy(out->arg1, ctx->pending_iface, RMI_BUF_SZ);
            strncpy(out->arg2, ctx->pending_method, RMI_BUF_SZ);
            strncpy(out->encoding, ctx->pending_encoding, RMI_BUF_SZ);
            out->argc = 3;
            out->type = RMI_EVENT_CALL;
            out->payload = ctx->payload_buf;
            out->payload_len = ctx->expected_payload_len;

            size_t remaining_after_payload = ctx->len - ctx->expected_payload_len;
            memmove(ctx->buffer, ctx->buffer + ctx->expected_payload_len, remaining_after_payload);
            ctx->len = remaining_after_payload;

            ctx->state = RMI_PROTOCOL_EXECUTE_CALL;
            ctx->state = RMI_PROTOCOL_SEND_RESPONSE;
            ctx->state = RMI_PROTOCOL_IDLE;
            reset_pending_call(ctx);
            return RMI_OK;
        }

        if (ctx->len == 0)
            return RMI_E_AGAIN;

        uint8_t *newline = memchr(ctx->buffer, '\n', ctx->len);
        if (newline == NULL) {
            if (ctx->len > RMI_MAX_LINE && ctx->state == RMI_PROTOCOL_IDLE) {
                if (err != NULL) {
                    err->code = RMI_ERROR_BAD_REQUEST;
                    err->message = "Line too long";
                }
                rmi_protocol_reset(ctx);
                return RMI_E_OVERFLOW;
            }
            return RMI_E_AGAIN;
        }

        size_t line_end = (size_t)(newline - ctx->buffer);
        size_t consume = line_end + 1;
        size_t line_len = line_end;

        if (line_end > 0 && ctx->buffer[line_end - 1] == '\r')
            line_len--;

        if (line_len > RMI_MAX_LINE) {
            if (err != NULL) {
                err->code = RMI_ERROR_BAD_REQUEST;
                err->message = "Line too long";
            }
            rmi_protocol_reset(ctx);
            return RMI_E_OVERFLOW;
        }

        ctx->buffer[line_len] = '\0';

        if (line_len == 0) {
            size_t remaining = ctx->len - consume;
            memmove(ctx->buffer, ctx->buffer + consume, remaining);
            ctx->len = remaining;
            continue;
        }

        ctx->buffer[line_len] = '\0';

        rmi_event_t parsed_event;
        if (!rmi_protocol_handle_line(ctx, (const char *)ctx->buffer, &parsed_event, err)) {
            rmi_protocol_reset(ctx);
            return RMI_E_ERR;
        }

        size_t remaining = ctx->len - consume;
        memmove(ctx->buffer, ctx->buffer + consume, remaining);
        ctx->len = remaining;

        if (ctx->state == RMI_PROTOCOL_CALL_PENDING_PAYLOAD) {
            if (parsed_event.type != RMI_EVENT_PAYLOAD) {
                if (err != NULL) {
                    err->code = RMI_ERROR_BAD_REQUEST;
                    err->message = "Expected PAYLOAD after CALL";
                }
                rmi_protocol_reset(ctx);
                ctx->state = RMI_PROTOCOL_IDLE;
                return RMI_E_ERR;
            }

            if (parsed_event.arg2[0] == '\0') {
                if (err != NULL) {
                    err->code = RMI_ERROR_BAD_ARGS;
                    err->message = "PAYLOAD length missing";
                }
                rmi_protocol_reset(ctx);
                return RMI_E_ERR;
            }

            char *endptr = NULL;
            unsigned long long payload_len = strtoull(parsed_event.arg2, &endptr, 10);
            if (endptr == parsed_event.arg2 || payload_len > RMI_MAX_BUFFER) {
                if (err != NULL) {
                    err->code = RMI_ERROR_BAD_REQUEST;
                    err->message = "Invalid payload length";
                }
                rmi_protocol_reset(ctx);
                return payload_len > RMI_MAX_BUFFER ? RMI_E_OVERFLOW : RMI_E_ERR;
            }

            ctx->expected_payload_len = (size_t)payload_len;
            strncpy(ctx->pending_encoding, parsed_event.arg1, RMI_BUF_SZ);
            ctx->state = RMI_PROTOCOL_READING_PAYLOAD;

            if (ctx->expected_payload_len == 0) {
                if (!ensure_payload_capacity(ctx, 1)) {
                    if (err != NULL) {
                        err->code = RMI_ERROR_BAD_REQUEST;
                        err->message = "Buffer allocation failed";
                    }
                    rmi_protocol_reset(ctx);
                    return RMI_E_ERR;
                }
                ctx->payload_buf[0] = '\0';
                out->payload = ctx->payload_buf;
                out->payload_len = 0;
                strncpy(out->cmd, "call", RMI_BUF_SZ);
                strncpy(out->arg1, ctx->pending_iface, RMI_BUF_SZ);
                strncpy(out->arg2, ctx->pending_method, RMI_BUF_SZ);
                strncpy(out->encoding, ctx->pending_encoding, RMI_BUF_SZ);
                out->type = RMI_EVENT_CALL;
                out->argc = 3;
                ctx->state = RMI_PROTOCOL_EXECUTE_CALL;
                ctx->state = RMI_PROTOCOL_SEND_RESPONSE;
                ctx->state = RMI_PROTOCOL_IDLE;
                reset_pending_call(ctx);
                return RMI_OK;
            }

            continue;
        }

        if (parsed_event.type == RMI_EVENT_CALL && parsed_event.argc == 3 && parsed_event.arg3[0] == '\0') {
            strncpy(ctx->pending_iface, parsed_event.arg1, RMI_BUF_SZ);
            strncpy(ctx->pending_method, parsed_event.arg2, RMI_BUF_SZ);
            ctx->state = RMI_PROTOCOL_CALL_PENDING_PAYLOAD;
            continue;
        }

        if (parsed_event.type == RMI_EVENT_PAYLOAD) {
            if (err != NULL) {
                err->code = RMI_ERROR_BAD_REQUEST;
                err->message = "PAYLOAD without CALL";
            }
            rmi_protocol_reset(ctx);
            return RMI_E_ERR;
        }

        *out = parsed_event;
        ctx->state = RMI_PROTOCOL_EXECUTE_CALL;
        ctx->state = RMI_PROTOCOL_SEND_RESPONSE;
        ctx->state = RMI_PROTOCOL_IDLE;

        return RMI_OK;
    }
}
