#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <syslog.h>
#include "cloud-rmi.h"
#include "config.h"
#include "capabilities.h"
#ifdef PROXY_SUPPORT
   #include "proxy.h"
#endif

#define SERVER_MAX_PROTOCOL_VERSION CLOUDRMI_PROTOCOL_VERSION
#define MAX_CAP_TOKENS 16

/**
 * Handles Cloud Simple RMI protocol and server side communication.
 *
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @project cloud-rmi
 * @author Marian Tomlak
 * @license BSD
 */


struct child_debug{
   int sock;
   int pid;
};

static size_t bounded_strlen(const char *value, size_t cap)
{
   size_t len = 0;

   if (value == NULL)
      return 0;

   while (len < cap && value[len] != '\0')
      len++;

   return len;
}

static void log_request_summary(const rmi_event_t *event, const struct conn_state *state)
{
   if (!debug_traffic_enabled() || event == NULL)
      return;

   switch (event->type) {
   case RMI_EVENT_CALL: {
      size_t payload_len = event->payload ? event->payload_len : 0;
      const char *encoding = event->encoding ? event->encoding : "";
      log_traffic_event(
         state,
         "request CALL iface=%s method=%s argc=%i arg_len=%zu payload_len=%zu encoding=%s",
         event->arg1 ? event->arg1 : "",
         event->arg2 ? event->arg2 : "",
         event->argc,
         bounded_strlen(event->arg3, 96),
         payload_len,
         encoding);
      break;
   }

   case RMI_EVENT_PROTO:
      log_traffic_event(state, "request PROTO %s", event->arg1 ? event->arg1 : "");
      break;

   case RMI_EVENT_DEBUG:
      log_traffic_event(state, "request DEBUG %s", event->arg1 ? event->arg1 : "");
      break;

   default:
      log_traffic_event(state, "request type=%d", event->type);
      break;
   }
}

static void log_parser_anomaly(rmi_status_t status, const rmi_error_t *err, const struct conn_state *state)
{
   const char *label = (status == RMI_E_OVERFLOW) ? "overflow" : "error";
   int code = err ? err->code : 0;

   log_unexpected_event(
      LOG_WARNING,
      "parser-%s status=%d code=%d proto=%d unix=%d",
      label,
      status,
      code,
      state ? state->protocol_version : 0,
      state ? state->is_unix_socket : 0);
}

static size_t json_escape_into(char *dst, size_t dst_size, const char *src)
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

static int append_format(char *buf, size_t buf_sz, size_t *offset, const char *fmt, ...)
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

static void json_array_append(char *buf, size_t buf_sz, size_t *offset, const char *value, int is_first)
{
   char escaped[BUF_SZ + 1];

   json_escape_into(escaped, sizeof(escaped), value);

   append_format(buf, buf_sz, offset, "%s\"%s\"", is_first ? "" : ",", escaped);
}

static void json_array_write_line(int sock, const char **items, size_t count)
{
   size_t offset = 0;
   char line[BUF_SZ + 1];

   append_format(line, sizeof(line), &offset, "200 JSON [");

   for (size_t i = 0; i < count; i++) {
      json_array_append(line, sizeof(line), &offset, items[i], i == 0);
   }

   append_format(line, sizeof(line), &offset, "]");
   stream_writeln(sock, "%s", line);
}

static void json_array_write_start(char *buf, size_t buf_sz, size_t *offset)
{
   if (buf == NULL || offset == NULL)
      return;

   *offset = 0;
   append_format(buf, buf_sz, offset, "200 JSON [");
}

static void json_array_write_end(char *buf, size_t buf_sz, size_t *offset)
{
   append_format(buf, buf_sz, offset, "]");
}

/**
 * Handles 'AUTH' command. 
 * WARNING: This version supports dev backdoor.
 *
 */
int handle_auth_cmd(char *cmd, char *arg, struct conn_state *state)
{
   if (state->is_authenticated) {
      stream_writeln(state->sock, "200 Already logged in");     // todo: "... as: xyz"
      return 1;
   }

   if (strcmp(cmd, "TOKEN") == 0) { 
      if (strcmp(arg, var_recovery_token) == 0) {
         state->is_authenticated = 1;
         strcpy(state->username, "RecoveryUser");
         stream_writeln(state->sock, "200 Hello %s", state->username);
         return 1;

      } else {
         stream_writeln(state->sock, "401 Unauthorized");
         return 0;
      }

   } else if (strcmp(cmd, "dbg") == 0) { 
      state->is_authenticated = 1;
      strcpy(state->username, "Developer");
      stream_writeln(state->sock, "200 Hello %s", state->username);   
      return 1; 

   } else {
      stream_writeln(state->sock, "400 Bad Request Arguments");
   }
}

