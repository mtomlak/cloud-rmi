#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <syslog.h>
#include <inttypes.h>
#include "config.h"
#include "cloud-rmi.h"

/**
 * Parses the configuration file. Some parts borrowed from sSMTP project.
 *
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @project cloud-rmi
 * @author Marian Tomlak
 * @license GNU/GPL
 */

/* Set default values for global vars */
int var_debug = 0;
int var_port = 9500;
int var_read_timeout = 10;
int var_start_delay = 0;
#ifdef REUSE_WORKER_PROC
int var_min_spare_workers = MIN_SPARE_WORKERS;
int var_max_spare_workers = MAX_SPARE_WORKERS;
#endif
char *var_recovery_token = NULL;
char *var_context_name = "tst.rmi";
char *var_script_exec_dir = "/usr/local/libexec/cloud-rmi";
char *var_script_base_namespace = "ProfiWeb\\Cloud\\Interfaces\\RMI";
char *var_config_file = CONFIGURATION_FILE;
char *var_unix_socket_path = NULL;
int var_unix_socket_skip_auth = 0;
char *var_interface_map_file = INTERFACE_MAP_FILE;
size_t var_max_payload = RMI_DEFAULT_MAX_PAYLOAD;

#ifdef PROXY_SUPPORT
char *var_proxy = NULL;
#endif


int read_config(char *config_file)
{
   int r = 1;
   FILE *fp;
   char *line = malloc(sizeof(char) * (BUF_SZ + 1));
   char *conf_name = malloc(sizeof(char) * (BUF_SZ + 1));
   char *conf_value = malloc(sizeof(char) * (BUF_SZ + 1));
   char *buf;

   bzero(line, BUF_SZ);
   bzero(conf_name, BUF_SZ);
   bzero(conf_value, BUF_SZ);

   if((fp = fopen(config_file, "r")) == NULL) {
      return 0;
   }

   r = fscanf(fp,"%[^\t\n#= ] = %[^\t\n=#]\n", conf_name, conf_value);

   while(r != EOF) {
   if (r == 2) {
         // the nasty part
         if(strcasecmp(conf_name, "context_name") == 0) {
            if((var_context_name = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            if(log_level > 0) {
               log_event(LOG_INFO, "Set context_name=\"%s\"\n", var_context_name);
            }
         }
         else if(strcasecmp(conf_name, "port") == 0) {
            if((buf = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            var_port = atoi(buf);

            if(log_level > 0) {
               log_event(LOG_INFO, "Set port=\"%d\"\n", var_port);
            }
         }
         else if(strcasecmp(conf_name, "read_timeout") == 0) {
            if((buf = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            var_read_timeout = atoi(buf);

            if(log_level > 0) {
               log_event(LOG_INFO, "Set read_timeout=\"%d\"\n", var_read_timeout);
            }
         }
         else if(strcasecmp(conf_name, "recovery_token") == 0) {
            if((var_recovery_token = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            if(log_level > 0) {
               log_event(LOG_INFO, "Set recovery_token=\"%s\"\n", var_recovery_token);
            }
         }
#ifdef PROXY_SUPPORT
         else if(strcasecmp(conf_name, "proxy") == 0) {
            if((var_proxy = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            if(log_level > 0) {
               log_event(LOG_INFO, "Set var_proxy=\"%s\"\n", var_proxy);
            }
         }
#endif
         else if(strcasecmp(conf_name, "script_exec_dir") == 0) {
            if((var_script_exec_dir = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            if(log_level > 0) {
               log_event(LOG_INFO, "Set var_script_exec_dir=\"%s\"\n", var_script_exec_dir);
            }
         } 
         else if(strcasecmp(conf_name, "script_base_namespace") == 0) {
            if((var_script_base_namespace = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            if(log_level > 0) {
               log_event(LOG_INFO, "Set script_base_namespace=\"%s\"\n", var_script_base_namespace);
            }
         } 
         else if(strcasecmp(conf_name, "listen_unix") == 0) {
            if((var_unix_socket_path = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            if(log_level > 0) {
               log_event(LOG_INFO, "Set listen_unix=\"%s\"\n", var_unix_socket_path);
            }
         }
         else if(strcasecmp(conf_name, "unix_socket_skip_auth") == 0) {
            if(strcasecmp(conf_value, "Yes") == 0 || strcasecmp(conf_value, "On") == 0 || strcasecmp(conf_value, "1") == 0) {
               var_unix_socket_skip_auth = 1;
            } else {
               var_unix_socket_skip_auth = 0;
            }

            if(log_level > 0) {
               log_event(LOG_INFO, "Set unix_socket_skip_auth=\"%s\"\n", var_unix_socket_skip_auth ? "True" : "False");
            }
         }
         else if(strcasecmp(conf_name, "interface_map_file") == 0) {
            if((var_interface_map_file = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            if(log_level > 0) {
               log_event(LOG_INFO, "Set interface_map_file=\"%s\"\n", var_interface_map_file);
            } //var_min_spare_workers
         }
         else if(strcasecmp(conf_name, "max_payload") == 0) {
            uintmax_t parsed = strtoumax(conf_value, NULL, 10);
            if(parsed > 0 && parsed < SIZE_MAX) {
               var_max_payload = (size_t)parsed;
               if(log_level > 0) {
                  log_event(LOG_INFO, "Set max_payload=\"%zu\"\n", var_max_payload);
               }
            }
         }
#ifdef REUSE_WORKER_PROC
         else if(strcasecmp(conf_name, "min_spare_workers") == 0) {
            if((buf = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            var_min_spare_workers = atoi(buf);

            if(log_level > 0) {
               log_event(LOG_INFO, "Set min_spare_workers=\"%d\"\n", var_min_spare_workers);
            }
         }
         else if(strcasecmp(conf_name, "max_spare_workers") == 0) {
            if((buf = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            var_max_spare_workers = atoi(buf);

            if(log_level > 0) {
               log_event(LOG_INFO, "Set max_spare_workers=\"%d\"\n", var_max_spare_workers);
            }
         }
#endif
         else if(strcasecmp(conf_name, "start_delay") == 0) {
            if((buf = strdup(conf_value)) == (char *)NULL) {
               die_err("parse_config() -- strdup() failed");
            }

            var_start_delay = atoi(buf);

            if(log_level > 0) {
               log_event(LOG_INFO, "Set start_delay=\"%d\"\n", var_start_delay);
            }
         }
         else if(strcasecmp(conf_name, "debug") == 0) {
            if(strcasecmp(conf_value, "Yes") == 0 || strcasecmp(conf_value, "On") == 0 || strcasecmp(conf_value, "1") == 0) {
               var_debug = 1;
               log_level = 1;
            }
            else {
               var_debug = 0;
               log_level = 0;
            }
 
            if(log_level > 0) {
               log_event(LOG_INFO,
                  "Set debug=\"%s\"\n",
                  var_debug ? "True" : "False");
            }
         }
         else {
            log_event(LOG_INFO, "Unable to set %s=\"%s\"\n", conf_name, conf_value);
         }

      } else
         fscanf(fp,"%*[^\n]\n"); // skip unparsable lines

      r = fscanf(fp,"%[^\t\n#= ] = %[^\t\n=#]\n", conf_name, conf_value);
   }

   fclose(fp);
   free(line);
   free(conf_name);
   free(conf_value);

   return 1;
}
