#ifndef CLOUDRMI_RMI_PROTOCOL_H
#define CLOUDRMI_RMI_PROTOCOL_H

#include "rmi_server.h"
#include "rmi_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

int rmi_handle_command(rmi_server_t *server, char *line, rmi_connection_t *conn);
void rmi_handle_connection(rmi_server_t *server, int sock);
int rmi_handle_call_cmd(rmi_server_t *server, char *iface, char *method, char *arg, rmi_connection_t *state);
int rmi_shell_call_handler_register(rmi_server_t *server);

#ifdef __cplusplus
}
#endif

#endif
