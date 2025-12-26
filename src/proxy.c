#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include "cloud-rmi.h"
#include "config.h"
#include "proxy.h"
#include "client.h"
#include <cloudrmi/semantics.h>

/**
 * Provides proxy server functionality
 *
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license BSD
 */

// Proxy reladed variables
//struct proxy_server proxy_list[PROXY_LIST_SZ];
//int proxy_list_len = 0;
//
struct proxy_server proxy_list[PROXY_LIST_SZ];
struct proxy_child_entry *proxy_child_list = NULL;
int proxy_count;
int proxy_manager_pipe[2];
int var_proxy_min_spare_conns = 3;
struct proxy_iface_entry *proxy_iface_map;
#define PROXY_STATS_CLIENT_LIMIT 50
#define PROXY_CONNECT_MAX_ATTEMPTS 3
#define PROXY_CONNECT_RETRY_DELAY_US 100000
#define PROXY_STATS_CLIENT_LIMIT 50

static const char *normalize_iface_name(const char *iface)
{
   if (iface != NULL && iface[0] == '\\' && iface[1] != '\0')
      return iface + 1;

   return iface;
}

// refered from inside
int proxy_get_spare_conn_count(int proxy_id);

static int proxy_write_all(int fd, const uint8_t *buf, size_t len)
{
   size_t total = 0;

   while (total < len) {
      ssize_t written;

      do {
         written = write(fd, buf + total, len - total);
      } while (written < 0 && errno == EINTR);

      if (written < 0) {
         // Proxy sockets are expected to be blocking; treat EAGAIN/EWOULDBLOCK as fatal to avoid spin
         if (errno == EAGAIN || errno == EWOULDBLOCK)
            errno = EIO;
         return 0;
      }

      if (written == 0)
         return 0;

      total += (size_t)written;
   }

   return 1;
}

static int proxy_discard_bytes(int fd, size_t len)
{
   size_t consumed = 0;
   uint8_t buf[BUF_SZ];

   while (consumed < len) {
      size_t chunk = len - consumed;
      if (chunk > sizeof(buf))
         chunk = sizeof(buf);

      ssize_t n = read(fd, buf, chunk);
      if (n < 0) {
         if (errno == EINTR)
            continue;
         return 0;
      }
      if (n == 0)
         break;
      consumed += (size_t)n;
   }

   return 1;
}

static int parse_proxy_address(
   const char *address,
   char *hostname,
   size_t hostname_len,
   int *port,
   char *unix_socket_path,
   size_t unix_socket_len)
{
   if (address == NULL || hostname == NULL || port == NULL || unix_socket_path == NULL)
      return 0;

   if (hostname_len > 0)
      bzero(hostname, hostname_len);

   if (unix_socket_len > 0)
      bzero(unix_socket_path, unix_socket_len);

   if (strncmp(address, "unix:", 5) == 0) {
      const char *path = address + 5;

      if (strlen(path) >= unix_socket_len)
         return 0;

      strncpy(unix_socket_path, path, unix_socket_len - 1);
      *port = 0;
      return 1;
   }

   if (address[0] == '/') {
      if (strlen(address) >= unix_socket_len)
         return 0;

      strncpy(unix_socket_path, address, unix_socket_len - 1);
      *port = 0;
      return 1;
   }

   split_host_addr((char *)address, hostname, port);
   unix_socket_path[0] = '\0';
   return 1;
}

static int proxy_in_cooldown(const struct proxy_server *proxy)
{
   if (proxy == NULL)
      return 1;

   if (proxy->next_retry_at == 0)
      return 0;

   return time(NULL) < proxy->next_retry_at;
}

static void proxy_mark_failure(struct proxy_server *proxy, const char *reason)
{
   time_t now = time(NULL);

   if (proxy == NULL)
      return;

   proxy->is_available = 0;
   proxy->last_failure_at = now;
   proxy->next_retry_at = now + 60;

   log_unexpected_event(
      LOG_WARNING,
      "proxy-backend-unavailable proxy_id=%i reason=%s retry_at=%ld",
      proxy->proxy_id,
      reason != NULL ? reason : "unknown",
      (long)proxy->next_retry_at);
}

static void proxy_mark_success(struct proxy_server *proxy)
{
   if (proxy == NULL)
      return;

   proxy->is_available = 1;
   proxy->last_failure_at = 0;
   proxy->next_retry_at = 0;
}

