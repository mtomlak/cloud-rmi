#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <libgen.h>  // for basename()
#include <getopt.h>
#include "cloud-rmi.h"
#include "config.h"
#include "capabilities.h"
#ifdef PROXY_SUPPORT
   #include "proxy.h"
#endif

/**
 * C POSIX implementation of Cloud Simple Remote Method Invocation server.
 * This file declares functions commonly through the project.
 *
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license BSD
 */

#ifdef ENABLE_DEBUG_LOGGING
static void apply_early_debug_flags(int argc, char *argv[])
{
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
         debug_log_set(1);
      } else if (strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "--debug-traffic") == 0) {
         debug_traffic_set(1);
      }
   }
}
#endif


void show_help(char *this_cmd)
{
   printf("\nProfiWeb CloudRMI Server\n");
   printf("Enterprise Remote Invocation Service\n");

	printf("\nUsage: \n");
	printf("\t%s < flags | options >\n", basename(this_cmd));
	printf("\t%s --help | -h\n", basename(this_cmd));
	printf("\nFlags:\n");
	printf("\t-d daemonize process\n");
#ifdef ENABLE_DEBUG_LOGGING
   printf("\t-T enable traffic debug logging (default: %s)\n", debug_traffic_enabled() ? "ON" : "OFF");
	printf("\t-v enable verbose logging (default: %s)\n", debug_log_enabled() ? "ON" : "OFF"); 
#endif

	printf("\nOptions:\n");
	printf("\t-b set base namespace for local scripts\n");
	printf("\t-c set path to the configuration file\n");
   printf("\t-D delay start for number of seconds (default: %i)\n", var_start_delay);
	printf("\t-e set directory for executing local scripts\n");
   printf("\t-h print help and exit\n");
	printf("\t-m set path to the interface map file\n");
	printf("\t-n set context name (default: %s)\n", var_context_name);
	printf("\t-p set port number (default: %d)\n", var_port);
	printf("\t-u set unix socket path (default: %s)\n", var_unix_socket_path ? var_unix_socket_path : "disabled");
	printf("\t-t set authentication token\n");
	printf("\t-w set worker model: single, fork, prefork, or adaptive (default: %s)\n",
		worker_model_to_string(var_worker_model));
	printf("\t-W set number of prefork workers (default: %d)\n", var_prefork_workers);
	printf("\t-M set minimum adaptive workers (default: %d)\n", var_adaptive_min_workers);
	printf("\t-X set maximum adaptive workers (default: %d)\n", var_adaptive_max_workers);
	printf("\t-S set desired adaptive spare workers (default: %d)\n", var_adaptive_spare_workers);
#ifdef PROXY_SUPPORT
	printf("\t-x enable proxy to given host (currently %s)\n", 
		var_proxy ? var_proxy : "OFF"); 
#endif

   printf("\nVersion: \n");
   printf("\tapplication\t%s\n", APP_VERSION);
   printf("\tlibcloudrmi\t%s\n", LIBCLOUDRMI_VERSION);
   printf("\tprotocol\t%d\n", CLOUDRMI_PROTOCOL_VERSION);
#ifdef PROXY_SUPPORT
   printf("\tproxy protocol\t%s\n", PROXY_PROTOCOL_VERSION);
#endif   
   char caps_buf[BUF_SZ + 1];
   
   printf("\nBuild Options: \n");   
   rmi_capabilities_to_tokens(rmi_get_compiled_capabilities(), caps_buf, sizeof(caps_buf));
   printf("\tprotocol extensions\t%s\n", caps_buf);   
   printf("\tkernel interfaces\tPOSIX\n");
   printf("\tworker models\t\tsingle, fork, prefork, adaptive\n");
   printf("\tdefault model\t\tfork\n");
   printf("\tbuild symbols\t\t");
#ifdef PROXY_SUPPORT
   printf("PROXY_SUPPORT ");
#endif
#ifdef LEGACY_STREAM
   printf("LEGACY_STREAM ");
#endif
#ifdef ENABLE_DEBUG_LOGGING
   printf("ENABLE_DEBUG_LOGGING ");
#endif
   printf("\n"); //   end of build  

   printf("\n"); // end of help      
}

