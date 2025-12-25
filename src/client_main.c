#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <libgen.h>  // for basename()
#include "cloud-rmi.h"
#include "config.h"
#include "client.h"

/**
 * C POSIX implementation of Cloud Simple Remote Method Invocation server.
 * This file declares functions commonly through the project.
 *
 * @copyright Copyright (C) 2016-2025 ProfiWeb, s.r.o. (https://profiweb.biz/).
 * @author Marian Tomlak
 * @license BSD
 */

//int var_port;
char *var_server_name = "127.0.0.1";   // = malloc(sizeof(char) * (BUF_SZ + 1));

/**
 * Shows help message
 * 
 * @param this_cmd Name of current command (argv[0])
 */
void show_help(char *this_cmd)
{
   printf("\nProfiWeb CloudRMI Client\n");
   printf("Enterprise Remote Invocation Service\n");

   printf("\nUsage: \n");
   printf("\t%s < options > [ interface ] [ method ] < arguments >\n", basename(this_cmd));
   printf("\t%s < options > -r [ command ]\n", basename(this_cmd));
   printf("\t%s --help\n", basename(this_cmd));
   printf("\nOptions:\n");
   printf("\t-h print help and exit\n");
   printf("\t-p set port number (default: %d)\n", var_port);
   printf("\t-r execute raw RMI command\n");
   printf("\t-s set server name (default: %s)\n", var_server_name);
   printf("\t-t set authentication token\n");
   printf("\t-u set unix socket path (default: %s)\n", var_unix_socket_path ? var_unix_socket_path : "disabled");
#ifdef ENABLE_DEBUG_LOGGING
   printf("\t-v enable verbose logging (default: %s)\n", 
      debug_log_enabled() ? "ON" : "OFF");
#endif

   printf("\nVersion: \n");
   printf("\tapplication\t%s\n", APP_VERSION);
   printf("\tlibcloudrmi\t%s\n", LIBCLOUDRMI_VERSION);
   printf("\tprotocol\t%d\n", CLOUDRMI_PROTOCOL_VERSION);   
   printf("\n"); // end of help    
}

void print_php_error(char *response)
{
   char *decoded = malloc(sizeof(char) * (BUF_SZ + 1));
   char *message = malloc(sizeof(char) * (BUF_SZ + 1));

   bzero(decoded, BUF_SZ);
   bzero(message, BUF_SZ);

   // Try to decode stuff
   decode_payload(response, decoded);
   printf("Error: Remote PHP error: %s\n", decoded);

   free(decoded);
   free(message);
}

/**
 * Main program handler
 * 
 * @param  int argc
 * @param  char* argv
 * @return int
 */
int main(int argc, char *argv[])
{
   int status_code, i, opt = 0;
   char *iface, *method, *args, *raw_cmd = NULL;
   char *result = malloc(sizeof(char) * (BUF_SZ + 1));
   struct rmi_client_conn conn;

   bzero(result, BUF_SZ);

   // Load default config
   read_config(CONFIGURATION_FILE);

   // Quick help command
   if (argc == 2 && strcmp(argv[1], "--help") == 0) {
      show_help(argv[0]);
      exit(0);
   }

#ifdef ENABLE_DEBUG_LOGGING
   const char *optstring = "c:hp:r:s:t:u:v";
#else
   const char *optstring = "c:hp:r:s:t:u:";
#endif

   // Overwrite some config variables
   while((opt = getopt(argc, argv, optstring)) != -1) {
      switch(opt) {
         case 'c':
            read_config(optarg);
            break;

         case 'h':
            show_help(argv[0]);
            exit(0);

         case 'p':
            var_port = atoi(optarg);
            break;

         case 'r':
            raw_cmd = strdup(optarg);
            break;

         case 's':
            var_server_name = strdup(optarg);
            break;

         case 'u':
            var_unix_socket_path = strdup(optarg);
            break;

         case 't':
            var_recovery_token = strdup(optarg);
            break;

#ifdef ENABLE_DEBUG_LOGGING
         case 'v':
            printf("Entering verbose mode\n");
            debug_log_set(1);
            break;
#endif

         default:
            break;
      }
   }

   // We need to find last 2 or 3 non-option arguments
   if (argc - optind > 1) {
      // Complete relative interface name
      if (argv[optind][0] != '\\') {
         size_t iface_len = strlen(var_script_base_namespace) + strlen(argv[optind]) + 2;
         iface = malloc(sizeof(char) * iface_len);

         if (iface == NULL)
            die_err("Error: Failed to allocate interface name buffer");

         snprintf(iface, iface_len, "%s\\%s", var_script_base_namespace, argv[optind]);

      } else
         iface = strdup(argv[optind]);

      method = strdup(argv[optind + 1]);
      size_t args_len = 0;
      int trailing_args = argc - (optind + 2);

      for (i = optind + 2; i < argc; i++) {
         args_len += strlen(argv[i]);
      }

      if (trailing_args > 1)
         args_len += (size_t)(trailing_args - 1); // spaces between args

      args = malloc(args_len + 1);

      if (args == NULL)
         die_err("Error: Failed to allocate argument buffer");

      size_t pos = 0;

      for (i = optind + 2; i < argc; i++) {
         size_t len = strlen(argv[i]);

         memcpy(args + pos, argv[i], len);
         pos += len;

         if (i + 1 < argc)
            args[pos++] = ' ';
      }

      args[pos] = '\0';

   } else if (raw_cmd == NULL) {
      show_help(argv[0]);
      exit(0);  
   }
   
   // Connect to the server
   if (client_connect(var_server_name, var_port, var_unix_socket_path, &conn) < 0)
      die_err("Error: Failed to connect to the server");

   // Authenticate
   if (var_recovery_token != NULL)
      if (!client_auth_by_token(&conn, var_recovery_token))
         die_err("Error: Token authentication failed");

   // Executing raw RMI protocol command
   if (raw_cmd != NULL) {
      status_code = client_raw_cmd(&conn, raw_cmd, result);

      printf("%i %s\n", status_code, result);
      client_disconnect(&conn);
      free(result);
      exit(0);
   }

   // Executing standard RMI call
   if (client_call_remote_method(&conn, iface, method, args, result)) {
      printf("%s", result);
      client_disconnect(&conn);
      free(result);
      exit(0);

    } else {   // Remote call failed
      if (strlen(result) > 3) {
         // Try to show plaintext errors and sort-of decode php errors as well
         if (strstr(result, "PHP:") == result) {
            print_php_error(result);

         } else {
            printf("%s%s\n", (strstr(result, "Error: ") == NULL) ? "Error: " : "", result);
         }
      }
      else
         printf("Error: Failed to perform remote call: %s\n", result);

      client_disconnect(&conn);
      free(result);
      exit(1);
   }
}