static int proxy_append_format(char *buf, size_t buf_sz, size_t *offset, const char *fmt, ...)
{
   va_list args;
   int written;

   if (buf == NULL || offset == NULL || *offset >= buf_sz)
      return 0;

   va_start(args, fmt);
   written = vsnprintf(buf + *offset, buf_sz - *offset, fmt, args);
   va_end(args);

   if (written < 0 || (size_t)written >= buf_sz - *offset) {
      *offset = buf_sz;
      return 0;
   }

   *offset += (size_t)written;
   return 1;
}

static size_t proxy_json_escape_into(char *dst, size_t dst_size, const char *src)
{
   size_t written = 0;

   if (dst_size == 0)
      return 0;

   if (src == NULL)
      src = "";

   while (src[0] != '\0' && written + 1 < dst_size) {
      char c = *src;
      const char *escape = NULL;

      switch (c) {
      case '\"':
         escape = "\\\"";
         break;
      case '\\':
         escape = "\\\\";
         break;
      case '\n':
         escape = "\\n";
         break;
      case '\r':
         escape = "\\r";
         break;
      case '\t':
         escape = "\\t";
         break;
      default:
         break;
      }

      if (escape != NULL) {
         size_t elen = strlen(escape);

         if (written + elen >= dst_size)
            break;

         memcpy(dst + written, escape, elen);
         written += elen;
      } else {
         dst[written++] = c;
      }

      src++;
   }

   dst[written] = '\0';
   return written;
}

static void proxy_json_array_append(char *buf, size_t buf_sz, size_t *offset, const char *value, int is_first)
{
   char escaped[BUF_SZ + 1];

   proxy_json_escape_into(escaped, sizeof(escaped), value);
   proxy_append_format(buf, buf_sz, offset, "%s\"%s\"", is_first ? "" : ",", escaped);
}


//
// Proxy list management
//

int proxy_list_count()
{
   return proxy_count;
}

struct proxy_server *proxy_list_get(int proxy_id)
{
     if (proxy_id > proxy_count)
      die_err("Error: proxy_list_get(): Cannot load proxy from memory");

   return &proxy_list[proxy_id];
}

int proxy_list_add(struct proxy_server proxy)
{
   int id = proxy_count;

   proxy.proxy_id = id;
   proxy_list[id] = proxy;
   proxy_count++;

   return id;
}

struct proxy_server new_proxy_struct(const char *hostname, int port, const char *unix_socket_path)
{
   struct proxy_server proxy;

   bzero(&proxy, sizeof(proxy));
   proxy.proxy_id = 0;      // will be set when it gets ID assigned
   proxy.conn_count = 0;
   proxy.port = port;
   proxy.last_failure_at = 0;
   proxy.next_retry_at = 0;
   proxy.is_available = 1;
   if (hostname != NULL) {
      strncpy(proxy.hostname, hostname, sizeof(proxy.hostname) - 1);
   }
   if (unix_socket_path != NULL && strlen(unix_socket_path)) {
      proxy.use_unix_socket = 1;
      strncpy(proxy.unix_socket_path, unix_socket_path, sizeof(proxy.unix_socket_path) - 1);
   }

   return proxy;
}

int proxy_list_add_new(const char *hostname, int port, const char *unix_socket_path)
{
   return proxy_list_add(new_proxy_struct(hostname, port, unix_socket_path));
}

//
// Proxy connection list
//

int proxy_add_conn(int proxy_id, struct rmi_client_conn client)
{
   if (proxy_id > proxy_count)
      die_err("Error: proxy_add_conn(): Cannot load proxy from memory");

   int ci = proxy_list[proxy_id].conn_count;

   proxy_list[proxy_id].conns[ci].client = client;
   proxy_list[proxy_id].conns[ci].is_busy = 0;
   proxy_list[proxy_id].conns[ci].proxy_id = proxy_id;
   proxy_list[proxy_id].conn_count++;

   return ci;
}

struct proxy_conn *proxy_get_conn(int proxy_id, int conn_id)
{
   if (proxy_id > proxy_count || conn_id > proxy_list[proxy_id].conn_count)
      die_err("Error: Cannot load proxy connection from memory");

