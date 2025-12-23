#include <stdio.h>
#include <string.h>

#include <cloudrmi/semantics.h>

static int expect_int(int actual, int expected, const char *label)
{
    if (actual != expected) {
        printf("Unexpected %s: got %d expected %d\n", label, actual, expected);
        return 0;
    }
    return 1;
}

static int expect_size(size_t actual, size_t expected, const char *label)
{
    if (actual != expected) {
        printf("Unexpected %s: got %zu expected %zu\n", label, actual, expected);
        return 0;
    }
    return 1;
}

static int test_banner(void)
{
    cloudrmi_banner_info info;

    /* v2 banner with proto + caps */
    if (!cloudrmi_parse_banner("220 demo Server Ready EXT PROTO=2,stats,proxy", &info)) {
        printf("Failed to parse v2 banner\n");
        return 0;
    }
    if (info.status != 220)
        return 0;
    if (strcmp(info.context, "demo") != 0)
        return 0;
    if (info.advertised_max_protocol != 2)
        return 0;
    if (info.capability_count != 2)
        return 0;
    if (strcmp(info.capabilities[0], "STATS") != 0)
        return 0;
    if (strcmp(info.capabilities[1], "PROXY") != 0)
        return 0;

    /* v2 banner, proto only */
    if (!cloudrmi_parse_banner("220 demo EXT PROTO=2", &info)) {
        printf("Failed to parse proto-only banner\n");
        return 0;
    }
    if (info.advertised_max_protocol != 2 || info.capability_count != 0)
        return 0;

    /* v1 banner with EXT but no PROTO */
    if (!cloudrmi_parse_banner("220 demo Server Ready EXT SERVER-STATS", &info)) {
        printf("Failed to parse v1 EXT-only banner\n");
        return 0;
    }
    if (info.advertised_max_protocol != 1)
        return 0;
    if (info.capability_count != 1)
        return 0;
    if (strcmp(info.capabilities[0], "SERVER-STATS") != 0)
        return 0;

    /* v1 banner without EXT at all */
    if (!cloudrmi_parse_banner("220 demo Server Ready", &info)) {
        printf("Failed to parse legacy banner without EXT\n");
        return 0;
    }
    if (info.advertised_max_protocol != 1)
        return 0;
    if (info.capability_count != 0)
        return 0;

    /* invalid banners */
    if (cloudrmi_parse_banner("220", &info)) {
        printf("Accepted malformed banner\n");
        return 0;
    }
    if (cloudrmi_parse_banner("220 demo EXT PROTO=0", &info)) {
        printf("Accepted invalid protocol version\n");
        return 0;
    }
    if (cloudrmi_parse_banner("220 demo EXT PROTO=999", &info)) {
        printf("Accepted out-of-range protocol\n");
        return 0;
    }

    return 1;
}

static int test_response_parsing(void)
{
    cloudrmi_response_info resp;

    if (!cloudrmi_parse_response_line("200 OK", &resp))
        return 0;
    if (!expect_int(resp.status, 200, "plain status"))
        return 0;
    if (!expect_int(resp.kind, CLOUDRMI_RESPONSE_PLAIN, "plain kind"))
        return 0;
    if (strcmp(resp.body, "OK") != 0) {
        printf("Unexpected plain body '%s'\n", resp.body);
        return 0;
    }

    if (!cloudrmi_parse_response_line("200 JSON [\"a\"]", &resp))
        return 0;
    if (!expect_int(resp.kind, CLOUDRMI_RESPONSE_JSON_INLINE, "json kind"))
        return 0;

    if (!cloudrmi_parse_response_line("200 PAYLOAD RAW 4", &resp))
        return 0;
    if (!expect_int(resp.kind, CLOUDRMI_RESPONSE_PAYLOAD, "payload kind"))
        return 0;
    if (strcmp(resp.encoding, "RAW") != 0) {
        printf("Unexpected payload encoding '%s'\n", resp.encoding);
        return 0;
    }
    if (!expect_size(resp.payload_len, 4, "payload length"))
        return 0;

    return 1;
}

static int test_enum_parsing(void)
{
    cloudrmi_enum_response result;
    const char *line = "200 JSON [\"caps\", \"stats\"]";

    if (!cloudrmi_parse_enum_response(line, &result)) {
        printf("Failed to parse enumeration\n");
        return 0;
    }

    if (!expect_size(result.count, 2, "enum count"))
        return 0;
    if (strcmp(result.values[0], "CAPS") != 0 || strcmp(result.values[1], "STATS") != 0) {
        printf("Unexpected enum values '%s','%s'\n", result.values[0], result.values[1]);
        return 0;
    }

    if (cloudrmi_parse_enum_response("200 OK", &result)) {
        printf("Accepted invalid enum response\n");
        return 0;
    }

    return 1;
}

static int test_limits(void)
{
    const char *valid = "200 OK";
    if (!cloudrmi_protocol_validate_line(valid))
        return 0;

    const char *invalid = "200 BAD\nLINE";
    if (cloudrmi_protocol_validate_line(invalid))
        return 0;

    rmi_protocol_ctx_t *ctx = rmi_protocol_create();
    if (ctx == NULL)
        return 0;

    rmi_protocol_set_max_payload(ctx, 8);

    int allowed = cloudrmi_protocol_payload_allowed(ctx, 4);
    int rejected = cloudrmi_protocol_payload_allowed(ctx, 16);

    rmi_protocol_destroy(ctx);

    if (!allowed || rejected)
        return 0;

    return 1;
}

int main(void)
{
    if (!test_banner())
        return 1;
    if (!test_response_parsing())
        return 1;
    if (!test_enum_parsing())
        return 1;
    if (!test_limits())
        return 1;

    printf("All semantics tests passed\n");
    return 0;
}
