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
#include <poll.h>
#include <limits.h>
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

static int append_output(char **buf, size_t *cap, size_t *len, const char *chunk, size_t chunk_len)
{
   size_t needed;

   if (*buf == NULL || cap == NULL || len == NULL)
      return 0;

   if (__builtin_add_overflow(*len, chunk_len, &needed) || __builtin_add_overflow(needed, 1, &needed))
      return 0;

   if (needed > *cap) {
      size_t new_cap = *cap;

      while (new_cap < needed) {
         if (new_cap > SIZE_MAX / 2)
            return 0;
         new_cap *= 2;
      }

      char *tmp = realloc(*buf, new_cap);
      if (tmp == NULL)
         return 0;

      *buf = tmp;
      *cap = new_cap;
   }

   memcpy((*buf) + *len, chunk, chunk_len);
   *len += chunk_len;
   (*buf)[*len] = '\0';

   return 1;
}

static int base64_encoded_length(size_t input_len, size_t *out_len)
{
   size_t rounded = 0;

   if (out_len == NULL)
      return 0;

   if (input_len == 0) {
      *out_len = BUF_SZ + 1;
      return 1;
   }

   if (__builtin_add_overflow(input_len, (size_t)2, &rounded))
      return 0;

   rounded /= 3;

   if (__builtin_mul_overflow(rounded, (size_t)4, &rounded))
      return 0;

   if (__builtin_add_overflow(rounded, (size_t)1, out_len))
      return 0;

   return 1;
}

static void trim_trailing_newlines(char *buf, size_t *len)
{
   if (buf == NULL || len == NULL)
      return;

   while (*len > 0 && (buf[*len - 1] == '\n' || buf[*len - 1] == '\r')) {
      buf[*len - 1] = '\0';
      (*len)--;
   }
}

static void free_arg_list(char **args, size_t count)
{
   if (args == NULL)
      return;

   for (size_t i = 0; i < count; i++) {
      free(args[i]);
   }

   free(args);
}

static int append_arg(char ***args, size_t *count, size_t *cap, const char *buf, size_t len)
{
   if (args == NULL || count == NULL || cap == NULL || buf == NULL)
      return 0;

   if (*count + 1 >= *cap) {
      size_t new_cap = (*cap == 0) ? 4 : (*cap * 2);

      char **tmp = realloc(*args, new_cap * sizeof(char *));
      if (tmp == NULL)
         return 0;

      *args = tmp;
      *cap = new_cap;
   }

   char *copy = malloc(len + 1);
   if (copy == NULL)
      return 0;

   if (len > 0 && buf != NULL)
      memcpy(copy, buf, len);
   copy[len] = '\0';

   (*args)[*count] = copy;
   (*count)++;
   (*args)[*count] = NULL;

   return 1;
}

static int append_char_dyn(char **buf, size_t *len, size_t *cap, char ch)
{
   if (buf == NULL || len == NULL || cap == NULL)
      return 0;

   if (*len + 1 >= *cap) {
      size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);

      char *tmp = realloc(*buf, new_cap);
      if (tmp == NULL)
         return 0;

      *buf = tmp;
      *cap = new_cap;
   }

   (*buf)[*len] = ch;
   (*len)++;
   return 1;
}

static int split_shell_args(const char *decoded, char ***out_args, size_t *out_count)
{
   enum parse_state { NORMAL, SINGLE_QUOTE, DOUBLE_QUOTE };
   enum parse_state state = NORMAL;
   char **args = NULL;
   size_t args_count = 0;
   size_t args_cap = 0;
   char *token = NULL;
   size_t token_len = 0;
   size_t token_cap = 0;
   int token_open = 0;
   const char *p = decoded;

   if (out_args == NULL || out_count == NULL)
      return 0;

   *out_args = NULL;
   *out_count = 0;

   if (decoded == NULL)
      return 1;

   while (1) {
      char ch = *p;
      int end = (ch == '\0');

      if (state == NORMAL) {
         if (end || isspace((unsigned char)ch)) {
            if (token_open) {
               if (!append_arg(&args, &args_count, &args_cap, token ? token : "", token_len)) {
                  free(token);
                  free_arg_list(args, args_count);
                  return 0;
               }
               token_len = 0;
               token_open = 0;
            }

            if (end)
               break;

            p++;
            continue;
         }

         if (ch == '\'') {
            token_open = 1;
            state = SINGLE_QUOTE;
            p++;
            continue;
         } else if (ch == '\"') {
            token_open = 1;
            state = DOUBLE_QUOTE;
            p++;
            continue;
         } else if (ch == '\\') {
            p++;
            ch = *p;
            if (ch == '\0') {
               ch = '\\';
               p--;
            }
         }

      } else if (state == SINGLE_QUOTE) {
         if (ch == '\'') {
            state = NORMAL;
            p++;
            continue;
         }
      } else if (state == DOUBLE_QUOTE) {
         if (ch == '\"') {
            state = NORMAL;
            p++;
            continue;
         }

         if (ch == '\\' && (p[1] == '\"' || p[1] == '\\')) {
            p++;
            ch = *p;
         }
      }

      if (!append_char_dyn(&token, &token_len, &token_cap, ch)) {
         free(token);
         free_arg_list(args, args_count);
         return 0;
      }

      token_open = 1;
      p++;
   }

   if (state != NORMAL) {
      free(token);
      free_arg_list(args, args_count);
      return 0;
   }

   free(token);

   *out_args = args;
   *out_count = args_count;
   return 1;
}