   return &proxy_list[proxy_id].conns[conn_id];
}

/**
 * Starts new proxy connection
 * 
 */
int proxy_start_new_conn(int proxy_id)
{
   struct rmi_client_conn client;
   struct proxy_server *proxy = proxy_list_get(proxy_id);
   int attempt = 0;

   if (proxy_in_cooldown(proxy))
      return -1;

   // Connect
   while (attempt < PROXY_CONNECT_MAX_ATTEMPTS) {
      if (client_connect(
         proxy->hostname,
         proxy->port,
         proxy->use_unix_socket ? proxy->unix_socket_path : NULL,
         &client
         ) >= 0)
         break;

      attempt++;
      usleep(PROXY_CONNECT_RETRY_DELAY_US);
   }

   if (attempt >= PROXY_CONNECT_MAX_ATTEMPTS) {
      proxy_mark_failure(proxy, "connect-failed");
      return -1;
   }

   // Authenticate
   if (var_recovery_token != NULL) {
      if (!client_auth_by_token(&client, var_recovery_token)) {
         client_disconnect(&client);
         proxy_mark_failure(proxy, "auth-failed");
         return -1;
      }
   }

   proxy_mark_success(proxy);

   // Add socket to the list
   return proxy_add_conn(proxy_id, client);
}

int proxy_restart_conn(struct proxy_conn *conn)
{
   int proxy_id;
   struct proxy_server *proxy;
   int attempt = 0;

   DEBUG_LOG("Proxy: Restarting connection to proxy #%i", conn->proxy_id);

   proxy_id = conn->proxy_id;
   proxy = proxy_list_get(proxy_id);
   client_disconnect(&conn->client);

   if (proxy_in_cooldown(proxy))
      return 0;

   while (attempt < PROXY_CONNECT_MAX_ATTEMPTS) {
      if (client_connect(
         proxy->hostname,
         proxy->port,
         proxy->use_unix_socket ? proxy->unix_socket_path : NULL,
         &conn->client
         ) >= 0)
         break;

      attempt++;
      usleep(PROXY_CONNECT_RETRY_DELAY_US);
   }

   if (attempt >= PROXY_CONNECT_MAX_ATTEMPTS) {
      proxy_mark_failure(proxy, "reconnect-failed");
      return 0;
   }

   if (var_recovery_token != NULL)
      if (!client_auth_by_token(&conn->client, var_recovery_token)) {
         client_disconnect(&conn->client);
         proxy_mark_failure(proxy, "auth-failed");
         return 0;
      }

   proxy_mark_success(proxy);

   return 1;
}

/**
 * Gets first unused connection to the given proxy.
 */
struct proxy_conn *proxy_get_free_conn(int proxy_id)
{
   int i;
   struct proxy_server *proxy;

   if (proxy_id > proxy_count)
      die_err("Error: Cannot load proxy connection from memory");

   proxy = proxy_list_get(proxy_id);

   for (i = 0; i < proxy_list[proxy_id].conn_count; i++) {
      if (!proxy_list[proxy_id].conns[i].is_busy)
         return &proxy_list[proxy_id].conns[i];
   }

   // If we got here, all connections are busy, need to create new one
   i = proxy_start_new_conn(proxy_id);

   if (i < 0)
      return NULL;

   return &proxy->conns[i];
}

void proxy_set_conn_busy(struct proxy_conn *conn)
{
   conn->is_busy = 1;
}

void proxy_set_conn_free(struct proxy_conn *conn)
{
   conn->is_busy = 0;
}

//
// Proxy interface list
//


struct proxy_iface_entry *new_proxy_iface_entry(char *iface_name, int proxy_id)
{
   struct proxy_iface_entry *result = malloc(sizeof(struct proxy_iface_entry));

   result->iface = strdup(iface_name);
   result->proxy_id = proxy_id;
   result->next = NULL;

   return result;
}

struct proxy_iface_entry *proxy_get_last_iface_entry()
{
   struct proxy_iface_entry *cur;

   if (proxy_iface_map == NULL)
      return NULL;

   cur = proxy_iface_map;

   while (cur->next != NULL) {
      cur = cur->next;
   }

   return cur;
}

struct proxy_iface_entry *proxy_get_iface_entry(char *iface_name)
{
   struct proxy_iface_entry *cur;

   if (proxy_iface_map == NULL)
      return NULL;

