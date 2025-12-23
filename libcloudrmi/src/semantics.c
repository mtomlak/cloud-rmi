#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <cloudrmi/semantics.h>

#include "protocol_line.h"

static int is_valid_context_char(char c)
{
    return isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-';
}

static void normalize_upper(char *str)
{
    if (str == NULL)
        return;

    for (; *str; ++str)
        *str = (char)toupper((unsigned char)*str);
}

int cloudrmi_protocol_validate_line(const char *line)
{
    if (line == NULL)
        return 0;

    size_t len = strlen(line);

    if (len > RMI_MAX_LINE)
        return 0;

    for (size_t i = 0; i < len; i++) {
        if (line[i] == '\n' || line[i] == '\r')
            return 0;
    }

    return 1;
}

static int parse_status_and_context(const char *line, int *status, char *context, size_t context_len, const char **ext_out)
{
    const char *cursor = line;

    if (!cloudrmi_protocol_validate_line(line))
        return 0;

    while (*cursor == ' ')
        cursor++;

    if (!isdigit((unsigned char)*cursor))
        return 0;

    char *endptr = NULL;
    long code = strtol(cursor, &endptr, 10);
    if (endptr == cursor || code != 220)
        return 0;

    *status = (int)code;
    cursor = endptr;

    if (*cursor != ' ')
        return 0;
    while (*cursor == ' ')
        cursor++;

    size_t ctx_len = 0;
    while (cursor[ctx_len] != '\0' && cursor[ctx_len] != ' ') {
        if (!is_valid_context_char(cursor[ctx_len]))
            return 0;
        ctx_len++;
    }

    if (ctx_len == 0 || ctx_len >= context_len)
        return 0;

    memcpy(context, cursor, ctx_len);
    context[ctx_len] = '\0';

    cursor += ctx_len;
    while (*cursor == ' ')
        cursor++;

    *ext_out = cursor;
    return 1;
}

static int parse_capability_token(const char *start, size_t len, char *dst, size_t dst_len)
{
    if (len == 0 || len >= dst_len)
        return 0;

    for (size_t i = 0; i < len; i++) {
        char c = start[i];
        if (!(c == '_' || c == '-' || isalnum((unsigned char)c)))
            return 0;
        dst[i] = (char)toupper((unsigned char)c);
    }
    dst[len] = '\0';
    return 1;
}

int cloudrmi_parse_banner(const char *line, cloudrmi_banner_info *out)
{
    if (line == NULL || out == NULL)
        return 0;

    memset(out, 0, sizeof(*out));

    int status = 0;
    const char *rest = NULL;

    if (!parse_status_and_context(line, &status,
                                  out->context, sizeof(out->context),
                                  &rest))
        return 0;

    out->status = status;

    /* Default: legacy v1 */
    out->advertised_max_protocol = 1;
    out->capability_count = 0;

    if (rest == NULL || *rest == '\0')
        return 1;

    /* Find EXT as a standalone token */
    const char *p = rest;
    const char *ext = NULL;

    while (*p) {
        if ((p == rest || isspace((unsigned char)p[-1])) &&
            strncmp(p, "EXT", 3) == 0 &&
            (p[3] == '\0' || isspace((unsigned char)p[3]))) {
            ext = p + 3;
            break;
        }
        p++;
    }

    /* No EXT → legacy v1 */
    if (ext == NULL)
        return 1;

    while (*ext && isspace((unsigned char)*ext))
        ext++;

    /* EXT present but empty → invalid */
    if (*ext == '\0')
        return 0;

    /* Parse comma-separated EXT tokens */
    const char *cursor = ext;

    while (*cursor) {
        const char *start = cursor;

        while (*cursor && *cursor != ',')
            cursor++;

        size_t len = (size_t)(cursor - start);
        if (len == 0)
            return 0;

        /* PROTO token */
        if (len >= 6 && strncasecmp(start, "PROTO=", 6) == 0) {
            const char *num = start + 6;
            char *endptr = NULL;

            long v = strtol(num, &endptr, 10);
            if (endptr == num || endptr != start + len)
                return 0;

            if (v < 1 || v > RMI_MAX_PROTOCOL_VERSION)
                return 0;

            out->advertised_max_protocol = (int)v;
        }
        /* Capability token */
        else {
            if (out->capability_count >= CLOUDRMI_MAX_CAPABILITIES)
                return 0;

            if (!parse_capability_token(
                    start, len,
                    out->capabilities[out->capability_count],
                    CLOUDRMI_MAX_CAPABILITY_LEN))
                return 0;

            out->capability_count++;
        }

        if (*cursor == ',')
            cursor++;
    }

    return 1;
}

