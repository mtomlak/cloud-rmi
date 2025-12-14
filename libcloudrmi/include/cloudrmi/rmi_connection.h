#ifndef CLOUDRMI_RMI_CONNECTION_H
#define CLOUDRMI_RMI_CONNECTION_H

#include "rmi_payload.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct conn_state {
    int sock;
    int is_authenticated;
    char username[256];
} rmi_connection_t;

typedef struct conn_state conn_state;

void rmi_connection_init(rmi_connection_t *conn, int sock);

#ifdef __cplusplus
}
#endif

#endif