   cur = proxy_iface_map;

   while (cur != NULL) {
      if (strcasecmp(iface_name, cur->iface) == 0)
         return cur;

      cur = cur->next;
   }

   return NULL;
}

static void proxy_clear_iface_entries(int proxy_id)
{
   struct proxy_iface_entry *cur = proxy_iface_map;
   struct proxy_iface_entry *prev = NULL;

   while (cur != NULL) {
      struct proxy_iface_entry *next = cur->next;

      if (cur->proxy_id == proxy_id) {
         if (prev == NULL)
            proxy_iface_map = next;
         else
            prev->next = next;

         free(cur->iface);
         free(cur);
      } else {
         prev = cur;
      }

      cur = next;
   }
}

void proxy_add_iface_entry(int proxy_id, char *iface_name)
{
   DEBUG_LOG("Proxy: Adding interface %s for proxy #%i", iface_name, proxy_id);

   if (proxy_iface_map == NULL) // adding first item
      proxy_iface_map = new_proxy_iface_entry(iface_name, proxy_id);
   else
      proxy_get_last_iface_entry()->next = new_proxy_iface_entry(iface_name, proxy_id);
}

int proxy_fetch_interfaces(int proxy_num)
{
   struct proxy_conn *conn;
   struct proxy_server *proxy;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));
   int ret = 0;

   if (response == NULL)
      return 0;

   bzero(response, BUF_SZ);

   if (proxy_num > proxy_count)
      goto cleanup;

   proxy = proxy_list_get(proxy_num);

   conn = proxy_get_free_conn(proxy_num);
   if (conn == NULL) {
      proxy_mark_failure(proxy, "iface-conn-unavailable");
      goto cleanup;
   }

   proxy_set_conn_busy(conn);
   if (stream_writeln(conn->client.sock, "IFACES") < 0 || stream_readln(conn->client.sock, response, BUF_SZ) < 0) {
      proxy_set_conn_free(conn);
      proxy_mark_failure(proxy, "iface-status");
      log_unexpected_event(LOG_WARNING, "proxy-ifaces-io-failed");
      goto cleanup;
   }
   proxy_set_conn_free(conn);

   cloudrmi_enum_response enum_resp;
   if (!cloudrmi_parse_enum_response(response, &enum_resp)) {
      proxy_mark_failure(proxy, "iface-format");
      log_unexpected_event(LOG_WARNING, "proxy-ifaces-parse-failed response=%s", response);
      goto cleanup;
   }

   proxy_clear_iface_entries(proxy_num);
   for (size_t i = 0; i < enum_resp.count; i++) {
      proxy_add_iface_entry(proxy_num, (char *)normalize_iface_name(enum_resp.values[i]));
   }

   proxy_mark_success(proxy);
   ret = 1;

cleanup:
   free(response);
   return ret;
}


//
// Other proxy methods
//


/**
 * Prints information about proxy usage
 * 
 */
void proxy_send_stats(int sock)
{
   int i, j;
   int trimmed = 0;
   size_t offset = 0;
   char line[BUF_SZ + 1];

   proxy_append_format(line, sizeof(line), &offset, "200 JSON {\"servers\":[");

   for (i = 0; i < proxy_count; i++) {
      struct proxy_server *proxy = proxy_list_get(i);
      char label[PROXY_HOSTNAME_SZ + PROXY_UNIX_PATH_SZ + 4];
      int ok;

      proxy_json_escape_into(label, sizeof(label), proxy->use_unix_socket ? proxy->unix_socket_path : proxy->hostname);

      ok = proxy_append_format(
         line,
         sizeof(line),
         &offset,
         "%s{\"id\":%i,\"label\":\"%s\",\"port\":%i,\"unix\":%s,\"free\":%i,\"total\":%i,\"available\":%s}",
         (i == 0) ? "" : ",",
         i,
         label,
         proxy->use_unix_socket ? 0 : proxy->port,
         proxy->use_unix_socket ? "true" : "false",
         proxy_get_spare_conn_count(i),
         proxy->conn_count,
         proxy->is_available ? "true" : "false");

      if (!ok) {
         trimmed = 1;
         break;
      }
   }

   proxy_append_format(line, sizeof(line), &offset, "],\"clients\":[");

   size_t client_count = 0;
   for (i = 0; i < proxy_count; i++) {
      struct proxy_server *proxy = proxy_list_get(i);

      for (j = 0; j < proxy->conn_count; j++) {
         struct proxy_conn *conn = &proxy->conns[j];
         int ok;

         if (client_count >= PROXY_STATS_CLIENT_LIMIT) {
            trimmed = 1;
            break;
         }

         ok = proxy_append_format(
            line,
            sizeof(line),
            &offset,
            "%s{\"conn_id\":%i,\"proxy_id\":%i,\"socket\":%i,\"busy\":%s}",
            (client_count == 0) ? "" : ",",
            conn->conn_id,
            conn->proxy_id,
            conn->client.sock,
            conn->is_busy ? "true" : "false");

         if (!ok) {
            trimmed = 1;
            break;
         }

         client_count++;
      }

      if (trimmed)
         break;
   }

   proxy_append_format(line, sizeof(line), &offset, "],\"trimmed\":%s}", trimmed ? "true" : "false");
   stream_writeln(sock, "%s", line);
}


