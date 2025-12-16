#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "protocol_line.h"

static void rmi_reset_event(rmi_event_t *event)
{
    if (event == NULL)
        return;

    memset(event->cmd, 0, sizeof(event->cmd));
    memset(event->arg1, 0, sizeof(event->arg1));
    memset(event->arg2, 0, sizeof(event->arg2));
    memset(event->arg3, 0, sizeof(event->arg3));
    memset(event->arg4, 0, sizeof(event->arg4));
    memset(event->encoding, 0, sizeof(event->encoding));
    event->argc = 0;
    event->type = RMI_EVENT_NONE;
    event->payload = NULL;
    event->payload_len = 0;
}

static void rmi_lowercase(char *value)
{
    if (value == NULL)
        return;

    for (; *value; ++value)
        *value = (char)tolower((unsigned char)*value);
}

int rmi_protocol_handle_line(
    rmi_protocol_ctx_t *ctx,
    const char *line,
    rmi_event_t *out_event,
    rmi_error_t *err)
{
    (void)ctx;

    if (err != NULL) {
        err->code = RMI_ERROR_NONE;
        err->message = NULL;
    }

    if (line == NULL || out_event == NULL) {
        if (err != NULL) {
            err->code = RMI_ERROR_BAD_REQUEST;
            err->message = "Invalid arguments";
        }
        return 0;
    }

    rmi_reset_event(out_event);

    char cmd[(RMI_BUF_SZ + 1)];
    char arg1[(RMI_BUF_SZ + 1)];
    char arg2[(RMI_BUF_SZ + 1)];
    char arg3[(RMI_BUF_SZ + 1)];
    char arg4[(RMI_BUF_SZ + 1)];

    memset(cmd, 0, sizeof(cmd));
    memset(arg1, 0, sizeof(arg1));
    memset(arg2, 0, sizeof(arg2));
    memset(arg3, 0, sizeof(arg3));
    memset(arg4, 0, sizeof(arg4));

    out_event->argc = sscanf(line, "%s %s %s %s %s", cmd, arg1, arg2, arg3, arg4);
    strncpy(out_event->cmd, cmd, RMI_BUF_SZ);
    strncpy(out_event->arg1, arg1, RMI_BUF_SZ);
    strncpy(out_event->arg2, arg2, RMI_BUF_SZ);
    strncpy(out_event->arg3, arg3, RMI_BUF_SZ);
    strncpy(out_event->arg4, arg4, RMI_BUF_SZ);

    rmi_lowercase(out_event->cmd);

    if (out_event->argc <= 0) {
        if (err != NULL) {
            err->code = RMI_ERROR_BAD_REQUEST;
            err->message = "Empty command";
        }
        return 0;
    }

    if (strcmp(out_event->cmd, "auth") == 0) {
        out_event->type = RMI_EVENT_AUTH;
    } else if (strcmp(out_event->cmd, "call") == 0) {
        out_event->type = RMI_EVENT_CALL;
        if (out_event->argc < 3 && err != NULL) {
            err->code = RMI_ERROR_BAD_ARGS;
            err->message = "CALL requires iface and method";
        }
    } else if (strcmp(out_event->cmd, "payload") == 0) {
        out_event->type = RMI_EVENT_PAYLOAD;
        if (out_event->argc < 3 && err != NULL) {
            err->code = RMI_ERROR_BAD_ARGS;
            err->message = "PAYLOAD requires encoding and length";
        }
    } else if (strcmp(out_event->cmd, "help") == 0) {
        out_event->type = RMI_EVENT_HELP;
    } else if (strcmp(out_event->cmd, "ifaces") == 0) {
        out_event->type = RMI_EVENT_IFACES;
    } else if (strcmp(out_event->cmd, "reload") == 0) {
        out_event->type = RMI_EVENT_RELOAD;
    } else if (strcmp(out_event->cmd, "shutdown") == 0) {
        out_event->type = RMI_EVENT_SHUTDOWN;
    } else if (strcmp(out_event->cmd, "time") == 0) {
        out_event->type = RMI_EVENT_TIME;
    } else if (strcmp(out_event->cmd, "uptime") == 0) {
        out_event->type = RMI_EVENT_UPTIME;
    } else if (strcmp(out_event->cmd, "noop") == 0) {
        out_event->type = RMI_EVENT_NOOP;
    } else if (strcmp(out_event->cmd, "debug") == 0) {
        out_event->type = RMI_EVENT_DEBUG;
    } else if ((strcmp(out_event->cmd, "quit") == 0) || (strcmp(out_event->cmd, "exit") == 0)) {
        out_event->type = RMI_EVENT_QUIT;
    } else if (strcmp(out_event->cmd, "proxy") == 0) {
        out_event->type = RMI_EVENT_PROXY;
    } else if (strcmp(out_event->cmd, "caps") == 0) {
        out_event->type = RMI_EVENT_CAPS;
    } else if (strcmp(out_event->cmd, "server") == 0) {
        out_event->type = RMI_EVENT_SERVER;
    } else if (strcmp(out_event->cmd, "proto") == 0) {
        out_event->type = RMI_EVENT_PROTO;
    } else {
        out_event->type = RMI_EVENT_UNKNOWN;
        if (err != NULL) {
            err->code = RMI_ERROR_BAD_REQUEST;
            err->message = "Unknown command";
        }
    }

    return 1;
}
