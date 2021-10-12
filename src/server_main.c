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


void show_help(char *this_cmd)
{
	printf("ProfiWeb Cloud SimpleRMI Server (version %s)\n", APP_VERSION);
#ifdef ENABLE_LINUX_FEATURES
   printf(" - compiled with Linux glibc features \n");
   #ifdef REUSE_WORKER_PROC
   printf(" - compiled with support for prefork frozen worker proccess manager (%i-%i spare workers)\n", MIN_SPARE_WORKERS, MAX_SPARE_WORKERS);
   #else
   printf(" - compiled without support for forked proccess manager\n");
   #endif
#else
   printf(" - compiled with POSIX libc features\n");
#endif
#ifdef PROXY_SUPPORT
	printf(" - compiled with Proxy support (protocol %s)\n", PROXY_PROTOCOL_VERSION);
#endif
	printf("\nUsage: \n");
	printf("\t%s < flags | options >\n", basename(this_cmd));
	printf("\t%s --help | -h\n", basename(this_cmd));
	printf("\nFlags:\n");
	printf("\t-d goes daemon\n");
	printf("\t-v enters verbose (debug) mode (currently %s)\n", var_debug ? "ON" : "OFF"); 
	printf("\nOptions:\n");
	printf("\t-b sets base namespace for local scripts\n");
	printf("\t-c sets path to the configuration file\n");
   printf("\t-D delay start for number of seconds (currently %i)\n", var_start_delay);
	printf("\t-e sets directory for executing local scripts\n");
	printf("\t-m sets path to the interface map file\n");
	printf("\t-n sets context name (currently to %s)\n", var_context_name);
	printf("\t-p sets port number (currently to %d)\n", var_port);
	printf("\t-t sets recovery authentication token\n");
#ifdef PROXY_SUPPORT
	printf("\t-x enables proxy to given host (currently %s)\n", 
		var_proxy ? var_proxy : "OFF"); 
#endif
	printf("\n");
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

   if (var_debug) {  // args are not parsed yet, this is for testing - can be set inside config
      if (success) {
         printf("Loaded configuration file %s\n", var_config_file);
         printf("Loaded interface map file %s\n", var_interface_map_file);
      } else
         printf("Failed to load some configuration files\n");
   } 
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

   // Try few common paths for configs and save the path
   find_and_load_configs(basename(argv[0]));

   // Quick help command
   if (argc == 2 && strcmp(argv[1], "--help") == 0) {
      show_help(argv[0]);
      exit(0);
   }

   if (argc > 1) { 
   // Overwrite some config variables
      while((opt = getopt(argc, argv, "b:c:dD:hn:p:t:vx:")) != -1) {
         switch(opt) {
         	case 'b':
               var_script_base_namespace = strdup(optarg);
               read_iface_config(var_interface_map_file);
               break;

            case 'c':
               var_config_file = strdup(optarg);
               read_config(var_config_file);
               break;

            case 'd':
               daemon = 1;
               break;

            case 'D':
               var_start_delay = atoi(optarg);
               break;

            case 'h':
	            show_help(argv[0]);
	            exit(0);

	        case 'm':
               var_interface_map_file = strdup(optarg);
               read_iface_config(var_interface_map_file);
               break;

            case 'n':
               var_context_name = strdup(optarg);
               break;

            case 'p':
               var_port = atoi(optarg);
               break;

            case 't':
               var_recovery_token = strdup(optarg);
               break;

            case 'v':
               printf("Entering verbose mode\n");
               var_debug = 1;
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

   // Let's sleep a bit if we want to
   if (var_start_delay) {
      if (var_debug)
         printf("Delayed start set, starting in %i seconds ...\n", var_start_delay);

      fflush(stdout);   // make sure output is shown before sleep
      sleep(var_start_delay);
   }

	// Start the server, possibly in a background
	if (daemon) {
		printf("Starting %s daemon on port %d\n", APP_NAME, var_port);

		pid = fork();

		if (pid < 0)
			die_err("Error on fork");

		if (pid == 0) {   // We are in child daemonized process
#ifdef PROXY_SUPPORT
			if (var_proxy) {
				proxy_start(var_proxy);
			}
#endif
			server_start(var_port);
			exit(0);
		}

	} else {
		printf("Starting %s on port %d\n", APP_NAME, var_port);
#ifdef PROXY_SUPPORT
		if (var_proxy) {
				proxy_start(var_proxy);
		}
#endif
		server_start(var_port);
	}

	//waitpid(-1, chldsig, WNOHANG); done in server loop
	exit(0);
}