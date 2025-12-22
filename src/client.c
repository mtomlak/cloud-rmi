#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include "config.h"
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

static int payload_supported(const struct rmi_client_conn *conn)
{
   if (conn == NULL)
      return 0;

   return conn->protocol_version >= 2;
}

static int base64_encoded_length_no_nul(size_t input_len, size_t *out_len)
{
   size_t rounded = 0;

   if (out_len == NULL)
      return 0;

   if (__builtin_add_overflow(input_len, (size_t)2, &rounded))
      return 0;

   rounded /= 3;

   if (__builtin_mul_overflow(rounded, (size_t)4, &rounded))
      return 0;

   *out_len = rounded;
   return 1;
}

static int compute_inline_call_length(const char *iface, const char *method, size_t encoded_arg_len, size_t *out_len)
{
   size_t total = 5; // "CALL "

   if (iface == NULL || method == NULL || out_len == NULL)
      return 0;

   if (__builtin_add_overflow(total, strlen(iface), &total))
      return 0;

   if (__builtin_add_overflow(total, (size_t)1, &total))
      return 0;

   if (__builtin_add_overflow(total, strlen(method), &total))
      return 0;

   if (__builtin_add_overflow(total, (size_t)1, &total))
      return 0;

   if (__builtin_add_overflow(total, encoded_arg_len, &total))
      return 0;

   *out_len = total;
   return 1;
}

static int read_all(int sock, char *buf, size_t len)
{
   size_t offset = 0;

   if (len == 0)
      return 1;

   while (offset < len) {
      ssize_t n = read(sock, buf + offset, len - offset);

      if (n <= 0)
         return 0;

      offset += (size_t)n;
   }

   return 1;
}

static int write_all(int sock, const uint8_t *buf, size_t len)
{
   size_t offset = 0;

   if (len == 0)
      return 1;

   while (offset < len) {
      ssize_t n = write(sock, buf + offset, len - offset);

      if (n <= 0)
         return 0;

      offset += (size_t)n;
   }

   return 1;
}

static int discard_bytes(int sock, size_t len)
{
   char buf[256];
   size_t remaining = len;

   while (remaining > 0) {
      size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
      ssize_t n = read(sock, buf, chunk);

      if (n <= 0)
         return 0;

      remaining -= (size_t)n;
   }

   return 1;
}

/**
 * Starts client connection
 * 
 * @return int socket handle or -1 on failure
 */
