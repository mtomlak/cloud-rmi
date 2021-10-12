#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include "cloud-rmi.h"
#include "config.h"

/**
 * Handles specific RMI call.
 *
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @project cloud-rmi
 * @author Marian Tomlak
 * @license BSD
 */

/**
 * Executes RMI command as a shell script.
 * 
 */
int handle_shell_call(char *file, char *method, char *arg, struct conn_state *state)
{
   int exit_code;
   FILE *pipe;
   //int *pipe;
   char *payload = malloc(sizeof(char) * (BUF_SZ + 1));
   char *output = malloc(sizeof(char) * (BUF_SZ + 1));
   char shell_cmd[(BUF_SZ + 1)];

   bzero(payload, BUF_SZ);
   bzero(output, BUF_SZ);
   bzero(shell_cmd, BUF_SZ);

   // base64 decode argument list
   if (!from64tobits(payload, arg)) {
      stream_writeln(state->sock, "500 Error decoding call arguments");
      return 0;
   }

   // call the shell command, using 2>&1 hack to catch errout with popen,
   // we can also use 2>&1 >/dev/null to get only the err output
   snprintf(shell_cmd, BUF_SZ, "%s %s %s 2>&1", file, method, payload);   
   pipe = popen(shell_cmd, "r");
   stream_read(fileno(pipe), output, BUF_SZ);   // we could read in loop and increase buffer
   exit_code = pclose(pipe);

   // send the output or status code on error
   if (strncmp(output, "Error", 5) == 0) {
      stream_writeln(state->sock, "500 Exited with error message: %s", output);

   } else if (exit_code != 0) {
      stream_writeln(state->sock, "500 Error: %s (%d)", output, exit_code);

   } else {
      // base64 encode the output
      bzero(payload, BUF_SZ);

      if (strlen(output))
         to64frombits(payload, output, strlen(output));
      
      stream_writeln(state->sock, "200 %s", payload);
   }

   free(payload);
   free(output);
}

/**
 * Handles RMI 'call' command.
 * 
 */
int handle_call_cmd(char *iface, char *method, char *arg, struct conn_state *state)
{
   struct server_iface_entry *iface_entry;

   // Authentication check
   if (!state->is_authenticated) {
      stream_writeln(state->sock, "401 Unauthorized");     // todo: "... as: xyz"
      return 0;
   }
   iface_entry = server_get_iface_entry(iface);

   // Check whether it's local script
   if (iface_entry != NULL) {   
      return handle_shell_call(iface_entry->file, method, arg, state);   

   } else {
      stream_writeln(state->sock, "400 Unsupported interface: %s", iface);
   }
}