int proxy_pass_raw_cmd(int proxy_num, struct conn_state *client_conn, char *cmd)
{
   int status_code, ret = 1; 
   struct proxy_conn *conn;
   struct proxy_server *proxy;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));

   bzero(response, BUF_SZ);

   if (proxy_num > proxy_list_count()) {
      stream_writeln(client_conn->sock, "503 Service Unavailable");
      free(response);
      return 0;
   }

   proxy = proxy_list_get(proxy_num);

   // Get interfaces
   conn = proxy_get_free_conn(proxy_num);
   if (conn == NULL) {
      proxy_mark_failure(proxy, "conn-unavailable");
      stream_writeln(client_conn->sock, "503 Service Unavailable");
      free(response);
      return 0;
   }

   proxy_set_conn_busy(conn);
   status_code = client_raw_cmd(&conn->client, cmd, response);

   if (!status_code || status_code == 408) {  // 408 - request timeout
      log_unexpected_event(
         LOG_WARNING,
         "proxy-reconnect proxy_id=%i status=%i",
         proxy_num,
         status_code);

      if (proxy_restart_conn(conn))
         status_code = client_raw_cmd(&conn->client, cmd, response);
      else
         status_code = 503;
   }

   if (!status_code) {
      status_code = 503;
      strcpy(response, "Service Unavailable");

      log_unexpected_event(
         LOG_ERR,
         "proxy-status-missing proxy_id=%i",
         proxy_num);

      ret = 0;
      proxy_mark_failure(proxy, "status-missing");
   }

   if (status_code == 200)
      proxy_mark_success(proxy);
   else if (status_code == 503 && response[0] == '\0')
      strcpy(response, "Service Unavailable");

   stream_writeln(client_conn->sock, "%i %s", status_code, response);
   proxy_set_conn_free(conn);

   free(response);

   return ret;
}