static int stream_child_io(
   int stdout_fd,
   int stdin_fd,
   const uint8_t *payload,
   size_t payload_len,
   char **output,
   size_t *output_len)
{
   size_t out_cap = BUF_SZ + 1;
   size_t out_len = 0;
   size_t payload_written = 0;
   int stdout_open = 1;
   int stdin_open = (stdin_fd >= 0 && payload != NULL && payload_len > 0);

   *output = malloc(out_cap);
   if (*output == NULL)
      return 0;

   (*output)[0] = '\0';

   if (payload != NULL && payload_len == 0 && stdin_fd >= 0) {
      close(stdin_fd);
      stdin_fd = -1;
      stdin_open = 0;
   }

   while (stdout_open || (stdin_open && payload_written < payload_len)) {
      struct pollfd fds[2];
      nfds_t nfds = 0;

      if (stdout_open) {
         fds[nfds].fd = stdout_fd;
         fds[nfds].events = POLLIN;
         nfds++;
      }

      if (stdin_open && payload_written < payload_len) {
         fds[nfds].fd = stdin_fd;
         fds[nfds].events = POLLOUT;
         nfds++;
      }

      int pr;

      do {
         pr = poll(fds, nfds, -1);
      } while (pr < 0 && errno == EINTR);

      if (pr < 0) {
         free(*output);
         *output = NULL;
         return 0;
      }

      nfds_t idx = 0;

      if (stdout_open) {
         short revents = fds[idx].revents;
         idx++;

         if (revents & POLLIN) {
            char buf[BUF_SZ];
            ssize_t n;

            do {
               n = read(stdout_fd, buf, sizeof(buf));
            } while (n < 0 && errno == EINTR);

            if (n > 0) {
               if (!append_output(output, &out_cap, &out_len, buf, (size_t)n)) {
                  free(*output);
                  *output = NULL;
                  return 0;
               }
            } else if (n == 0) {
               stdout_open = 0;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
               free(*output);
               *output = NULL;
               return 0;
            }
         } else if (revents & (POLLHUP | POLLERR | POLLNVAL)) {
            stdout_open = 0;
         }
      }

      if (stdin_open && payload_written < payload_len) {
         short revents = fds[idx].revents;

         if (revents & POLLOUT) {
            ssize_t n;

            do {
               n = write(stdin_fd, payload + payload_written, payload_len - payload_written);
            } while (n < 0 && errno == EINTR);

            if (n > 0) {
               payload_written += (size_t)n;

               if (payload_written == payload_len) {
                  close(stdin_fd);
                  stdin_open = 0;
               }
            } else if (n == 0) {
               continue;
            } else if (errno == EPIPE || errno == ECONNRESET) {
               close(stdin_fd);
               stdin_open = 0;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
               free(*output);
               *output = NULL;
               return 0;
            }
         } else if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
            close(stdin_fd);
            stdin_open = 0;
         }
      }
   }

   *output_len = out_len;
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
   int exit_code;
   int status;
   pid_t pid;
   int stdout_pipe[2];
   int stdin_pipe[2] = { -1, -1 };
   char *decoded_arg = NULL;
   char *output = NULL;
   size_t output_len = 0;
   char *response_output = NULL;
   int success = 0;
   char **argv = NULL;
   char **inline_args = NULL;
   size_t inline_argc = 0;

   if (payload == NULL && payload_len > 0) {
      stream_writeln(state->sock, "500 Error preparing payload buffer");
      return 0;
   }

   if (payload == NULL) {
      decoded_arg = malloc(sizeof(char) * (BUF_SZ + 1));
      if (decoded_arg == NULL) {
         stream_writeln(state->sock, "500 Error preparing call buffer");
         return 0;
      }

      bzero(decoded_arg, BUF_SZ);

      if (!from64tobits(decoded_arg, arg)) {
         stream_writeln(state->sock, "500 Error decoding call arguments");
         goto cleanup;
      }

      if (!split_shell_args(decoded_arg, &inline_args, &inline_argc)) {
         stream_writeln(state->sock, "500 Error parsing call arguments");
         goto cleanup;
      }

      argv = calloc(inline_argc + 3, sizeof(char *));
      if (argv == NULL) {
         stream_writeln(state->sock, "500 Error preparing argv buffer");
         goto cleanup;
      }

      argv[0] = file;
      argv[1] = method;

      for (size_t i = 0; i < inline_argc; i++) {
         argv[2 + i] = inline_args[i];
      }

      argv[inline_argc + 2] = NULL;
   } else {
      const char *encoding_arg = encoding != NULL ? encoding : "";

      argv = calloc(4, sizeof(char *));
      if (argv == NULL) {
         stream_writeln(state->sock, "500 Error preparing argv buffer");
         goto cleanup;
      }

      argv[0] = file;
      argv[1] = method;
      argv[2] = (char *)encoding_arg;
      argv[3] = NULL;
   }

   if (pipe(stdout_pipe) == -1) {
      stream_writeln(state->sock, "500 Error preparing call pipeline");
      goto cleanup;
   }

   if (payload != NULL && pipe(stdin_pipe) == -1) {
      stream_writeln(state->sock, "500 Error preparing payload pipeline");
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      goto cleanup;
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
      goto cleanup;
   }

   if (pid == 0) {
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
      }

      execv(file, argv);

      _exit(127);
   }

   close(stdout_pipe[1]);
   if (payload != NULL)
      close(stdin_pipe[0]);

   int stdin_fd = (payload != NULL) ? stdin_pipe[1] : -1;

   if (!stream_child_io(stdout_pipe[0], stdin_fd, payload, payload_len, &output, &output_len)) {
      stream_writeln(state->sock, "500 Error streaming payload");
      if (stdin_fd >= 0)
         close(stdin_fd);
      close(stdout_pipe[0]);
      waitpid(pid, NULL, 0);
      goto cleanup;
   }

   if (stdin_fd >= 0)
      close(stdin_fd);

   close(stdout_pipe[0]);

   while (waitpid(pid, &status, 0) == -1) {
      if (errno == EINTR)
         continue;
      stream_writeln(state->sock, "500 Error waiting for call");
      goto cleanup;
   }

   exit_code = (WIFEXITED(status) ? WEXITSTATUS(status) : -1);

   if (output == NULL)
      goto cleanup;

   trim_trailing_newlines(output, &output_len);

   if (strncmp(output, "Error", 5) == 0) {
      stream_writeln(state->sock, "500 Exited with error message: %s", output);

   } else if (exit_code != 0) {
      stream_writeln(state->sock, "500 Error: %s (%d)", output, exit_code);

   } else {
      if (payload != NULL && state->protocol_version >= 2) {
         const char *response_enc = (encoding != NULL && encoding[0] != '\0') ? encoding : "PLAIN";

         if ((size_t)output_len > var_max_payload) {
            stream_writeln(state->sock, "500 Error: payload exceeds max payload (%zu > %zu)", (size_t)output_len, var_max_payload);
            goto cleanup;
         }

         stream_writeln(state->sock, "200 PAYLOAD %s %zu", response_enc, output_len);

         if (output_len > 0) {
            ssize_t written = write(state->sock, output, output_len);
            (void)written;
         }

         success = 1;
         goto cleanup;
      }

      size_t encoded_len;

      if (!base64_encoded_length(output_len, &encoded_len)) {
         stream_writeln(state->sock, "500 Error preparing response buffer");
         goto cleanup;
      }

      response_output = malloc(encoded_len);
      if (response_output == NULL) {
         stream_writeln(state->sock, "500 Error preparing response buffer");
         goto cleanup;
      }

      response_output[0] = '\0';

      if (output_len)
         to64frombits((unsigned char *)response_output, (const unsigned char *)output, output_len);

      stream_writeln(state->sock, "200 %s", response_output);
   }

   success = 1;

cleanup:
   free(decoded_arg);
   free(output);
   free(response_output);
   free(argv);
   free_arg_list(inline_args, inline_argc);

   return success;
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
   const char *iface_name = iface;

   // Authentication check
   if (!state->is_authenticated) {
      stream_writeln(state->sock, "401 Unauthorized");     // todo: "... as: xyz"
      return 0;
   }

   if (iface_name != NULL && iface_name[0] == '\\')
      iface_name++;

   iface_entry = server_get_iface_entry((char *)iface_name);

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
      stream_writeln(state->sock, "400 Unsupported interface: %s", iface_name);
   }

   return 0;
}
