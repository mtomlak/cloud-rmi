#ifndef CLOUDRMI_SEMANTICS_H
#define CLOUDRMI_SEMANTICS_H

#include <stddef.h>
#include "version.h"
#include "types.h"
#include "protocol.h"

#define CLOUDRMI_MAX_CAPABILITIES 32
#define CLOUDRMI_MAX_CAPABILITY_LEN 64
#define CLOUDRMI_MAX_ENUM_VALUES 64
#define CLOUDRMI_MAX_ENUM_TOKEN_LEN 128
#define CLOUDRMI_MAX_ENCODING_LEN 64

typedef struct {
    int status;
    char context[RMI_BUF_SZ + 1];
    int advertised_max_protocol;
    size_t capability_count;
    char capabilities[CLOUDRMI_MAX_CAPABILITIES][CLOUDRMI_MAX_CAPABILITY_LEN];
} cloudrmi_banner_info;

typedef enum {
    CLOUDRMI_RESPONSE_PLAIN = 0,
    CLOUDRMI_RESPONSE_JSON_INLINE,
    CLOUDRMI_RESPONSE_PAYLOAD
} cloudrmi_response_kind;

typedef struct {
    int status;
    cloudrmi_response_kind kind;
    char encoding[CLOUDRMI_MAX_ENCODING_LEN];
    size_t payload_len;
    char body[RMI_MAX_LINE + 1];
} cloudrmi_response_info;

typedef struct {
    size_t count;
    char values[CLOUDRMI_MAX_ENUM_VALUES][CLOUDRMI_MAX_ENUM_TOKEN_LEN];
} cloudrmi_enum_response;

int cloudrmi_parse_banner(const char *line, cloudrmi_banner_info *out);
int cloudrmi_parse_response_line(const char *line, cloudrmi_response_info *out);
int cloudrmi_parse_enum_response(const char *line, cloudrmi_enum_response *out);
int cloudrmi_protocol_validate_line(const char *line);
int cloudrmi_protocol_payload_allowed(const rmi_protocol_ctx_t *ctx, size_t payload_len);
size_t rmi_protocol_get_max_payload(const rmi_protocol_ctx_t *ctx);

#endif /* CLOUDRMI_SEMANTICS_H */
