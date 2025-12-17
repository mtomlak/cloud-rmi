#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <ctype.h>
#include "cloud-rmi.h"
#include "capabilities.h"
#include "client.h"

/**
 * Simple RMI client in C
 *
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license BSD
 */

/**
 * Parses status code from the response
 * 
 * @param  result Server response
 * @return        Error code
 */
int client_parse_status_code(char *response, int *status_code, char *data)
{
   if (sscanf(response, "%i ", status_code)) {
      substring(response, data, 4, strlen(response) - 4);
      return 1;
   }

   printf("Error: could not parse authentication response");
   return 0;
}

static int parse_max_proto(const char *ext)
{
   const char *proto = strstr(ext, "PROTO=");
   int version = 0;

   if (proto != NULL) {
      proto += 6;

      while (isdigit((unsigned char)*proto)) {
         version = version * 10 + (*proto - '0');
         proto++;
      }
   }

   return version > 0 ? version : 1;
}

static void normalize_capability_tokens(const char *ext, char *buffer, size_t buffer_len)
{
   size_t len = strlen(ext);

   if (len >= buffer_len)
      len = buffer_len - 1;

   for (size_t i = 0; i < len; i++) {
      buffer[i] = (ext[i] == ',') ? ' ' : ext[i];
   }

   buffer[len] = '\0';
}

/**
 * Starts client connection
 * 
 * @return int socket handle or -1 on failure
 */
int client_connect(char *hostname, int port_num, struct rmi_client_conn *conn)
{
   int sock, status_code;  // newsock
   struct sockaddr_in serv_addr;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));
   char *message = malloc(sizeof(char) * (BUF_SZ + 1));
   char *ip_addr;

   if (conn == NULL)
      return -1;

   conn->sock = -1;
   conn->capabilities = 0;
   conn->capabilities_known = 0;
   conn->protocol_version = 1;

   bzero(response, BUF_SZ);
   bzero(message, BUF_SZ);

   ip_addr = get_ip_addr(hostname);

   if (ip_addr == NULL)
      die_err("Failed to resolve server hostname");

   /* First call to socket() function */
   sock = socket(AF_INET, SOCK_STREAM, 0);

   if (sock < 0) {
      printf("Error: Failed to open socket");
      free(response);
      free(message);

      return -1;
   }

   bzero((char *) &serv_addr, sizeof(serv_addr));

   serv_addr.sin_family = AF_INET;
   serv_addr.sin_port = htons(port_num);

   // process the IP address
   if(inet_pton(AF_INET, ip_addr, &serv_addr.sin_addr)<=0) {
      printf("Error: inet_pton() failed\n");
      free(response);
      free(message);
      close(sock);

      return -1;
    }

   /* Now connect to the remote host */
   if (connect(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
      printf("Error: connect() failed\n");
      free(response);
      free(message);
      close(sock);

      return -1;
   }

   /* And read the banner */
   if (stream_readln(sock, response, BUF_SZ) < 0) {
      free(response);
      free(message);
      close(sock);
      return -1;
   }

   if (!client_parse_status_code(response, &status_code, message)) {
      printf("Error: Cannot parse authentication response: %s\n", response);
      free(response);
      free(message);
      close(sock);

      return -1;
   }

   if (status_code != 220) {
      printf("Error: Failed to connect - invalid banner received: %s\n", response);
      free(response);
      free(message);
      close(sock);

      return -1;
   }

   conn->sock = sock;

   int server_max_proto = 1;
   char *ext = strstr(response, " EXT ");
   if (ext != NULL) {
      char *caps_tokens = malloc(sizeof(char) * (BUF_SZ + 1));

      ext += 5;
      server_max_proto = parse_max_proto(ext);

      if (caps_tokens != NULL) {
         normalize_capability_tokens(ext, caps_tokens, BUF_SZ + 1);
         conn->capabilities = rmi_parse_capabilities(caps_tokens);
         conn->capabilities_known = 1;
      }

      free(caps_tokens);
   }

   if (!conn->capabilities_known) {
      char *caps_response = malloc(sizeof(char) * (BUF_SZ + 1));
      char *caps_message = malloc(sizeof(char) * (BUF_SZ + 1));

      if (caps_response != NULL && caps_message != NULL) {
         bzero(caps_response, BUF_SZ);
         bzero(caps_message, BUF_SZ);

         if (stream_writeln(sock, "CAPS") >= 0 && stream_readln(sock, caps_response, BUF_SZ) >= 0) {
            if (client_parse_status_code(caps_response, &status_code, caps_message) && status_code == 200) {
               conn->capabilities = rmi_parse_capabilities(caps_message);
               conn->capabilities_known = 1;
            }
         }
      }

      free(caps_response);
      free(caps_message);
   }

   if (server_max_proto >= 2) {
      bzero(response, BUF_SZ);
      bzero(message, BUF_SZ);

      if (stream_writeln(sock, "PROTO 2") >= 0 && stream_readln(sock, response, BUF_SZ) >= 0) {
         if (client_parse_status_code(response, &status_code, message) && status_code == 200)
            conn->protocol_version = 2;
      }
   }

   free(response);
   free(message);

   return sock;
}

/**
 * Disconnects client
 * 
 * @return void
 */
