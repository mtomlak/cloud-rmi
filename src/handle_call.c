#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
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
static int write_all(int fd, const uint8_t *buf, size_t len)
{
   size_t written = 0;

   while (written < len) {
      ssize_t n = write(fd, buf + written, len - written);
      if (n < 0) {
         if (errno == EINTR)
            continue;
         return 0;
      }
      written += (size_t)n;
   }

   return 1;
}

static int read_all_available(int fd, char *buf, size_t buf_len)
{
   size_t total = 0;

   while (total < buf_len) {
      ssize_t n = read(fd, buf + total, buf_len - total);
      if (n < 0) {
         if (errno == EINTR)
            continue;
         return 0;
      }

      if (n == 0)
         break;

      total += (size_t)n;
   }

   buf[total] = '\0';
   return 1;
}

int handle_shell_call(
   char *file,
   char *method,
   char *arg,
   const uint8_t *payload,
   size_t payload_len,
   const char *encoding,
   struct conn_state *state)
{
   (void)encoding;

   int exit_code;
   int status;
   pid_t pid;
   int stdout_pipe[2];
   int stdin_pipe[2] = { -1, -1 };
   char *decoded_arg = NULL;
   char *encoded_output = NULL;
   char *output = malloc(sizeof(char) * (BUF_SZ + 1));

   if (output == NULL) {
      stream_writeln(state->sock, "500 Error preparing call buffer");
      return 0;
   }

   bzero(output, BUF_SZ);

   if (payload == NULL) {
      decoded_arg = malloc(sizeof(char) * (BUF_SZ + 1));
      if (decoded_arg == NULL) {
         stream_writeln(state->sock, "500 Error preparing call buffer");
         free(output);
         return 0;
      }

      bzero(decoded_arg, BUF_SZ);

      // base64 decode argument list (legacy mode)
      if (!from64tobits(decoded_arg, arg)) {
         stream_writeln(state->sock, "500 Error decoding call arguments");
         free(decoded_arg);
         free(output);
         return 0;
      }

      encoded_output = decoded_arg;
   } else {
      encoded_output = malloc(sizeof(char) * (BUF_SZ + 1));
      if (encoded_output == NULL) {
         stream_writeln(state->sock, "500 Error preparing response buffer");
         free(output);
         return 0;
      }
      bzero(encoded_output, BUF_SZ);
   }

   if (pipe(stdout_pipe) == -1) {
      stream_writeln(state->sock, "500 Error preparing call pipeline");
      if (encoded_output != decoded_arg)
         free(encoded_output);
      free(decoded_arg);
      free(output);
      return 0;
   }

   if (payload != NULL && pipe(stdin_pipe) == -1) {
      stream_writeln(state->sock, "500 Error preparing payload pipeline");
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      if (encoded_output != decoded_arg)
         free(encoded_output);
      free(output);
      return 0;
   }

   pid = fork();
   if (pid == -1) {
      stream_writeln(state->sock, "500 Error spawning call handler");
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      if (stdin_pipe[0] != -1) {
         close(stdin_pipe[0]);
         close(stdin_pipe[1]);
      }
      if (encoded_output != decoded_arg)
         free(encoded_output);
      free(decoded_arg);
      free(output);
      return 0;
   }

   if (pid == 0) {
      // Child
      close(stdout_pipe[0]);
      if (dup2(stdout_pipe[1], STDOUT_FILENO) == -1)
         _exit(127);
      if (dup2(stdout_pipe[1], STDERR_FILENO) == -1)
         _exit(127);
      close(stdout_pipe[1]);

      if (payload != NULL) {
         close(stdin_pipe[1]);
         if (dup2(stdin_pipe[0], STDIN_FILENO) == -1)
            _exit(127);
         close(stdin_pipe[0]);
      } else {
         // keep parent's stdin untouched for legacy calls
      }

      if (payload != NULL) {
         execl(file, file, method, (char *)NULL);
      } else {
         execl(file, file, method, decoded_arg, (char *)NULL);
      }

      _exit(127);
   }

   // Parent
   close(stdout_pipe[1]);

   if (payload != NULL) {
      close(stdin_pipe[0]);
      if (!write_all(stdin_pipe[1], payload, payload_len)) {
         stream_writeln(state->sock, "500 Error streaming payload");
         close(stdin_pipe[1]);
         close(stdout_pipe[0]);
         waitpid(pid, NULL, 0);
         if (encoded_output != decoded_arg)
            free(encoded_output);
         free(decoded_arg);
         free(output);
         return 0;
      }
      close(stdin_pipe[1]);
   }

   if (!read_all_available(stdout_pipe[0], output, BUF_SZ)) {
      stream_writeln(state->sock, "500 Error reading call output");
      close(stdout_pipe[0]);
      waitpid(pid, NULL, 0);
      if (encoded_output != decoded_arg)
         free(encoded_output);
      free(decoded_arg);
      free(output);
      return 0;
   }

   close(stdout_pipe[0]);

   if (waitpid(pid, &status, 0) == -1) {
      stream_writeln(state->sock, "500 Error waiting for call");
      if (encoded_output != decoded_arg)
         free(encoded_output);
      free(decoded_arg);
      free(output);
      return 0;
   }

   exit_code = (WIFEXITED(status) ? WEXITSTATUS(status) : -1);

   // send the output or status code on error
   if (strncmp(output, "Error", 5) == 0) {
      stream_writeln(state->sock, "500 Exited with error message: %s", output);

   } else if (exit_code != 0) {
      stream_writeln(state->sock, "500 Error: %s (%d)", output, exit_code);

   } else {
      if (encoded_output == decoded_arg)
         bzero(encoded_output, BUF_SZ);

      if (strlen(output))
         to64frombits((unsigned char *)encoded_output, (const unsigned char *)output, strlen(output));

      stream_writeln(state->sock, "200 %s", encoded_output);
   }

   if (encoded_output != decoded_arg)
      free(encoded_output);
   free(decoded_arg);
   free(output);

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
