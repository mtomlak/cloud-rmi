#ifndef CLOUDRMI_RMI_SERVER_H
#define CLOUDRMI_RMI_SERVER_H

#include "rmi_payload.h"
#include "rmi_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rmi_server rmi_server_t;

typedef int (*rmi_call_handler)(
    const char *iface,
    const char *method,
    const rmi_payload_t *args,
    rmi_payload_t *result,
    rmi_error_t *error,
    void *userdata
);

struct rmi_server {
    rmi_call_handler call_handler;
    void *call_userdata;
    const char *context_name;
    int protocol_extensions;
};

void rmi_server_init(rmi_server_t *server, const char *context_name);
void rmi_server_set_call_handler(rmi_server_t *server, rmi_call_handler handler, void *userdata);

#ifdef __cplusplus
}
#endif

#endif
