#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdint.h>
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
int handle_shell_call(
   char *file,
   char *method,
   char *arg,
   const uint8_t *payload,
   size_t payload_len,
   const char *encoding,
   struct conn_state *state)
{
   int exit_code;
   FILE *pipe;
   //int *pipe;
   char *decoded_arg = malloc(sizeof(char) * (BUF_SZ + 1));
   char *output = malloc(sizeof(char) * (BUF_SZ + 1));
   char *payload_path = NULL;
   char shell_cmd[(BUF_SZ + 1)];

   if (decoded_arg == NULL || output == NULL) {
      stream_writeln(state->sock, "500 Error preparing call buffer");
      free(decoded_arg);
      free(output);
      return 0;
   }

   bzero(decoded_arg, BUF_SZ);
   bzero(output, BUF_SZ);
   bzero(shell_cmd, BUF_SZ);

   if (encoding != NULL)
      setenv("PAYLOAD_ENCODING", encoding, 1);

   if (payload != NULL) {
      int fd;
      ssize_t written;
      char tmp_filename[] = "/tmp/cloudrmi_payloadXXXXXX";

      fd = mkstemp(tmp_filename);
      if (fd == -1) {
         stream_writeln(state->sock, "500 Error preparing payload buffer");
         free(decoded_arg);
         free(output);
         return 0;
      }

      written = write(fd, payload, payload_len);
      if (written < 0 || (size_t)written != payload_len) {
         stream_writeln(state->sock, "500 Error writing payload buffer");
         close(fd);
         unlink(tmp_filename);
         free(decoded_arg);
         free(output);
         return 0;
      }

      close(fd);
      payload_path = strdup(tmp_filename);

      if (payload_path == NULL) {
         stream_writeln(state->sock, "500 Error preparing payload path");
         unlink(tmp_filename);
         free(decoded_arg);
         free(output);
         return 0;
      }

      snprintf(shell_cmd, BUF_SZ, "%s %s %s 2>&1", file, method, payload_path);

   } else {
      // base64 decode argument list (legacy mode)
      if (!from64tobits(decoded_arg, arg)) {
         stream_writeln(state->sock, "500 Error decoding call arguments");
         free(decoded_arg);
         free(output);
         return 0;
      }

      snprintf(shell_cmd, BUF_SZ, "%s %s %s 2>&1", file, method, decoded_arg);
   }

   // call the shell command, using 2>&1 hack to catch errout with popen,
   // we can also use 2>&1 >/dev/null to get only the err output
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
      bzero(decoded_arg, BUF_SZ);

      if (strlen(output))
         to64frombits((unsigned char *)decoded_arg, (const unsigned char *)output, strlen(output));

      stream_writeln(state->sock, "200 %s", decoded_arg);
   }

   free(decoded_arg);
   free(output);
   if (payload_path != NULL) {
      unlink(payload_path);
      free(payload_path);
   }

   return 1;
}

/**
 * Handles RMI 'call' command.
 * 
 */
int handle_call_cmd(
   char *iface,
   char *method,
   char *arg,
   const uint8_t *payload,
   size_t payload_len,
   const char *encoding,
   struct conn_state *state)
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
      return handle_shell_call(
         iface_entry->file,
         method,
         arg,
         payload,
         payload_len,
         encoding,
         state);

   } else {
      stream_writeln(state->sock, "400 Unsupported interface: %s", iface);
   }

   return 0;
}
