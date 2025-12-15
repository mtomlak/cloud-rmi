#include <stdio.h>
#include <string.h>

#include <cloudrmi/protocol.h>

static int expect_event(
    rmi_protocol_ctx_t *ctx,
    rmi_event_type_t expected_type,
    const char *expected_cmd,
    const char *expected_arg1,
    rmi_error_code_t expected_err,
    const char *expected_encoding,
    const uint8_t *expected_payload,
    size_t expected_payload_len)
{
    static int event_counter = 0;
    rmi_event_t event;
    rmi_error_t err;
    event_counter++;
    rmi_status_t status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_OK) {
        printf("Expected event #%d but got status %d (err %d)\n", event_counter, status, err.code);
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

    if (expected_encoding != NULL && strcmp(event.encoding, expected_encoding) != 0) {
        printf("Unexpected encoding '%s' for event #%d\n", event.encoding, event_counter);
        return 0;
    }

    if (expected_payload != NULL) {
        if (event.payload_len != expected_payload_len) {
            printf("Unexpected payload length %zu\n", event.payload_len);
            return 0;
        }
        if (memcmp(event.payload, expected_payload, expected_payload_len) != 0) {
            printf("Unexpected payload contents\n");
            return 0;
        }
    } else if (event.payload_len != 0) {
        printf("Expected no payload but got %zu bytes\n", event.payload_len);
        return 0;
    }

    if (err.code != expected_err) {
        printf("Unexpected error code %d\n", err.code);
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

    if (!feed_chunks(ctx, "CALL f", "oo bar baz\n"))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "foo", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;

    /* Multiple commands in one buffer with CRLF and empty lines */
    const char *combo = "noop\r\n\nHELP\n";
    rmi_error_t err;
    if (rmi_protocol_feed(ctx, (const uint8_t *)combo, strlen(combo), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_NOOP, "noop", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_HELP, "help", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;

    /* Semantic error should surface via err while returning RMI_OK */
    const char *bad_call = "CALL only_one_arg\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)bad_call, strlen(bad_call), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "only_one_arg", RMI_ERROR_BAD_ARGS, NULL, NULL, 0))
        return 1;

    /* Framing overflow: line too long without newline */
    char long_line[RMI_BUF_SZ + 2];
    memset(long_line, 'a', sizeof(long_line));
    long_line[sizeof(long_line) - 1] = '\0';
    rmi_status_t status = rmi_protocol_feed(ctx, (const uint8_t *)long_line, RMI_BUF_SZ + 1, &err);
    if (status != RMI_E_OVERFLOW)
        return 1;
    rmi_event_t event;
    if (rmi_protocol_next_event(ctx, &event, &err) != RMI_E_AGAIN)
        return 1;

    /* Multiple commands in one buffer without chunking */
    const char *double_noop = "NOOP\nNOOP\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)double_noop, strlen(double_noop), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_NOOP, "noop", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_NOOP, "noop", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;

    /* CALL with payload */
    const char *call_with_payload = "CALL demo method\nPAYLOAD JSON 3\nabc";
    if (rmi_protocol_feed(ctx, (const uint8_t *)call_with_payload, strlen(call_with_payload), &err) != RMI_OK)
        return 1;
    const uint8_t expected_payload[] = { 'a', 'b', 'c' };
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "demo", RMI_ERROR_NONE, "JSON", expected_payload, 3))
        return 1;

    /* PAYLOAD without CALL */
    const char *orphan_payload = "PAYLOAD RAW 2\nhi";
    if (rmi_protocol_feed(ctx, (const uint8_t *)orphan_payload, strlen(orphan_payload), &err) != RMI_OK)
        return 1;
    status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_E_ERR || err.code != RMI_ERROR_BAD_REQUEST)
        return 1;

    /* Invalid command while awaiting payload */
    const char *bad_sequence = "CALL test iface\nNOOP\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)bad_sequence, strlen(bad_sequence), &err) != RMI_OK)
        return 1;
    status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_E_ERR || err.code != RMI_ERROR_BAD_REQUEST)
        return 1;
    if (rmi_protocol_next_event(ctx, &event, &err) != RMI_E_AGAIN)
        return 1;

    /* CALL with zero-length payload */
    const char *empty_payload = "CALL zero method\nPAYLOAD PLAIN 0\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)empty_payload, strlen(empty_payload), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "zero", RMI_ERROR_NONE, "PLAIN", (const uint8_t *)"", 0))
        return 1;

    /* CALL with legacy arg must not accept PAYLOAD */
    const char *legacy_then_payload = "CALL demo method Zm9v\nPAYLOAD RAW 1\nx";
    if (rmi_protocol_feed(ctx, (const uint8_t *)legacy_then_payload, strlen(legacy_then_payload), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "demo", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_E_ERR || err.code != RMI_ERROR_BAD_REQUEST)
        return 1;

    rmi_protocol_destroy(ctx);
    printf("protocol_feed_test passed\n");
    return 0;
}