int client_connect(char *hostname, int port_num, const char *unix_socket_path, struct rmi_client_conn *conn)
{
   int sock, status_code;  // newsock
   struct sockaddr_in serv_addr;
   struct sockaddr_un serv_addr_un;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));
   char *message = malloc(sizeof(char) * (BUF_SZ + 1));
   char *ip_addr = NULL;
   const int use_unix_socket = unix_socket_path && strlen(unix_socket_path);

   if (conn == NULL)
      return -1;

   conn->sock = -1;
   conn->capabilities = 0;
   conn->capabilities_known = 0;
   conn->protocol_version = 1;

   bzero(response, BUF_SZ);
   bzero(message, BUF_SZ);

   /* First call to socket() function */
   sock = socket(use_unix_socket ? AF_UNIX : AF_INET, SOCK_STREAM, 0);

   if (sock < 0) {
      printf("Error: Failed to open socket");
      free(response);
      free(message);

      return -1;
   }

   if (use_unix_socket && strlen(unix_socket_path) >= sizeof(serv_addr_un.sun_path)) {
      printf("Error: Unix socket path too long\n");
      free(response);
      free(message);
      close(sock);

      return -1;
   }

   if (use_unix_socket) {
      bzero((char *) &serv_addr_un, sizeof(serv_addr_un));
      serv_addr_un.sun_family = AF_UNIX;
      strncpy(serv_addr_un.sun_path, unix_socket_path, sizeof(serv_addr_un.sun_path) - 1);
   } else {
      bzero((char *) &serv_addr, sizeof(serv_addr));

      ip_addr = get_ip_addr(hostname);

      if (ip_addr == NULL) {
         printf("Failed to resolve server hostname\n");
         free(response);
         free(message);
         close(sock);

         return -1;
      }

      serv_addr.sin_family = AF_INET;
      serv_addr.sin_port = htons(port_num);

      // process the IP address
      if(inet_pton(AF_INET, ip_addr, &serv_addr.sin_addr)<=0) {
         printf("Error: inet_pton() failed\n");
         free(response);
         free(message);
         free(ip_addr);
         close(sock);

         return -1;
      }
   }

   /* Now connect to the remote host */
   if (connect(
         sock,
         use_unix_socket ? (struct sockaddr *)&serv_addr_un : (struct sockaddr *) &serv_addr,
         use_unix_socket ? sizeof(serv_addr_un) : sizeof(serv_addr)) < 0) {
      log_unexpected_event(
         LOG_ERR,
         "client-connect-failed unix=%i errno=%i",
         use_unix_socket,
         errno);
      printf("Error: connect() failed\n");
      free(response);
      free(message);
      free(ip_addr);
      close(sock);

      return -1;
   }

   free(ip_addr);

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
   size_t response_len = 0;
   int has_payload_response = 0;
   char response_encoding[BUF_SZ + 1];
   char response_keyword[BUF_SZ + 1];

   bzero(response, BUF_SZ);
   bzero(response_encoding, BUF_SZ);
   bzero(response_keyword, BUF_SZ);

   if (!payload_supported(conn)) {
      snprintf(response_payload, BUF_SZ, "PAYLOAD requires PROTO 2 negotiation");
      status_code = 505;
      goto done;
   }

    if (payload_len > var_max_payload) {
      snprintf(response_payload, BUF_SZ, "Payload exceeds client limit");
      status_code = 505;
      goto done;
    }

   if (stream_writeln(conn->sock, "CALL %s %s PAYLOAD %s %zu", iface, method, encoding_value, payload_len) < 0)
      goto done;

   if (!write_all(conn->sock, payload, payload_len))
      goto done;

   if (stream_readln(conn->sock, response, BUF_SZ) < 0)
      goto done;

   if (!client_parse_status_code(response, &status_code, response_payload)) {
      printf("Error: Cannot parse command response: %s\n", response);
      status_code = 505;
      goto done;
   }

   if ((status_code == 200 || status_code == 500) &&
       sscanf(response_payload, "%s %s %zu", response_keyword, response_encoding, &response_len) == 3 &&
       strcmp(response_keyword, "PAYLOAD") == 0) {
      has_payload_response = 1;
   }

   if (has_payload_response) {
      if (response_len > var_max_payload) {
         snprintf(response_payload, BUF_SZ, "Payload response exceeds client limit");
         status_code = 505;
         discard_bytes(conn->sock, response_len);
         goto done;
      }

      char *payload_buf = malloc(response_len > 0 ? response_len : 1);

      if (payload_buf == NULL) {
         snprintf(response_payload, BUF_SZ, "Error allocating payload buffer");
         status_code = 505;
         goto done;
      }

      if (!read_all(conn->sock, payload_buf, response_len)) {
         snprintf(response_payload, BUF_SZ, "Error reading payload response");
         free(payload_buf);
         status_code = 505;
         goto done;
      }

      size_t encoded_len;
      if (!base64_encoded_length_no_nul(response_len, &encoded_len)) {
         snprintf(response_payload, BUF_SZ, "Error preparing payload response");
         free(payload_buf);
         status_code = 505;
         goto done;
      }

      size_t required_len = strlen(response_encoding) + 1 + encoded_len;

      if (required_len + 1 > (size_t)BUF_SZ) {
         snprintf(response_payload, BUF_SZ, "Payload response too large for buffer");
         free(payload_buf);
         status_code = 505;
         goto done;
      }

      char *encoded_payload = malloc(encoded_len + 1);

      if (encoded_payload == NULL) {
         snprintf(response_payload, BUF_SZ, "Error allocating encoded payload buffer");
         free(payload_buf);
         status_code = 505;
         goto done;
      }

      if (response_len > 0)
         to64frombits((unsigned char *)encoded_payload, (const unsigned char *)payload_buf, (int)response_len);
      else
         encoded_payload[0] = '\0';

      size_t prefix_len = strlen(response_encoding);

      memcpy(response_payload, response_encoding, prefix_len);
      response_payload[prefix_len] = ':';
      memcpy(response_payload + prefix_len + 1, encoded_payload, encoded_len + 1);

      free(encoded_payload);
      free(payload_buf);
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
   char *enc_args = NULL;
   size_t args_len = strlen(args);
   size_t encoded_len = 0;
   size_t inline_line_len = 0;
   int use_payload = 0;
   int payload_available = payload_supported(conn);

   if (payload == NULL) {
      return 0;
   }

   bzero(payload, BUF_SZ);

   if (!base64_encoded_length_no_nul(args_len, &encoded_len)) {
      snprintf(result, BUF_SZ, "Error preparing encoded arguments");
      free(payload);
      return 0;
   }

   if (!compute_inline_call_length(iface, method, encoded_len, &inline_line_len)) {
      snprintf(result, BUF_SZ, "Error preparing CALL line");
      free(payload);
      return 0;
   }

   if (args_len > RMI_INLINE_PAYLOAD_THRESHOLD && payload_available)
      use_payload = 1;

   if (!use_payload && inline_line_len >= RMI_MAX_LINE) {
      if (payload_available) {
         use_payload = 1;
      } else {
         snprintf(result, BUF_SZ, "Payload required for %zu byte CALL", args_len);
         free(payload);
         return 0;
      }
   }

   if (!use_payload) {
      enc_args = malloc(encoded_len + 1);

      if (enc_args == NULL) {
         free(payload);
         return 0;
      }

      bzero(enc_args, encoded_len + 1);
      to64frombits((unsigned char *)enc_args, (const unsigned char *)args, args_len);

      if (client_call_remote_method_raw(conn, iface, method, enc_args, payload) == 200) {
         if (strlen(payload))
            ret = decode_payload(payload, result);

      } else {
         sprintf(result, "%s", payload);
         ret = 0;
      }
   } else {
      if (client_call_remote_method_payload(conn, iface, method, "PLAIN", (const uint8_t *)args, args_len, payload) == 200) {
         if (strlen(payload))
            ret = decode_payload(payload, result);
      } else {
         sprintf(result, "%s", payload);
         ret = 0;
      }
   }

   free(payload);
   free(enc_args);
   return ret;
}
