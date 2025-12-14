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

#include "cloud-rmi.h"
#include "cloudrmi/rmi_protocol.h"
#include "cloudrmi/rmi_server.h"
#include "cloudrmi/rmi_connection.h"
#include "config.h"
#ifdef PROXY_SUPPORT
   #include "proxy.h"
#endif

struct child_debug{
   int sock;
   int pid;
};

static int handle_auth_cmd(rmi_server_t *server, char *cmd, char *arg, rmi_connection_t *state)
{
   if (state->is_authenticated) {
      stream_writeln(state->sock, "200 Already logged in");
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

   return 0;
}

static void handle_print_ifaces(rmi_connection_t *state)
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

int rmi_handle_command(rmi_server_t *server, char *line, rmi_connection_t *state)
{
   int argc = 0, time_diff;
   char cmd[(BUF_SZ + 1)], arg1[(BUF_SZ + 1)], arg2[(BUF_SZ + 1)], arg3[(BUF_SZ + 1)], *p;
   char *output = malloc(sizeof(char) * (BUF_SZ + 1));
   time_t t;
   struct tm *tmp;

   bzero(output, BUF_SZ);

   argc = sscanf(line, "%s %s %s %s", cmd, arg1, arg2, arg3);

   for (p = cmd; *p; ++p) *p = tolower(*p);

   if (strcmp(cmd, "auth") == 0) {
      if (!handle_auth_cmd(server, arg1, arg2, state)) {
         stream_writeln(state->sock, "401 Unauthorized");
         free(output);
         return 0;
      }

   } else if (strcmp(cmd, "call") == 0) {
      if (argc < 3)
         stream_writeln(state->sock, "400 Bad Request Arguments");

      else if (state->is_authenticated) {
#ifdef PROXY_SUPPORT
         if (var_proxy)
            proxy_pass_call_cmd(arg1, arg2, arg3, (struct conn_state *)state);
         else
#endif
         rmi_handle_call_cmd(server, arg1, arg2, arg3, state);
      } else
         stream_writeln(state->sock, "401 Unauthorized");

   } else if (strcmp(cmd, "help") == 0) {
      stream_writeln(state->sock, "200 Supported commands:\r\nAUTH\r\nCALL\r\nHELP\r\nIFACES\r\nNOOP");
#ifdef PROXY_SUPPORT
      stream_writeln(state->sock, "PROXY");
#endif
      stream_writeln(state->sock, "SERVER\r\nRELOAD\r\nTIME\r\nUPTIME\r\nQUIT\r\n");

   } else if (strcmp(cmd, "ifaces") == 0) {
#ifdef PROXY_SUPPORT
      if (var_proxy)
         proxy_print_ifaces((struct conn_state *)state);
      else
#endif
      handle_print_ifaces(state);

   } else if (strcmp(cmd, "reload") == 0) {
      if (state->is_authenticated) {
         stream_writeln(state->sock, "200 Reloading configuration");
         read_config(CONFIGURATION_FILE);
         read_iface_config(INTERFACE_MAP_FILE);

      } else
         stream_writeln(state->sock, "401 Unauthorized");

   } else if (strcmp(cmd, "shutdown") == 0) {
      stream_writeln(state->sock, "404 Unsupported");

   } else if (strcmp(cmd, "time") == 0) {
      t = time(NULL);
      tmp = localtime(&t);

      if (tmp == NULL)
         die_err("Error: localtime() failed");

      if (strftime(output, BUF_SZ, "%a, %d %b %y %T %z", tmp) == 0)
         die_err("Error: strftime() failed");

      stream_writeln(state->sock, "200 %s", output);

   } else if (strcmp(cmd, "uptime") == 0) {
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

   } else if (strcmp(cmd, "noop") == 0) {
      stream_writeln(state->sock, "200 OK");

   } else if (strcmp(cmd, "debug") == 0) {
      if (state->is_authenticated) {
         if (strcmp(arg1, "on") == 0) {
            var_debug = 1;

         } else if (strcmp(arg1, "off") == 0) {
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
            stream_write(state->sock, "\n");
         }
      } else
         stream_writeln(state->sock, "401 Unauthorized");

   } else if ((strcmp(cmd, "quit") == 0) || (strcmp(cmd, "exit") == 0)) {
      stream_writeln(state->sock, "221 Bye");
      free(output);
      return 0;

   }
#ifdef PROXY_SUPPORT
   else if (strcmp(cmd, "proxy") == 0) {
      if (strcmp(arg1, "stats") == 0) {
         stream_writeln(state->sock, "200 Proxy is %s, showing statistics:", var_proxy ? "ON" : "OFF");
         proxy_send_stats(state->sock);
      } else {
         stream_writeln(state->sock, "400 Bad Request Arguments");
      }
   }
#endif
   else if (strcmp(cmd, "server") == 0) {
      if (strcmp(arg1, "stats") == 0) {
         stream_writeln(state->sock, "200 Showing statistics:");
         server_send_stats(state->sock);
      } else {
         stream_writeln(state->sock, "400 Bad Request Arguments");
      }
   }
   else {
      stream_writeln(state->sock, "400 Bad Request");
   }

   free(output);

   return 1;
}

void rmi_handle_connection (rmi_server_t *server, int sock)
{
   int loop, bytes;
   char buf[(BUF_SZ + 1)];
   rmi_connection_t state;
   struct child_debug debug;

   bzero(buf, BUF_SZ);

   stream_writeln(sock, "220 %s Server Ready EXT %s", var_context_name, PROTO_EXTENSIONS);
   rmi_connection_init(&state, sock);

   debug.sock = sock;
   debug.pid = (int) getpid();

   do {
      if (bytes = stream_readln(state.sock, buf, BUF_SZ)) {
         loop = rmi_handle_command(server, buf, &state);
      }

      bzero(buf, BUF_SZ);
   } while (loop && bytes > 0);

   if (var_debug && !loop) printf("handle_connection: Received exit command from client\n");
   if (var_debug && !bytes) printf("handle_connection: Client connection closed\n");
}