int proxy_pass_call_cmd_ex(
   const char *iface,
   const char *cmd,
   const char *arg,
   const uint8_t *payload,
   size_t payload_len,
   const char *encoding,
   struct conn_state *client_conn)
{
   int ret = 1;
   struct proxy_iface_entry *iface_entry;
   struct proxy_conn *conn;
   struct proxy_server *proxy = NULL;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));
   char *message = malloc(sizeof(char) * (BUF_SZ + 1));
   char *payload_response = NULL;
   const char *iface_lookup = normalize_iface_name(iface);
   int status_code = 0;
   int responded = 0;

   if (iface_lookup == NULL)
      iface_lookup = "";

   if (response == NULL || message == NULL) {
      free(response);
      free(message);
      return 0;
   }

   if (payload != NULL && payload_len > var_max_payload) {
      stream_writeln(client_conn->sock, "505 Payload exceeds proxy limit");
      free(response);
      free(message);
      return 0;
   }

   iface_entry = proxy_get_iface_entry((char *)iface_lookup);

   if (iface_entry == NULL) {
      stream_writeln(client_conn->sock, "400 Unsupported interface: %s", iface_lookup ? iface_lookup : "");
      free(response);
      free(message);
      return ret;
   }

   proxy = proxy_list_get(iface_entry->proxy_id);

   DEBUG_LOG("Proxy: Passing CALL command to proxy #%i", iface_entry->proxy_id);

   conn = proxy_get_free_conn(iface_entry->proxy_id);
   if (conn == NULL) {
      proxy_mark_failure(proxy, "conn-unavailable");
      stream_writeln(client_conn->sock, "503 Service Unavailable");
      ret = 0;
      responded = 1;
      goto cleanup;
   }
   proxy_set_conn_busy(conn);

   bzero(response, BUF_SZ);
   bzero(message, BUF_SZ);

   if (payload != NULL) {
      const char *enc_value = (encoding != NULL && encoding[0] != '\0') ? encoding : "NONE";

      if (stream_writeln(conn->client.sock, "CALL %s %s PAYLOAD %s %zu", iface_lookup, cmd, enc_value, payload_len) < 0) {
         status_code = 503;
         snprintf(message, BUF_SZ, "Service Unavailable");
         ret = 0;
         responded = 1;
         stream_writeln(client_conn->sock, "%i %s", status_code, message);
         proxy_mark_failure(proxy, "call-send-failed");
         goto cleanup;
      }

      if (payload_len > 0 && !proxy_write_all(conn->client.sock, payload, payload_len)) {
         status_code = 503;
         snprintf(message, BUF_SZ, "Service Unavailable");
         ret = 0;
         responded = 1;
         stream_writeln(client_conn->sock, "%i %s", status_code, message);
         proxy_mark_failure(proxy, "call-payload-write-failed");
         goto cleanup;
      }
   } else {
      if (stream_writeln(conn->client.sock, "CALL %s %s %s", iface_lookup, cmd, arg) < 0) {
         status_code = 503;
         snprintf(message, BUF_SZ, "Service Unavailable");
         ret = 0;
         responded = 1;
         stream_writeln(client_conn->sock, "%i %s", status_code, message);
         proxy_mark_failure(proxy, "call-send-failed");
         goto cleanup;
      }
   }

   if (stream_readln(conn->client.sock, response, BUF_SZ) < 0) {
      status_code = 503;
      snprintf(message, BUF_SZ, "Service Unavailable");
      ret = 0;
      responded = 1;
      stream_writeln(client_conn->sock, "%i %s", status_code, message);
      proxy_mark_failure(proxy, "call-read-failed");
      goto cleanup;
   }

   if (!client_parse_status_code(response, &status_code, message)) {
      status_code = 503;
      snprintf(message, BUF_SZ, "Service Unavailable");
      ret = 0;
      responded = 1;
      stream_writeln(client_conn->sock, "%i %s", status_code, message);
      proxy_mark_failure(proxy, "call-parse-failed");
      goto cleanup;
   }

   if (response[strlen(response) - 1] == ':') {
      if (stream_readlns(conn->client.sock, response, BUF_SZ) < 0) {
         ret = 0;
         goto cleanup;
      }

      size_t remaining = BUF_SZ - strlen(message) - 1;
      strncat(message, "\n", remaining);
      remaining = BUF_SZ - strlen(message) - 1;
      strncat(message, response, remaining);
   }

   char response_enc[BUF_SZ + 1];
   char response_keyword[BUF_SZ + 1];
   unsigned long long response_len = 0;
   int is_payload_response = 0;

   bzero(response_enc, sizeof(response_enc));
   bzero(response_keyword, sizeof(response_keyword));

   if (status_code == 200 || status_code == 500) {
      if (sscanf(message, "%s %s %llu", response_keyword, response_enc, &response_len) == 3 &&
          strcmp(response_keyword, "PAYLOAD") == 0 &&
          response_len <= SIZE_MAX) {
         if ((size_t)response_len > var_max_payload) {
            snprintf(message, BUF_SZ, "Payload response exceeds proxy limit");
            status_code = 505;
            proxy_discard_bytes(conn->client.sock, (size_t)response_len);
         } else {
            is_payload_response = 1;
            payload_response = malloc((size_t)response_len + 1);

            if (payload_response == NULL) {
               ret = 0;
               goto cleanup;
            }

            size_t received = 0;
            while (received < (size_t)response_len) {
               ssize_t n = read(conn->client.sock, payload_response + received, (size_t)response_len - received);

               if (n < 0) {
                  ret = 0;
                  goto cleanup;
               }

               if (n == 0)
                  break;

               received += (size_t)n;
            }

            if (received < (size_t)response_len) {
               ret = 0;
               goto cleanup;
            }

            payload_response[(size_t)response_len] = '\0';
         }
      }
   }

   if (is_payload_response && payload_response != NULL) {
      if (client_conn->protocol_version >= 2) {
         stream_writeln(client_conn->sock, "%i PAYLOAD %s %llu", status_code, response_enc, response_len);

         if (response_len > 0)
            proxy_write_all(client_conn->sock, (const uint8_t *)payload_response, (size_t)response_len);
         responded = 1;
      } else {
         size_t encoded_len = (((size_t)response_len + 2) / 3) * 4 + 1;
         char *encoded = malloc(encoded_len);

         if (encoded == NULL) {
            ret = 0;
            goto cleanup;
         }

         if (response_len > 0)
            to64frombits((unsigned char *)encoded, (const unsigned char *)payload_response, (int)response_len);
         else
            encoded[0] = '\0';

         const char *encoding_label = response_enc[0] != '\0' ? response_enc : "PLAIN";

         stream_writeln(client_conn->sock, "%i %s:%s", status_code, encoding_label, encoded);
         free(encoded);
         responded = 1;
      }
   } else {
      stream_writeln(client_conn->sock, "%i %s", status_code, message);
      responded = 1;
   }