void handle_print_ifaces(struct conn_state *state)
{
   struct server_iface_entry *entry;
   char line[BUF_SZ + 1];
   size_t offset;
   int is_first = 1;

   entry = var_server_iface_map;

   json_array_write_start(line, sizeof(line), &offset);
   while (entry != NULL) {
      json_array_append(line, sizeof(line), &offset, entry->iface, is_first);
      is_first = 0;
      entry = entry->next;
   }
   json_array_write_end(line, sizeof(line), &offset);
   stream_writeln(state->sock, "%s", line);
}

static int handle_command_event(rmi_event_t *event, struct conn_state *state, rmi_protocol_ctx_t *protocol_ctx)
{
   int time_diff;
   char output[BUF_SZ + 1];
   time_t t;
   struct tm *tmp;

   memset(output, 0, sizeof(output));

   log_request_summary(event, state);

   switch (event->type) {
   case RMI_EVENT_AUTH:
      if (!handle_auth_cmd(event->arg1, event->arg2, state)) {
         stream_writeln(state->sock, "401 Unauthorized");
         return 0;
      }
      break;

   case RMI_EVENT_PROTO: {
      int requested = atoi(event->arg1);

      if (requested < 1 || requested > SERVER_MAX_PROTOCOL_VERSION) {
         stream_writeln(state->sock, "400 Bad Request");
         return 0;
      }

      if (state->is_unix_socket && requested < 2) {
         stream_writeln(state->sock, "400 Bad Request");
         if (protocol_ctx != NULL)
            rmi_protocol_set_version(protocol_ctx, state->protocol_version);
         return 0;
      }

      state->protocol_version = requested;
      stream_writeln(state->sock, "200 OK");
      log_traffic_event(state, "protocol-switch version=%i", requested);
      break;
   }

   case RMI_EVENT_CALL:
      if (state->is_authenticated) {
         int has_payload = event->payload != NULL;
         if (!has_payload && event->argc < 3)
            stream_writeln(state->sock, "400 Bad Request Arguments");

         else {
#ifdef PROXY_SUPPORT
         if (var_proxy)
            proxy_pass_call_cmd_ex(
               event->arg1,
               event->arg2,
               event->arg3,
               event->payload,
               event->payload_len,
               event->encoding,
               state);
         else
#endif
         handle_call_cmd(
            event->arg1,
            event->arg2,
            event->arg3,
            event->payload,
            event->payload_len,
            event->encoding,
            state);
         }
      } else
         stream_writeln(state->sock, "401 Unauthorized");
      break;

   case RMI_EVENT_HELP: {
      const char *items[16];
      size_t idx = 0;

      items[idx++] = "AUTH";
      items[idx++] = "CALL";
      items[idx++] = "CAPS";
      items[idx++] = "DEBUG";
      items[idx++] = "HELP";
      items[idx++] = "IFACES";
      items[idx++] = "PING";
      items[idx++] = "PROTO";
      items[idx++] = "RELOAD";
      items[idx++] = "SERVER";
      items[idx++] = "TIME";
      items[idx++] = "UPTIME";
      items[idx++] = "QUIT";
#ifdef PROXY_SUPPORT
      if (var_proxy && idx < sizeof(items) / sizeof(items[0]))
         items[idx++] = "PROXY";
#endif

      json_array_write_line(state->sock, items, idx);
      break;
   }

   case RMI_EVENT_IFACES:
#ifdef PROXY_SUPPORT
      if (var_proxy)
         proxy_print_ifaces(state);
      else
#endif
      handle_print_ifaces(state);
      break;

   case RMI_EVENT_RELOAD:
      if (state->is_authenticated) {
         stream_writeln(state->sock, "200 Reloading configuration");
         read_config(CONFIGURATION_FILE);
         read_iface_config(INTERFACE_MAP_FILE);

      } else
         stream_writeln(state->sock, "401 Unauthorized");
      break;

   case RMI_EVENT_SHUTDOWN:
      stream_writeln(state->sock, "404 Unsupported");
      break;

   case RMI_EVENT_TIME: // RFC822 time string
      t = time(NULL);
      tmp = localtime(&t);

      if (tmp == NULL) {
         log_event(LOG_ERR, "Error: localtime() failed");
         stream_writeln(state->sock, "500 Error retrieving time");
         break;
      }

      if (strftime(output, BUF_SZ, "%a, %d %b %y %T %z", tmp) == 0) {
         log_event(LOG_ERR, "Error: strftime() failed");
         stream_writeln(state->sock, "500 Error retrieving time");
         break;
      }

      stream_writeln(state->sock, "200 %s", output);
      break;

   case RMI_EVENT_UPTIME:
      if (state->is_authenticated) {
         time_diff = (int)time(NULL) - var_server_start_time;

         stream_writeln(
            state->sock,
            "200 Server up for %i days, %i hrs, %i min, %i sec",
            (int)floor(time_diff / 86400),
            (int)floor(fmod(time_diff, 86400) / 3600),
            (int)floor(fmod(time_diff, 3600) / 60),
            (int)fmod(time_diff, 60)
            );
      } else
         stream_writeln(state->sock, "401 Unauthorized");
      break;

   case RMI_EVENT_PING:
      stream_writeln(state->sock, "200 OK");
      break;

   case RMI_EVENT_CAPS: {
      char caps_buf[BUF_SZ + 1];
      char *token;
      const char *items[MAX_CAP_TOKENS];
      size_t idx = 0;

      rmi_capabilities_to_tokens(rmi_get_compiled_capabilities(), caps_buf, sizeof(caps_buf));

      token = strtok(caps_buf, " ");
      while (token != NULL && idx < (sizeof(items) / sizeof(items[0]))) {
         items[idx++] = token;
         token = strtok(NULL, " ");
      }

      json_array_write_line(state->sock, items, idx);
      break;
   }

   case RMI_EVENT_DEBUG:
      if (state->is_authenticated) {
         if (strcmp(event->arg1, "on") == 0) {
            debug_log_set(1);
         } else if (strcmp(event->arg1, "off") == 0) {
            debug_log_set(0);
         } else if (strcmp(event->arg1, "traffic") == 0) {
            if (event->arg2 != NULL && strcmp(event->arg2, "on") == 0) {
               debug_traffic_set(1);
               stream_writeln(state->sock, "200 Debug traffic logging ON");
            } else if (event->arg2 != NULL && strcmp(event->arg2, "off") == 0) {
               debug_traffic_set(0);
               stream_writeln(state->sock, "200 Debug traffic logging OFF");
            } else {
               stream_writeln(
                  state->sock,
                  "200 Debug traffic is %s",
                  debug_traffic_enabled() ? "ON" : "OFF");
            }
         } else {
            char line[BUF_SZ + 1];
            char config_path[BUF_SZ + 1];
            char iface_path[BUF_SZ + 1];
            size_t offset = 0;
            const char *language_label = "C (libc)";
            const char *kernel_label = "POSIX";
            const char *proxy_label = "NO";

#ifdef ENABLE_LINUX_FEATURES
            language_label = "C (glibc)";
            kernel_label = "Linux";
#endif
#ifdef PROXY_SUPPORT
            proxy_label = var_proxy ? "YES" : "NO";
#endif

            json_escape_into(config_path, sizeof(config_path), var_config_file);
            json_escape_into(iface_path, sizeof(iface_path), var_interface_map_file);

            append_format(
               line,
               sizeof(line),
               &offset,
               "200 JSON {\"build\":{\"language\":\"%s\",\"kernel\":\"%s\",\"proxy\":\"%s\"},\"runtime\":{\"pid\":%i,\"config\":\"%s\",\"iface_map\":\"%s\"}}",
               language_label,
               kernel_label,
               proxy_label,
               (int)getpid(),
               config_path,
               iface_path);

            stream_writeln(state->sock, "%s", line);
         }
      } else {
         stream_writeln(state->sock, "401 Unauthorized");
      }
      break;

   case RMI_EVENT_QUIT:
      stream_writeln(state->sock, "221 Bye");
      return 0;

#ifdef PROXY_SUPPORT
   case RMI_EVENT_PROXY:
      if (strcmp(event->arg1, "stats") == 0) {
         proxy_send_stats(state->sock);
      } else {
         stream_writeln(state->sock, "400 Bad Request Arguments");
      }
      break;
#endif

   case RMI_EVENT_SERVER:
      if (strcmp(event->arg1, "stats") == 0) {
         server_send_stats(state->sock);
      } else {
         stream_writeln(state->sock, "400 Bad Request Arguments");
      }
      break;

   case RMI_EVENT_UNKNOWN:
   case RMI_EVENT_NONE:
   default:
      stream_writeln(state->sock, "400 Bad Request");
   }

   return 1;
}