int cloudrmi_parse_response_line(const char *line, cloudrmi_response_info *out)
{
    if (line == NULL || out == NULL)
        return 0;

    if (!cloudrmi_protocol_validate_line(line))
        return 0;

    memset(out, 0, sizeof(*out));
    out->kind = CLOUDRMI_RESPONSE_PLAIN;

    const char *cursor = line;
    while (*cursor == ' ')
        cursor++;

    if (!isdigit((unsigned char)*cursor))
        return 0;

    char *endptr = NULL;
    long status = strtol(cursor, &endptr, 10);
    if (endptr == cursor)
        return 0;

    out->status = (int)status;
    cursor = endptr;

    while (*cursor == ' ')
        cursor++;

    if (*cursor == '\0') {
        out->body[0] = '\0';
        return 1;
    }

    if (strncasecmp(cursor, "JSON", 4) == 0 && (cursor[4] == '\0' || isspace((unsigned char)cursor[4]))) {
        cursor += 4;
        while (*cursor == ' ')
            cursor++;
        out->kind = CLOUDRMI_RESPONSE_JSON_INLINE;
        strncpy(out->body, cursor, sizeof(out->body) - 1);
        out->body[sizeof(out->body) - 1] = '\0';
        return 1;
    }

    if (strncasecmp(cursor, "PAYLOAD", 7) == 0 && (cursor[7] == '\0' || isspace((unsigned char)cursor[7]))) {
        cursor += 7;
        while (*cursor == ' ')
            cursor++;

        if (*cursor == '\0')
            return 0;

        size_t enc_len = 0;
        while (cursor[enc_len] != '\0' && !isspace((unsigned char)cursor[enc_len])) {
            if (enc_len + 1 >= sizeof(out->encoding))
                return 0;
            out->encoding[enc_len] = cursor[enc_len];
            enc_len++;
        }
        out->encoding[enc_len] = '\0';

        cursor += enc_len;
        while (*cursor == ' ')
            cursor++;

        if (*cursor == '\0')
            return 0;

        unsigned long long len_val = strtoull(cursor, &endptr, 10);
        if (endptr == cursor || (*endptr != '\0' && !isspace((unsigned char)*endptr)))
            return 0;

        if (len_val > SIZE_MAX)
            return 0;

        out->payload_len = (size_t)len_val;
        out->kind = CLOUDRMI_RESPONSE_PAYLOAD;
        return 1;
    }

    out->kind = CLOUDRMI_RESPONSE_PLAIN;
    strncpy(out->body, cursor, sizeof(out->body) - 1);
    out->body[sizeof(out->body) - 1] = '\0';
    return 1;
}

static void trim_whitespace(char *str)
{
    size_t len;
    char *start = str;
    char *end;

    if (str == NULL)
        return;

    while (*start && isspace((unsigned char)*start))
        start++;

    len = strlen(start);
    end = start + len;
    while (end > start && isspace((unsigned char)end[-1]))
        end--;

    memmove(str, start, (size_t)(end - start));
    str[end - start] = '\0';
}

int cloudrmi_parse_enum_response(const char *line, cloudrmi_enum_response *out)
{
    if (line == NULL || out == NULL)
        return 0;

    cloudrmi_response_info resp;
    if (!cloudrmi_parse_response_line(line, &resp))
        return 0;

    if (resp.status != 200 || resp.kind != CLOUDRMI_RESPONSE_JSON_INLINE)
        return 0;

    memset(out, 0, sizeof(*out));

    const char *cursor = resp.body;
    while (*cursor == ' ')
        cursor++;

    if (*cursor != '[')
        return 0;
    cursor++;

    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor))
            cursor++;

        if (*cursor == ']')
            return 1;

        if (*cursor != '\"')
            return 0;
        cursor++;

        char token[CLOUDRMI_MAX_ENUM_TOKEN_LEN];
        size_t len = 0;

        while (*cursor != '\0' && *cursor != '\"') {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor++;
            }

            if (len + 1 >= sizeof(token))
                return 0;

            token[len++] = *cursor;
            cursor++;
        }

        if (*cursor != '\"')
            return 0;
        cursor++;
        token[len] = '\0';

        trim_whitespace(token);
        normalize_upper(token);

        if (token[0] == '\0')
            return 0;

        if (out->count >= CLOUDRMI_MAX_ENUM_VALUES)
            return 0;

        strncpy(out->values[out->count], token, CLOUDRMI_MAX_ENUM_TOKEN_LEN - 1);
        out->values[out->count][CLOUDRMI_MAX_ENUM_TOKEN_LEN - 1] = '\0';
        out->count++;

        while (isspace((unsigned char)*cursor))
            cursor++;

        if (*cursor == ',') {
            cursor++;
            continue;
        }

        if (*cursor == ']')
            return 1;

        if (*cursor == '\0')
            break;

        return 0;
    }

    return 0;
}

int cloudrmi_protocol_payload_allowed(const rmi_protocol_ctx_t *ctx, size_t payload_len)
{
    if (ctx == NULL)
        return 0;

    extern size_t rmi_protocol_get_max_payload(const rmi_protocol_ctx_t *ctx);
    size_t max_payload = rmi_protocol_get_max_payload(ctx);
    return payload_len <= max_payload;
}