int find_and_load_configs(char *command)
{
   int success = 1;

#ifdef PROXY_SUPPORT 
   if (strcmp(command, "rmi-proxy") == 0 && read_config(PROXY_CONFIGURATION_FILE))
         var_config_file = strdup(PROXY_CONFIGURATION_FILE);
   else if (strcmp(command, "rmi-proxy") == 0 && read_config(PROXY_CONFIGURATION_FILE2))
         var_config_file = strdup(PROXY_CONFIGURATION_FILE2);
   else if (strcmp(command, "rmi-proxy") == 0 && read_config(PROXY_CONFIGURATION_FILE3))
         var_config_file = strdup(PROXY_CONFIGURATION_FILE3);
   else
#endif
   if (read_config(CONFIGURATION_FILE))
      var_config_file = strdup(CONFIGURATION_FILE);
   else if (read_config(CONFIGURATION_FILE2))
      var_config_file = strdup(CONFIGURATION_FILE2);
   else if (read_config(CONFIGURATION_FILE3))
      var_config_file = strdup(CONFIGURATION_FILE3);
   else
      success = 0;

   if (!read_iface_config(var_interface_map_file)) {    // this could be set in config file
      if (read_iface_config(INTERFACE_MAP_FILE))
         var_interface_map_file = strdup(INTERFACE_MAP_FILE);
      else if (read_iface_config(INTERFACE_MAP_FILE2))
         var_interface_map_file = strdup(INTERFACE_MAP_FILE2);
      else if (read_iface_config(INTERFACE_MAP_FILE3))
         var_interface_map_file = strdup(INTERFACE_MAP_FILE3);
      else
         success = 0;
   }

   if (success) {
      DEBUG_LOG("Loaded configuration file %s", var_config_file);
      DEBUG_LOG("Loaded interface map file %s", var_interface_map_file);
   } else {
      DEBUG_LOG("Failed to load some configuration files");
   }

   return success;
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
   int opt, pid, daemon = 0;

#ifdef ENABLE_DEBUG_LOGGING
   apply_early_debug_flags(argc, argv);
#endif

   // Try few common paths for configs and save the path
   find_and_load_configs(basename(argv[0]));

   // Quick help command
   if (argc == 2 && strcmp(argv[1], "--help") == 0) {
      show_help(argv[0]);
      exit(0);
   }

   if (argc > 1) { 
   // Overwrite some config variables
   static struct option long_options[] = {
       {"unix-socket",  required_argument, 0, 'u'},
       {"worker-model", required_argument, 0, 'w'},
       {"workers",      required_argument, 0, 'W'},
       {"adaptive-min", required_argument, 0, 'M'},
       {"adaptive-max", required_argument, 0, 'X'},
       {"adaptive-spare", required_argument, 0, 'S'},
       {"help",         no_argument,       0, 'h'},
#ifdef ENABLE_DEBUG_LOGGING
       {"debug-traffic", no_argument,      0, 'T'},
#endif
       {0, 0, 0, 0}
   };

   const char *optstring =
#ifdef ENABLE_DEBUG_LOGGING
       "b:c:dD:he:m:n:p:t:u:vTw:W:M:X:S:x:";
#else
       "b:c:dD:he:m:n:p:t:u:w:W:M:X:S:x:";
#endif

      while((opt = getopt_long(argc, argv, optstring, long_options, NULL)) != -1) {
         switch(opt) {
         	case 'b':
               var_script_base_namespace = strdup(optarg);
               normalize_script_base_namespace();
               read_iface_config(var_interface_map_file);
               DEBUG_LOG("arg:set script_base_namespace=%s", var_script_base_namespace);
               break;

            case 'c':
               var_config_file = strdup(optarg);
               if (!read_config(var_config_file))
                  die_errf("Error: Failed to load config file %s", var_config_file);
               DEBUG_LOG("arg:set config_file=%s", var_config_file);
               break;

            case 'd':
               daemon = 1;
               var_run_as_daemon = 1;
               DEBUG_LOG("arg:daemonize");
               break;

            case 'D':
               var_start_delay = atoi(optarg);
               DEBUG_LOG("arg:set start_delay=%d", var_start_delay);
               break;

            case 'h':
	            show_help(argv[0]);
	            exit(0);

            case 'm':
               var_interface_map_file = strdup(optarg);
               read_iface_config(var_interface_map_file);
               DEBUG_LOG("arg:set interface_map_file=%s", var_interface_map_file);
               break;

            case 'e':
               var_script_exec_dir = strdup(optarg);
               read_iface_config(var_interface_map_file);
               DEBUG_LOG("arg:set script_exec_dir=%s", var_script_exec_dir);
               break;

            case 'n':
               var_context_name = strdup(optarg);
               DEBUG_LOG("arg:set context_name=%s", var_context_name);
               break;

            case 'p':
               var_port = atoi(optarg);
               DEBUG_LOG("arg:set port=%d", var_port);
               break;

            case 'u':
               var_unix_socket_path = strdup(optarg);
               DEBUG_LOG("arg:set unix_socket_path=%s", var_unix_socket_path);
               break;

            case 't':
               var_recovery_token = strdup(optarg);
               DEBUG_LOG("arg:set recovery_token=%s", var_recovery_token);
               break;

#ifdef ENABLE_DEBUG_LOGGING
            case 'v':
               debug_log_set(1);
               DEBUG_LOG("arg:verbose");
               break;

            case 'T':
               debug_traffic_set(1);
               DEBUG_LOG("arg:debug_traffic");
               break;
#endif

            case 'w':
               if (strcasecmp(optarg, "single") == 0 || strcasecmp(optarg, "fork") == 0)
                  var_worker_model = WORKER_MODEL_FORK;
               else if (strcasecmp(optarg, "prefork") == 0)
                  var_worker_model = WORKER_MODEL_PREFORK;
               else if (strcasecmp(optarg, "adaptive") == 0)
                  var_worker_model = WORKER_MODEL_ADAPTIVE;
               else
                  die_errf("Error: Unknown worker model %s", optarg);

               DEBUG_LOG("arg:set worker_model=%s", optarg);
               break;

            case 'W':
                  int workers = atoi(optarg);
                  if (workers < PREFORK_WORKERS_MIN) {
                     workers = PREFORK_WORKERS_MIN;
                  } else if (workers > PREFORK_WORKERS_MAX) {
                     workers = PREFORK_WORKERS_MAX;
                  }
                  var_prefork_workers = workers;
                  DEBUG_LOG("arg:set prefork_workers=%d", workers);
               break;

            case 'M': {
                  int workers = atoi(optarg);
                  if (workers < ADAPTIVE_WORKERS_MIN) {
                     workers = ADAPTIVE_WORKERS_MIN;
                  } else if (workers > ADAPTIVE_WORKERS_MAX) {
                     workers = ADAPTIVE_WORKERS_MAX;
                  }
                  var_adaptive_min_workers = workers;
                  DEBUG_LOG("arg:set adaptive_min_workers=%d", workers);
               }
               break;

            case 'X': {
                  int workers = atoi(optarg);
                  if (workers < ADAPTIVE_WORKERS_MIN) {
                     workers = ADAPTIVE_WORKERS_MIN;
                  } else if (workers > ADAPTIVE_WORKERS_MAX) {
                     workers = ADAPTIVE_WORKERS_MAX;
                  }
                  var_adaptive_max_workers = workers;
                  DEBUG_LOG("arg:set adaptive_max_workers=%d", workers);
               }
               break;

            case 'S': {
                  int workers = atoi(optarg);
                  if (workers < 0) {
                     workers = 0;
                  } else if (workers > ADAPTIVE_WORKERS_MAX) {
                     workers = ADAPTIVE_WORKERS_MAX;
                  }
                  var_adaptive_spare_workers = workers;
                  DEBUG_LOG("arg:set adaptive_spare_workers=%d", workers);
               }
               break;

            case 'x':
#ifdef PROXY_SUPPORT
            	var_proxy = strdup(optarg);
#else
            	die_err("Error: This program is built without proxy support!");
#endif
            	break;

            default:
               break;
         }
      }
   }

   DEBUG_LOG("Arguments parsed; Configuration summary: unix_socket_skip_auth=%i, script_exec_dir=%s", var_unix_socket_skip_auth, var_script_exec_dir);

   if (!read_iface_config(var_interface_map_file))
      die_err("Error: Failed to load interface map file");
   normalize_worker_config();

   // Let's sleep a bit if we want to
   if (var_start_delay) {
      DEBUG_LOG("Delayed start set, starting in %i seconds ...", var_start_delay);
      fflush(stdout);   // make sure output is shown before sleep
      sleep(var_start_delay);
   }

  const char *mode = daemon ? "daemon" : "foreground";

   if (var_unix_socket_path)
       printf("Starting %s server (%s) on unix socket %s\n",
              APP_NAME, mode, var_unix_socket_path);
   else
       printf("Starting %s server (%s) on port %d\n",
              APP_NAME, mode, var_port);

	// Start the server, possibly in a background
   if (daemon) {
       pid = fork();
       if (pid < 0)
           die_err("Error: fork() failed");

       if (pid > 0)
           exit(0);
   }

   // Child - daemon or foreground continues here
   #ifdef PROXY_SUPPORT
   if (var_proxy)
       proxy_start(var_proxy);
   #endif

   server_start(var_port, var_unix_socket_path);   // TODO: also in pproxy mode?
   exit(0);
}