/**
 * Handles a single socket connection to the client.
 * 
 */
void handle_connection (int sock, int is_unix_socket)
{
   int loop = 1, bytes = 0;
   char buf[(BUF_SZ + 1)];
   struct conn_state state;
   struct child_debug debug;
   rmi_protocol_ctx_t *protocol_ctx = rmi_protocol_create();
   if (protocol_ctx == NULL)
      return;
   rmi_protocol_set_max_payload(protocol_ctx, var_max_payload);

   memset(&state, 0, sizeof(state));
   state.sock = sock;
   state.is_unix_socket = is_unix_socket;
   state.protocol_version = is_unix_socket ? 2 : 1;
   state.is_authenticated = (is_unix_socket && var_unix_socket_skip_auth);

   if (state.is_authenticated)
      strncpy(state.username, "UnixSocket", sizeof(state.username) - 1);

   if (is_unix_socket && !rmi_protocol_set_version(protocol_ctx, state.protocol_version)) {
      rmi_protocol_destroy(protocol_ctx);
      return;
   }

   // Write welcome message
   char caps_buf[BUF_SZ + 1];
   rmi_capabilities_to_tokens(rmi_get_compiled_capabilities(), caps_buf, sizeof(caps_buf));

   for (size_t i = 0; caps_buf[i]; i++)
      if (caps_buf[i] == ' ')
         caps_buf[i] = ',';

   if (caps_buf[0] != '\0')
      stream_writeln(sock, "220 %s Server Ready EXT PROTO=2,%s", var_context_name, caps_buf);
   else
      stream_writeln(sock, "220 %s Server Ready EXT PROTO=2", var_context_name);
   debug.sock = sock;
   debug.pid = (int) getpid();
   log_traffic_event(&state, "connection-open");

   while (loop) {
      bytes = stream_read(state.sock, buf, BUF_SZ);
      if (bytes <= 0) {
         if (bytes < 0)
            log_unexpected_event(LOG_WARNING, "stream_read failed on connection proto=%d", state.protocol_version);

         break;
      }

      rmi_error_t err;
      memset(&err, 0, sizeof(err));
      rmi_status_t status = rmi_protocol_feed(protocol_ctx, (const uint8_t *)buf, (size_t)bytes, &err);

      if (status == RMI_E_OVERFLOW || status == RMI_E_ERR) {
         log_parser_anomaly(status, &err, &state);
         stream_writeln(state.sock, "400 Bad Request");
         loop = 0;
         continue;
      }

      do {
         rmi_event_t event;
         rmi_error_t parse_err;
         memset(&parse_err, 0, sizeof(parse_err));

         status = rmi_protocol_next_event(protocol_ctx, &event, &parse_err);

         if (status == RMI_OK) {
            if (parse_err.code == RMI_ERROR_BAD_REQUEST) {
               log_parser_anomaly(status, &parse_err, &state);
               stream_writeln(state.sock, "400 Bad Request");
               loop = 1;
            } else if (parse_err.code == RMI_ERROR_BAD_ARGS) {
               log_parser_anomaly(status, &parse_err, &state);
               stream_writeln(state.sock, "400 Bad Request Arguments");
               loop = 1;
            } else {
               loop = handle_command_event(&event, &state, protocol_ctx);
            }
         } else if (status == RMI_E_ERR) {
            if (parse_err.code == RMI_ERROR_BAD_ARGS) {
               log_parser_anomaly(status, &parse_err, &state);
               stream_writeln(state.sock, "400 Bad Request Arguments");
            } else if (parse_err.code != RMI_ERROR_NONE) {
               log_parser_anomaly(status, &parse_err, &state);
               stream_writeln(state.sock, "400 Bad Request");
            }
         }
      } while (status == RMI_OK && loop);
   }

   rmi_protocol_destroy(protocol_ctx);

   if (debug_log_enabled() && !loop) DEBUG_LOG("handle_connection: Received exit command from client");
   if (debug_log_enabled() && !bytes) DEBUG_LOG("handle_connection: Client connection closed");
   log_traffic_event(&state, "connection-close");
}