cleanup:
   if (ret == 0 && status_code == 200)
      status_code = 503;

   if (!responded) {
      if (status_code == 0)
         status_code = 503;

      if (message[0] == '\0')
         snprintf(message, BUF_SZ, "Service Unavailable");

      stream_writeln(client_conn->sock, "%i %s", status_code, message);
      responded = 1;
   }

   if (proxy != NULL) {
      if (status_code == 200)
         proxy_mark_success(proxy);
      else if (ret == 0 || status_code == 503)
         proxy_mark_failure(proxy, "call-failed");
   }

   if (conn != NULL)
      proxy_set_conn_free(conn);
   free(response);
   free(message);
   free(payload_response);

   return ret;
}

int proxy_pass_call_cmd(char *iface, char *cmd, char *arg, struct conn_state *client_conn)
{
   return proxy_pass_call_cmd_ex(iface, cmd, arg, NULL, 0, NULL, client_conn);
}

void proxy_print_ifaces(struct conn_state *state)
{
   struct proxy_iface_entry *entry;
   char line[BUF_SZ + 1];
   size_t offset = 0;
   int is_first = 1;

   proxy_append_format(line, sizeof(line), &offset, "200 JSON [");

   entry = proxy_iface_map;
   while (entry != NULL) {
      proxy_json_array_append(line, sizeof(line), &offset, entry->iface, is_first);
      is_first = 0;
      entry = entry->next;
   }

   proxy_append_format(line, sizeof(line), &offset, "]");
   stream_writeln(state->sock, "%s", line);
}

int proxy_get_spare_conn_count(int proxy_id)
{
   int i, count = 0;

   for (i = 0; i < proxy_list[proxy_id].conn_count; i++) {
      if (!proxy_list[proxy_id].conns[i].is_busy)
         count++;
   }

   return count;
}

void proxy_check_spare_conns(int proxy_id)
{
   int i, spare_conns = proxy_get_spare_conn_count(proxy_id);

   for(i = spare_conns; i < var_proxy_min_spare_conns; i++)
      proxy_start_new_conn(proxy_id);
}

void proxy_all_check_spare_conns()
{
   int id;

   for(id = 0; id < proxy_count; id++)
      proxy_check_spare_conns(id);  
}

//
// Proxy child list
//


struct proxy_child_entry *proxy_get_last_child_entry()
{
   struct proxy_child_entry *cur;

   if (proxy_child_list == NULL)
      return NULL;

   cur = proxy_child_list;

   while (cur->next != NULL) {
      cur = cur->next;
   }

   return cur;
}

struct proxy_child_entry *new_proxy_child_entry()
{
   struct proxy_child_entry *result = malloc(sizeof(struct proxy_child_entry));

   result->pid = 0;
   result->prev = proxy_get_last_child_entry();
   result->next = NULL;

   return result;
}

struct proxy_child_entry *proxy_add_child_entry()
{
   struct proxy_child_entry *entry = new_proxy_child_entry();

