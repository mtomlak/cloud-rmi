#include <stdio.h>
#include <string.h>

#include <cloudrmi/protocol.h>

static int expect_event(
    rmi_protocol_ctx_t *ctx,
    rmi_event_type_t expected_type,
    const char *expected_cmd,
    const char *expected_arg1)
{
    rmi_event_t event;
    rmi_error_t err;
    rmi_status_t status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_OK) {
        printf("Expected event but got status %d\n", status);
        return 0;
    }

    if (event.type != expected_type) {
        printf("Unexpected event type %d\n", event.type);
        return 0;
    }

    if (expected_cmd != NULL && strcmp(event.cmd, expected_cmd) != 0) {
        printf("Unexpected cmd '%s'\n", event.cmd);
        return 0;
    }

    if (expected_arg1 != NULL && strcmp(event.arg1, expected_arg1) != 0) {
        printf("Unexpected arg1 '%s'\n", event.arg1);
        return 0;
    }

    return 1;
}

static int feed_chunks(rmi_protocol_ctx_t *ctx, const char *chunk1, const char *chunk2)
{
    rmi_error_t err;
    if (rmi_protocol_feed(ctx, (const uint8_t *)chunk1, strlen(chunk1), &err) != RMI_OK)
        return 0;
    if (rmi_protocol_feed(ctx, (const uint8_t *)chunk2, strlen(chunk2), &err) != RMI_OK)
        return 0;
    return 1;
}

int main(void)
{
    rmi_protocol_ctx_t *ctx = rmi_protocol_create();
    if (ctx == NULL)
        return 1;

    if (!feed_chunks(ctx, "CALL f", "oo bar\n"))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "foo"))
        return 1;

    /* Multiple commands in one buffer with CRLF and empty lines */
    const char *combo = "noop\r\n\nHELP\n";
    rmi_error_t err;
    if (rmi_protocol_feed(ctx, (const uint8_t *)combo, strlen(combo), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_NOOP, "noop", ""))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_HELP, "help", ""))
        return 1;

    rmi_protocol_destroy(ctx);
    printf("protocol_feed_test passed\n");
    return 0;
}
