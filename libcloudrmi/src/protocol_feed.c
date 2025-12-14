#include <stdlib.h>
#include <string.h>
#include <cloudrmi/protocol.h>

struct rmi_protocol_ctx {
    uint8_t *buffer;
    size_t len;
    size_t cap;
    int has_pending_event;
    rmi_event_t pending_event;
};

static int protocol_parse_line_legacy(
    rmi_protocol_ctx_t *ctx,
    const char *line,
    rmi_event_t *out,
    rmi_error_t *err)
{
    extern int rmi_protocol_handle_line(
        rmi_protocol_ctx_t *ctx,
        const char *line,
        rmi_event_t *out_event,
        rmi_error_t *err);

    return rmi_protocol_handle_line(ctx, line, out, err);
}

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
    ctx->has_pending_event = 0;
    return ctx;
}

void rmi_protocol_destroy(rmi_protocol_ctx_t *ctx)
{
    if (ctx == NULL)
        return;

    free(ctx->buffer);
    free(ctx);
}

void rmi_protocol_reset(rmi_protocol_ctx_t *ctx)
{
    if (ctx == NULL)
        return;

    ctx->len = 0;
    ctx->has_pending_event = 0;
}

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

    if (len == 0) {
        ctx->has_pending_event = 0;
        return RMI_OK;
    }

    if (!ensure_capacity(ctx, ctx->len + len)) {
        if (err != NULL) {
            err->code = RMI_ERROR_BAD_REQUEST;
            err->message = "Buffer allocation failed";
        }
        return RMI_E_ERR;
    }

    memcpy(ctx->buffer + ctx->len, data, len);
    ctx->len += len;
    ctx->has_pending_event = 0;

    return RMI_OK;
}

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

    if (ctx->has_pending_event) {
        *out = ctx->pending_event;
        ctx->has_pending_event = 0;
        return RMI_OK;
    }

    while (1) {
        if (ctx->len == 0)
            return RMI_E_AGAIN;

        uint8_t *newline = memchr(ctx->buffer, '\n', ctx->len);
        if (newline == NULL) {
            if (ctx->len > RMI_BUF_SZ) {
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

        if (line_len > RMI_BUF_SZ) {
            if (err != NULL) {
                err->code = RMI_ERROR_BAD_REQUEST;
                err->message = "Line too long";
            }
            rmi_protocol_reset(ctx);
            return RMI_E_OVERFLOW;
        }

        ctx->buffer[line_end] = '\0';

        if (line_len == 0) {
            size_t remaining = ctx->len - consume;
            memmove(ctx->buffer, ctx->buffer + consume, remaining);
            ctx->len = remaining;
            continue;
        }

        ctx->buffer[line_len] = '\0';

        rmi_event_t event;
        int parsed = protocol_parse_line_legacy(ctx, (const char *)ctx->buffer, &event, err);

        size_t remaining = ctx->len - consume;
        memmove(ctx->buffer, ctx->buffer + consume, remaining);
        ctx->len = remaining;

        if (!parsed) {
            return RMI_E_ERR;
        }

        ctx->pending_event = event;
        ctx->has_pending_event = 1;

        break;
    }

    *out = ctx->pending_event;
    ctx->has_pending_event = 0;
    return RMI_OK;
}