void client_disconnect(struct rmi_client_conn *conn)
{
   if (conn == NULL || conn->sock < 0)
      return;

   close(conn->sock);
   conn->sock = -1;
}


/**
 * Authenticates client connection with token
 * 
 * @return  1 on success, 0 on failure
 */
int client_auth_by_token(struct rmi_client_conn *conn, char *token)
{
   int status_code, ret;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));
   char *message = malloc(sizeof(char) * (BUF_SZ + 1));

   bzero(response, BUF_SZ);
   bzero(message, BUF_SZ);

   stream_writeln(conn->sock, "AUTH TOKEN %s", token);
   stream_readln(conn->sock, response, BUF_SZ);

   if (!client_parse_status_code(response, &status_code, message)) {
      printf("Error: Cannot parse authentication response: %s\n", response);
      ret = 0;

   } else if (status_code != 200) {
      printf("Error: Authentication failed (client mode): %s\n", message);
      ret = 0;

   } else
      ret = 1;

   free(response);
   free(message);
   return ret;
}

/**
 * Calls raw RMI protocol command
 * 
 * @return  Server status code or 0
 */
int client_raw_cmd(struct rmi_client_conn *conn, char *command, char *response_payload)
{
   int status_code = 0;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));

   bzero(response, BUF_SZ);

   if (stream_writeln(conn->sock, "%s", command) < 0) {
      free(response);
      return 0;
   }

   if (stream_readln(conn->sock, response, BUF_SZ) < 0) {
      return 0;
   }

   if (!client_parse_status_code(response, &status_code, response_payload)) {
      printf("Error: Cannot parse command response: %s\n", response);
      status_code = 0;
   }

   // Read the rest if command has multiline output
   if (response[strlen(response) - 1] == ':') {
      stream_readlns(conn->sock, response, BUF_SZ);
      sprintf(response_payload, "%s\n%s", response_payload, response);
   }

   free(response);
   return status_code;
}

/**
 * Calls remote method, with raw encoded parameters and returns raw encoded output.
 * 
 * @return  Server status code
 */
int client_call_remote_method_raw(struct rmi_client_conn *conn, char *iface, char *method, char *enc_args, char *response_payload)
{
   int status_code;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));

   bzero(response, BUF_SZ);
   stream_writeln(conn->sock, "CALL %s %s %s", iface, method, enc_args);
   stream_readln(conn->sock, response, BUF_SZ);

   if (!client_parse_status_code(response, &status_code, response_payload)) {
      printf("Error: Cannot parse command response: %s\n", response);
      status_code = 505;
   }

   free(response);
   return status_code;
}

int client_call_remote_method_payload(
   struct rmi_client_conn *conn,
   char *iface,
   char *method,
   const char *encoding,
   const uint8_t *payload,
   size_t payload_len,
   char *response_payload)
{
   int status_code = 0;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));
   const char *encoding_value = encoding ? encoding : "BLOB";

   bzero(response, BUF_SZ);

   if (conn->protocol_version < 2) {
      snprintf(response_payload, BUF_SZ, "PAYLOAD requires PROTO 2 negotiation");
      status_code = 505;
      goto done;
   }

   if (!conn->capabilities_known || !(conn->capabilities & RMI_CAP_PAYLOAD)) {
      snprintf(response_payload, BUF_SZ, "PAYLOAD not supported by server");
      status_code = 505;
      goto done;
   }

   if (stream_writeln(conn->sock, "CALL %s %s", iface, method) < 0)
      goto done;

   if (stream_writeln(conn->sock, "PAYLOAD %s %zu", encoding_value, payload_len) < 0)
      goto done;

   if (payload_len > 0 && stream_write(conn->sock, (char *)payload, (int)payload_len) < 0)
      goto done;

   if (stream_readln(conn->sock, response, BUF_SZ) < 0)
      goto done;

   if (!client_parse_status_code(response, &status_code, response_payload)) {
      printf("Error: Cannot parse command response: %s\n", response);
      status_code = 505;
   }

done:
   free(response);
   return status_code;
}

int decode_payload(char *encoded, char *decoded)
{
   char *start;

   if (start = strchr(encoded, ':')) {
      start++;
      from64tobits(decoded, start);
      return 1;

   } else {
      from64tobits(decoded, encoded);
      return 1;
   }
}

/**
 * Calls remote method with simple parameter string and returns plaintext response
 * 
 * @return 1 on success 0 on failure
 */
int client_call_remote_method(struct rmi_client_conn *conn, char *iface, char *method, char *args, char *result)
{
   int ret = 1;
   char *payload = malloc(sizeof(char) * (BUF_SZ + 1));
   char *enc_args = malloc(sizeof(char) * (BUF_SZ + 1));

   bzero(payload, BUF_SZ);
   bzero(enc_args, BUF_SZ);

   // Encode arguments
   to64frombits(enc_args, args, strlen(args));

   if (client_call_remote_method_raw(conn, iface, method, enc_args, payload) == 200) {
      if (strlen(payload))
         ret = decode_payload(payload, result);

   } else {
      sprintf(result, "%s", payload);
      ret = 0;
   }

   free(payload);
   free(enc_args);
   return ret;
}