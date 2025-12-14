#ifndef CLOUDRMI_TYPES_H
#define CLOUDRMI_TYPES_H

#include <stddef.h>
#include "version.h"

typedef enum {
    RMI_EVENT_NONE = 0,
    RMI_EVENT_AUTH,
    RMI_EVENT_CALL,
    RMI_EVENT_HELP,
    RMI_EVENT_IFACES,
    RMI_EVENT_RELOAD,
    RMI_EVENT_SHUTDOWN,
    RMI_EVENT_TIME,
    RMI_EVENT_UPTIME,
    RMI_EVENT_NOOP,
    RMI_EVENT_DEBUG,
    RMI_EVENT_QUIT,
    RMI_EVENT_PROXY,
    RMI_EVENT_SERVER,
    RMI_EVENT_UNKNOWN
} rmi_event_type_t;

typedef enum {
    RMI_ENCODING_NONE = 0,
    RMI_ENCODING_BASE64
} rmi_encoding_t;

typedef struct {
    int placeholder;
} rmi_protocol_ctx_t;

typedef struct {
    rmi_event_type_t type;
    int argc;
    char cmd[RMI_BUF_SZ + 1];
    char arg1[RMI_BUF_SZ + 1];
    char arg2[RMI_BUF_SZ + 1];
    char arg3[RMI_BUF_SZ + 1];
} rmi_event_t;

#endif /* CLOUDRMI_TYPES_H */
