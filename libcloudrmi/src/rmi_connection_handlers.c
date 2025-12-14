#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>

#include "cloud-rmi.h"
#include "cloudrmi/rmi_payload.h"
#include "cloudrmi/rmi_error.h"
#include "cloudrmi/rmi_server.h"
#include "config.h"

static int rmi_shell_call_handler(
    const char *iface,
    const char *method,
    const rmi_payload_t *args,
    rmi_payload_t *result,
    rmi_error_t *error,
    void *userdata)
{
   struct server_iface_entry *iface_entry;
   int exit_code;
   FILE *pipe;
   char *payload = malloc(sizeof(char) * (BUF_SZ + 1));
   char *output = malloc(sizeof(char) * (BUF_SZ + 1));
   char shell_cmd[(BUF_SZ + 1)];

   if (payload == NULL || output == NULL) {
      if (payload) free(payload);
      if (output) free(output);
      return 0;
   }

   bzero(payload, BUF_SZ);
   bzero(output, BUF_SZ);
   bzero(shell_cmd, BUF_SZ);

   iface_entry = server_get_iface_entry((char *)iface);
   if (iface_entry == NULL) {
      if (error) {
         error->code = 400;
         snprintf(error->message, sizeof(error->message), "Unsupported interface: %s", iface);
      }
      free(payload);
      free(output);
      return 0;
   }

   if (!from64tobits(payload, (const char *)args->data)) {
      if (error) {
         error->code = 500;
         snprintf(error->message, sizeof(error->message), "Error decoding call arguments");
      }
      free(payload);
      free(output);
      return 0;
   }

   snprintf(shell_cmd, BUF_SZ, "%s %s %s 2>&1", iface_entry->file, method, payload);
   pipe = popen(shell_cmd, "r");
   stream_read(fileno(pipe), output, BUF_SZ);
   exit_code = pclose(pipe);

   if (strncmp(output, "Error", 5) == 0) {
      if (error) {
         error->code = 500;
         snprintf(error->message, sizeof(error->message), "Exited with error message: %s", output);
      }

   } else if (exit_code != 0) {
      if (error) {
         error->code = 500;
         snprintf(error->message, sizeof(error->message), "Error: %s (%d)", output, exit_code);
      }

   } else {
      bzero(payload, BUF_SZ);

      if (strlen(output))
         to64frombits((unsigned char *)payload, (unsigned char *)output, strlen(output));

      rmi_payload_free(result);
      result->data = strdup(payload);
      result->size = strlen(payload);
   }

   free(payload);
   free(output);

   return (error && error->code) ? 0 : 1;
}

int rmi_handle_call_cmd(rmi_server_t *server, char *iface, char *method, char *arg, rmi_connection_t *state)
{
   rmi_payload_t args;
   rmi_payload_t result;
   rmi_error_t error;
   int rc;

   if (!state->is_authenticated) {
      stream_writeln(state->sock, "401 Unauthorized");
      return 0;
   }

   rmi_payload_init(&args, RMI_PAYLOAD_PLAIN);
   rmi_payload_init(&result, RMI_PAYLOAD_PLAIN);
   rmi_error_clear(&error);

   args.data = strdup(arg);
   args.size = strlen(arg);

   if (server->call_handler == NULL)
      rmi_server_set_call_handler(server, rmi_shell_call_handler, NULL);

   rc = server->call_handler(iface, method, &args, &result, &error, server->call_userdata);

   if (rc) {
      if (result.data != NULL)
         stream_writeln(state->sock, "200 %s", (char *)result.data);
      else
         stream_writeln(state->sock, "200");
   } else {
      if (error.code)
         stream_writeln(state->sock, "%d %s", error.code, error.message);
      else
         stream_writeln(state->sock, "500 Error processing call");
   }

   rmi_payload_free(&args);
   rmi_payload_free(&result);

   return rc;
}

int rmi_shell_call_handler_register(rmi_server_t *server)
{
   if (server == NULL)
      return 0;

   rmi_server_set_call_handler(server, rmi_shell_call_handler, NULL);
   return 1;
}
