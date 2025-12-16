#ifndef CLOUD_RMI_CLIENT_H
#define CLOUD_RMI_CLIENT_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

struct rmi_client_conn {
   int sock;
   uint32_t capabilities;
   int capabilities_known;
   int protocol_version;
};

int client_connect(char *ip_addr, int port_num, struct rmi_client_conn *conn);
void client_disconnect(struct rmi_client_conn *conn);
int client_parse_status_code(char *response, int *status_code, char *data);
int client_auth_by_token(struct rmi_client_conn *conn, char *token);
int client_call_remote_method_raw(struct rmi_client_conn *conn, char *iface, char *method, char *enc_args, char *response_payload);
int client_call_remote_method(struct rmi_client_conn *conn, char *iface, char *method, char *args, char *result);
int client_call_remote_method_payload(
   struct rmi_client_conn *conn,
   char *iface,
   char *method,
   const char *encoding,
   const uint8_t *payload,
   size_t payload_len,
   char *response_payload);
int decode_payload(char *encoded, char *decoded);
int client_raw_cmd(struct rmi_client_conn *conn, char *command, char *response_payload);

#endif /* CLOUD_RMI_CLIENT_H */