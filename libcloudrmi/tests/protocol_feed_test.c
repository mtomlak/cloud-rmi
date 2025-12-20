#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cloudrmi/protocol.h>

#ifndef RMI_MAX_LINE
#define RMI_MAX_LINE RMI_BUF_SZ
#endif

#ifndef RMI_MAX_PAYLOAD
#define RMI_MAX_PAYLOAD (10 * 1024 * 1024)
#endif

static int expect_event(
    rmi_protocol_ctx_t *ctx,
    rmi_event_type_t expected_type,
    const char *expected_cmd,
    const char *expected_arg1,
    const char *expected_arg2,
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

    if (expected_cmd != NULL) {
        if (strcmp(event.cmd, expected_cmd) != 0) {
            printf("Unexpected cmd '%s'\n", event.cmd);
            return 0;
        }
    }

    if (expected_arg1 != NULL) {
        if (strcmp(event.arg1, expected_arg1) != 0) {
            printf("Unexpected arg1 '%s'\n", event.arg1);
            return 0;
        }
    }

    if (expected_arg2 != NULL) {
        if (strcmp(event.arg2, expected_arg2) != 0) {
            printf("Unexpected arg2 '%s'\n", event.arg2);
            return 0;
        }
    }

    if (expected_encoding != NULL) {
        if (strcmp(event.encoding, expected_encoding) != 0) {
            printf("Unexpected encoding '%s' for event #%d\n", event.encoding, event_counter);
            return 0;
        }
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
    if (rmi_protocol_feed(ctx, (const uint8_t *)chunk1, strlen(chunk1), &err) != RMI_OK) {
        printf("feed_chunks failed on first chunk (err %d)\n", err.code);
        return 0;
    }
    if (rmi_protocol_feed(ctx, (const uint8_t *)chunk2, strlen(chunk2), &err) != RMI_OK) {
        printf("feed_chunks failed on second chunk (err %d)\n", err.code);
        return 0;
    }
    return 1;
}

static int expect_overflow(rmi_protocol_ctx_t *ctx, const uint8_t *data, size_t len)
{
    rmi_error_t err;
    rmi_status_t status = rmi_protocol_feed(ctx, data, len, &err);
    if (status != RMI_E_OVERFLOW) {
        printf("Expected overflow, got %d\n", status);
        return 0;
    }
    return 1;
}

int main(void)
{
    rmi_protocol_ctx_t *ctx = rmi_protocol_create();
    rmi_event_t event;
    rmi_status_t status;
    if (ctx == NULL)
        return 1;

    if (!feed_chunks(ctx, "CALL f", "oo bar baz\n"))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "foo", "bar", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;

    /* Multiple commands in one buffer with CRLF and empty lines */
    const char *combo = "noop\r\n\nHELP\n";
    rmi_error_t err;
    if (rmi_protocol_feed(ctx, (const uint8_t *)combo, strlen(combo), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_NOOP, "noop", "", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_HELP, "help", "", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;

    /* Semantic error should surface via err while returning RMI_OK */
    const char *bad_call = "CALL only_one_arg\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)bad_call, strlen(bad_call), &err) != RMI_OK)
        return 1;
    status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_E_ERR || err.code != RMI_ERROR_BAD_ARGS)
        return 1;

    /* Framing overflow: line too long without newline */
    char long_line[RMI_BUF_SZ + 2];
    memset(long_line, 'a', sizeof(long_line));
    long_line[sizeof(long_line) - 1] = '\0';
    status = rmi_protocol_feed(ctx, (const uint8_t *)long_line, RMI_BUF_SZ + 1, &err);
    if (status != RMI_E_OVERFLOW)
        return 1;
    if (rmi_protocol_next_event(ctx, &event, &err) != RMI_E_AGAIN)
        return 1;

    /* Multiple commands in one buffer without chunking */
    const char *double_noop = "NOOP\nNOOP\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)double_noop, strlen(double_noop), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_NOOP, "noop", "", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_NOOP, "noop", "", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;

    /* CALL with payload */
    const char *call_with_payload = "PROTO 2\nCALL demo method PAYLOAD JSON 3\nabc";
    if (rmi_protocol_feed(ctx, (const uint8_t *)call_with_payload, strlen(call_with_payload), &err) != RMI_OK)
        return 1;
    const uint8_t expected_payload[] = { 'a', 'b', 'c' };
    if (!expect_event(ctx, RMI_EVENT_PROTO, "proto", "2", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "demo", "method", RMI_ERROR_NONE, "JSON", expected_payload, 3))
        return 1;

    /* PAYLOAD without CALL */
    const char *orphan_payload = "PAYLOAD RAW 2\nhi";
    if (rmi_protocol_feed(ctx, (const uint8_t *)orphan_payload, strlen(orphan_payload), &err) != RMI_OK)
        return 1;
    status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_OK || event.type != RMI_EVENT_UNKNOWN || err.code != RMI_ERROR_BAD_REQUEST)
        return 1;

    /* PAYLOAD framing also requires protocol upgrade */
    rmi_protocol_destroy(ctx);
    ctx = rmi_protocol_create();
    if (ctx == NULL) {
        printf("Failed to recreate protocol context\n");
        return 1;
    }
    const char *payload_without_proto = "CALL iface method PAYLOAD RAW 3\nxyz";
    status = rmi_protocol_feed(ctx, (const uint8_t *)payload_without_proto, strlen(payload_without_proto), &err);
    if (status == RMI_OK) {
        status = rmi_protocol_next_event(ctx, &event, &err);
        if (status != RMI_E_ERR || err.code != RMI_ERROR_BAD_REQUEST)
            return 1;
    } else if (status != RMI_E_ERR) {
        return 1;
    }

    /* Missing CALL arguments should error and reset */
    const char *bad_sequence = "CALL test iface\nNOOP\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)bad_sequence, strlen(bad_sequence), &err) != RMI_OK)
        return 1;
    status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_E_ERR || err.code != RMI_ERROR_BAD_ARGS)
        return 1;
    if (rmi_protocol_next_event(ctx, &event, &err) != RMI_E_AGAIN)
        return 1;

    /* CALL with zero-length payload */
    const char *empty_payload = "PROTO 2\nCALL zero method PAYLOAD PLAIN 0\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)empty_payload, strlen(empty_payload), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_PROTO, "proto", "2", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "zero", "method", RMI_ERROR_NONE, "PLAIN", (const uint8_t *)"", 0))
        return 1;

    /* CALL with legacy arg must not accept PAYLOAD */
    const char *legacy_then_payload = "CALL demo method Zm9v\nPAYLOAD RAW 1\nx";
    if (rmi_protocol_feed(ctx, (const uint8_t *)legacy_then_payload, strlen(legacy_then_payload), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "demo", "method", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_OK || event.type != RMI_EVENT_UNKNOWN || err.code != RMI_ERROR_BAD_REQUEST)
        return 1;

    /* Multiple CALLs in sequence, mixing legacy and payload */
    const char *mixed = "PROTO 2\nCALL first methodOne Zm9v\nCALL second methodTwo PAYLOAD RAW 4\nbody";
    if (rmi_protocol_feed(ctx, (const uint8_t *)mixed, strlen(mixed), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_PROTO, "proto", "2", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "first", "methodOne", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    const uint8_t body_payload[] = { 'b', 'o', 'd', 'y' };
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "second", "methodTwo", RMI_ERROR_NONE, "RAW", body_payload, sizeof(body_payload)))
        return 1;

    /* Repeated payload CALLs without reconnect */
    const char *double_payload = "PROTO 2\nCALL again method PAYLOAD RAW 1\na\nCALL again method PAYLOAD RAW 1\nb";
    if (rmi_protocol_feed(ctx, (const uint8_t *)double_payload, strlen(double_payload), &err) != RMI_OK)
        return 1;
    const uint8_t payload_a[] = { 'a' };
    const uint8_t payload_b[] = { 'b' };
    if (!expect_event(ctx, RMI_EVENT_PROTO, "proto", "2", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "again", "method", RMI_ERROR_NONE, "RAW", payload_a, sizeof(payload_a)))
        return 1;
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "again", "method", RMI_ERROR_NONE, "RAW", payload_b, sizeof(payload_b)))
        return 1;

    /* Malformed payload length */
    rmi_protocol_destroy(ctx);
    ctx = rmi_protocol_create();
    if (ctx == NULL) {
        printf("Failed to recreate protocol context\n");
        return 1;
    }
    const char *bad_length = "CALL iface method PAYLOAD RAW notnum\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)bad_length, strlen(bad_length), &err) != RMI_OK)
        return 1;
    status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_E_ERR || err.code != RMI_ERROR_BAD_REQUEST)
        return 1;

    /* Partial payload should request more data */
    rmi_protocol_set_version(ctx, 2);
    const char *short_payload = "CALL iface method PAYLOAD RAW 4\nxy";
    if (rmi_protocol_feed(ctx, (const uint8_t *)short_payload, strlen(short_payload), &err) != RMI_OK)
        return 1;
    if (rmi_protocol_next_event(ctx, &event, &err) != RMI_E_AGAIN)
        return 1;
    rmi_protocol_destroy(ctx);
    ctx = rmi_protocol_create();
    if (ctx == NULL) {
        printf("Failed to recreate protocol context\n");
        return 1;
    }

    /* Protocol negotiation is required before v2 framing */
    const char *v2_call_without_proto = "CALL iface method PAYLOAD RAW 3\nxyz";
    status = rmi_protocol_feed(ctx, (const uint8_t *)v2_call_without_proto, strlen(v2_call_without_proto), &err);
    if (status == RMI_OK) {
        status = rmi_protocol_next_event(ctx, &event, &err);
        if (status != RMI_E_ERR || err.code != RMI_ERROR_BAD_REQUEST)
            return 1;
    } else if (status != RMI_E_ERR) {
        return 1;
    }

    /* Invalid PROTO version */
    const char *bad_proto = "PROTO 99\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)bad_proto, strlen(bad_proto), &err) != RMI_OK)
        return 1;
    status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_E_ERR || err.code != RMI_ERROR_BAD_REQUEST)
        return 1;

    /* PROTO 2 enables single-line payload CALL */
    rmi_protocol_reset(ctx);
    const char *proto_ok = "PROTO 2\nCALL demo method PAYLOAD RAW 4\nbody";
    if (rmi_protocol_feed(ctx, (const uint8_t *)proto_ok, strlen(proto_ok), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_PROTO, "proto", "2", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    const uint8_t body_v2[] = { 'b', 'o', 'd', 'y' };
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "demo", "method", RMI_ERROR_NONE, "RAW", body_v2, sizeof(body_v2)))
        return 1;

    /* Legacy multi-line payload framing should be rejected */
    rmi_protocol_reset(ctx);
    const char *proto_and_payload = "PROTO 2\nCALL demo method\nPAYLOAD RAW 3\nbar";
    if (rmi_protocol_feed(ctx, (const uint8_t *)proto_and_payload, strlen(proto_and_payload), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_PROTO, "proto", "2", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_E_ERR || err.code != RMI_ERROR_BAD_ARGS)
        return 1;

    /* Exact RMI_MAX_LINE line length should be accepted */
    char *max_line = malloc(RMI_MAX_LINE + 2);
    if (max_line == NULL)
        return 1;
    memset(max_line, 'a', RMI_MAX_LINE);
    max_line[RMI_MAX_LINE] = '\n';
    max_line[RMI_MAX_LINE + 1] = '\0';
    if (rmi_protocol_feed(ctx, (const uint8_t *)max_line, RMI_MAX_LINE + 1, &err) != RMI_OK) {
        free(max_line);
        return 1;
    }
    free(max_line);
    if (!expect_event(ctx, RMI_EVENT_UNKNOWN, NULL, NULL, NULL, RMI_ERROR_BAD_REQUEST, NULL, NULL, 0))
        return 1;

    /* Boundary: allow payload exactly RMI_MAX_PAYLOAD bytes */
    rmi_protocol_reset(ctx);
    rmi_protocol_set_max_payload(ctx, RMI_MAX_PAYLOAD);
    const char *proto_payload_header = "PROTO 2\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)proto_payload_header, strlen(proto_payload_header), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_PROTO, "proto", "2", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    char header[128];
    snprintf(header, sizeof(header), "CALL demo method PAYLOAD RAW %zu\n", (size_t)RMI_MAX_PAYLOAD);
    if (rmi_protocol_feed(ctx, (const uint8_t *)header, strlen(header), &err) != RMI_OK)
        return 1;
    status = rmi_protocol_next_event(ctx, &event, &err);
    if (status != RMI_E_AGAIN)
        return 1;
    uint8_t *large_payload = malloc(RMI_MAX_PAYLOAD);
    if (large_payload == NULL)
        return 1;
    memset(large_payload, 'x', RMI_MAX_PAYLOAD);
    if (rmi_protocol_feed(ctx, large_payload, RMI_MAX_PAYLOAD, &err) != RMI_OK) {
        free(large_payload);
        return 1;
    }
    if (!expect_event(ctx, RMI_EVENT_CALL, "call", "demo", "method", RMI_ERROR_NONE, "RAW", large_payload, RMI_MAX_PAYLOAD)) {
        free(large_payload);
        return 1;
    }
    free(large_payload);

    /* Boundary: reject growth beyond RMI_MAX_PAYLOAD */
    rmi_protocol_reset(ctx);
    const char *proto_again = "PROTO 2\n";
    if (rmi_protocol_feed(ctx, (const uint8_t *)proto_again, strlen(proto_again), &err) != RMI_OK)
        return 1;
    if (!expect_event(ctx, RMI_EVENT_PROTO, "proto", "2", "", RMI_ERROR_NONE, NULL, NULL, 0))
        return 1;
    size_t over_limit = (size_t)RMI_MAX_PAYLOAD + 1;
    uint8_t *too_large = malloc(over_limit);
    if (too_large == NULL)
        return 1;
    memset(too_large, 'z', over_limit);
    if (!expect_overflow(ctx, too_large, over_limit)) {
        free(too_large);
        return 1;
    }
    free(too_large);

    rmi_protocol_destroy(ctx);
    printf("protocol_feed_test passed\n");
    return 0;
}