   if (proxy_child_list == NULL) // adding first item
      proxy_child_list = entry;
   else
      proxy_get_last_child_entry()->next = entry;

   return entry;
}


int proxy_conn_keepalive(struct proxy_conn *conn)
{
   int status_code, ret = 1;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));
   struct proxy_server *proxy = proxy_list_get(conn->proxy_id);

   bzero(response, BUF_SZ);

   proxy_set_conn_busy(conn);
   status_code = client_raw_cmd(&conn->client, "PING", response);

   // Try to reconnect stale connection
   if (!status_code || status_code == 408) {  // 408 - request timeout
      if (proxy_restart_conn(conn))
         status_code = client_raw_cmd(&conn->client, "PING", response);
      else
         status_code = 503;
   }

   if (!status_code || status_code == 503) {
      status_code = 503;
      log_unexpected_event(LOG_WARNING, "proxy-keepalive-failed proxy_id=%i", conn->proxy_id);
      proxy_mark_failure(proxy, "keepalive-failed");
      ret = 0;
   } else {
      proxy_mark_success(proxy);
   }
   proxy_set_conn_free(conn);

   free(response);
   return ret;
}

void proxy_all_keepalive_conns()
{
   int i, j;
   struct proxy_server *proxy;

   for (i = 0; i < proxy_list_count(); i++) {
      proxy = proxy_list_get(i);

      if (proxy == NULL)
         continue;

      for (j = 0; j < proxy_list[i].conn_count; j++) {
         if (!proxy_list[i].conns[j].is_busy)
            proxy_conn_keepalive(&proxy_list[i].conns[j]);
      }
   }
}

//void proxy_

/**
 * Starts new proxy link
 * 
 */
int proxy_start_new(const char *proxy_ip, int proxy_port, const char *unix_socket_path)
{
   int id, i;

   DEBUG_LOG("Starting proxy to %s",
      unix_socket_path ? unix_socket_path : proxy_ip);

   // Add new proxy struct to the list
   id = proxy_list_add_new(proxy_ip, proxy_port, unix_socket_path);

   for(i = 0; i < var_proxy_min_spare_conns; i++)
      proxy_start_new_conn(id);

   proxy_fetch_interfaces(id);
   return 1;
}

/**
 * Starts the proxy server functionality
 * 
 */
int proxy_start(char *host_addresses)
{
   int proxy_port, byte, size = 0;
   char *hostname = malloc(sizeof(char) * (PROXY_HOSTNAME_SZ + 1));
   char *address = malloc(sizeof(char) * (PROXY_HOSTNAME_SZ + 1));
   char *unix_socket = malloc(sizeof(char) * (PROXY_UNIX_PATH_SZ + 1));
   char *proxy_ip;

   bzero(hostname, PROXY_HOSTNAME_SZ);
   bzero(address, PROXY_HOSTNAME_SZ);
   bzero(unix_socket, PROXY_UNIX_PATH_SZ);

   // Parse list of proxy servers separated by ","
   for(byte = 0; byte <= strlen(host_addresses); byte++) {
      if (host_addresses[byte] == ' ') // skip spaces
         continue;

      if (host_addresses[byte] == ',' || host_addresses[byte] == '\0') {
         if (!parse_proxy_address(address, hostname, PROXY_HOSTNAME_SZ, &proxy_port, unix_socket, PROXY_UNIX_PATH_SZ)) {
            log_unexpected_event(LOG_ERR, "proxy-parse-failed address=%s", address);
            goto reset_state;
         }

         if (unix_socket[0] != '\0') {
            proxy_start_new(NULL, 0, unix_socket);
         } else {
            proxy_ip = get_ip_addr(hostname);
 
            if (proxy_ip == NULL) {
               log_unexpected_event(LOG_ERR, "proxy-resolve-failed host=%s", hostname);
               goto reset_state;
            }
            proxy_start_new(proxy_ip, proxy_port, NULL);
            free(proxy_ip);
         }

reset_state:
         bzero(hostname, PROXY_HOSTNAME_SZ);
         bzero(address, PROXY_HOSTNAME_SZ);
         bzero(unix_socket, PROXY_UNIX_PATH_SZ);
         size = 0;
         continue;
      }

      address[size] = host_addresses[byte];
      size++;
   }

   free(hostname);
   free(address);
   free(unix_socket);

   return 1;
}
