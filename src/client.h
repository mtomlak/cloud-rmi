#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int client_connect(char *ip_addr, int port_num);
void client_disconnect(int sockfd);
int client_parse_status_code(char *response, int *status_code, char *data);
int client_auth_by_token(int sock, char *token);
int client_call_remote_method_raw(int sock, char *iface, char *method, char *enc_args, char *response_payload);
int client_call_remote_method(int sock, char *iface, char *method, char *args, char *result);
int client_call_remote_method_payload(
   int sock,
   char *iface,
   char *method,
   const char *encoding,
   const uint8_t *payload,
   size_t payload_len,
   char *response_payload);
int decode_payload(char *encoded, char *decoded);
int client_raw_cmd(int sock, char *command, char *response_payload);