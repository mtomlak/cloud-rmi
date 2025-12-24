#ifndef CLOUDRMI_TYPES_H
#define CLOUDRMI_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include "version.h"

typedef enum {
    RMI_EVENT_NONE = 0,
    RMI_EVENT_AUTH,
    RMI_EVENT_CALL,
    RMI_EVENT_HELP,
    RMI_EVENT_IFACES,
    RMI_EVENT_TIME,
    RMI_EVENT_PING,
    RMI_EVENT_QUIT,
    RMI_EVENT_PROXY,
    RMI_EVENT_CAPS,
    RMI_EVENT_SERVER,
    RMI_EVENT_PROTO,
    RMI_EVENT_INFO,
    RMI_EVENT_UNKNOWN
} rmi_event_type_t;

typedef enum {
    RMI_PROTOCOL_IDLE = 0,
    RMI_PROTOCOL_READING_PAYLOAD,
    RMI_PROTOCOL_EXECUTE_CALL,
    RMI_PROTOCOL_SEND_RESPONSE
} rmi_protocol_state_t;

typedef enum {
    RMI_ENCODING_NONE = 0,
    RMI_ENCODING_BASE64
} rmi_encoding_t;

typedef enum {
    RMI_OK = 0,
    RMI_E_AGAIN,
    RMI_E_OVERFLOW,
    RMI_E_ERR
} rmi_status_t;

typedef struct {
    rmi_event_type_t type;
    int argc;
    char cmd[RMI_BUF_SZ + 1];
    char arg1[RMI_BUF_SZ + 1];
    char arg2[RMI_BUF_SZ + 1];
    char arg3[RMI_BUF_SZ + 1];
    char arg4[RMI_BUF_SZ + 1];
    char arg5[RMI_BUF_SZ + 1];
    char encoding[RMI_BUF_SZ + 1];
    const uint8_t *payload;
    size_t payload_len;
} rmi_event_t;

#endif /* CLOUDRMI_TYPES_H */
