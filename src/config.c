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

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define CONFIG_SCAN_FMT "%" STR(BUF_SZ) "[^\\t\\n#= ] = %" STR(BUF_SZ) "[^\\t\\n=#]\n"

/**
 * Parses the configuration file. Some parts borrowed from sSMTP project.
 *
 * @copyright Copyright (C) 2016-2025 ProfiWeb, s.r.o. (https://profiweb.biz/).
 * @project cloud-rmi
 * @author Marian Tomlak
 * @license GNU/GPL
 */

/* Set default values for global vars */
#ifdef ENABLE_DEBUG_LOGGING
int var_debug = 0;
int var_debug_traffic = 0;
#endif
int var_port = 9500;
int var_read_timeout = 10;
int var_start_delay = 0;
char *var_recovery_token = NULL;
char *var_context_name = "tst.rmi";
char *var_script_exec_dir = "/usr/local/libexec/cloud-rmi";
char *var_script_base_namespace = "ProfiWeb\\Cloud\\Interfaces\\RMI";
char *var_config_file = CONFIGURATION_FILE;
char *var_unix_socket_path = NULL;
int var_unix_socket_skip_auth = 0;
char *var_interface_map_file = INTERFACE_MAP_FILE;
size_t var_max_payload = RMI_DEFAULT_MAX_PAYLOAD;
worker_model_t var_worker_model = WORKER_MODEL_FORK;
int var_prefork_workers = PREFORK_WORKERS_DEFAULT;

static void sanitize_script_base_namespace()
{
   if (var_script_base_namespace != NULL && var_script_base_namespace[0] == '\\') {
      size_t len = strlen(var_script_base_namespace);
      if (len > 0)
         memmove(var_script_base_namespace, var_script_base_namespace + 1, len);
   }
}

#ifdef PROXY_SUPPORT
char *var_proxy = NULL;
#endif


int read_config(char *config_file)
{
   FILE *fp;
   char conf_name[BUF_SZ + 1];
   char conf_value[BUF_SZ + 1];
   char line[BUF_SZ + 1];
   char *name;
   char *value;

   bzero(conf_name, sizeof(conf_name));
   bzero(conf_value, sizeof(conf_value));
   bzero(line, sizeof(line));

   if((fp = fopen(config_file, "r")) == NULL) {
      return 0;
   }

   while (fgets(line, sizeof(line), fp) != NULL) {
      if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
         continue;

      name = line;
      value = strchr(line, '=');
      if (value == NULL) {
         log_unexpected_event(LOG_WARNING, "Skipping unparsable config line in %s", config_file);
         continue;
      }

      *value = '\0';
      value++;

      while (isspace((unsigned char)*name))
         name++;
      while (*value != '\0' && isspace((unsigned char)*value))
         value++;

      char *name_end = name + strlen(name);
      while (name_end > name && isspace((unsigned char)name_end[-1])) {
         *--name_end = '\0';
      }

      char *value_end = value + strlen(value);
      while (value_end > value && (isspace((unsigned char)value_end[-1]) || value_end[-1] == '#')) {
         *--value_end = '\0';
      }

      if (*name == '\0' || *value == '\0')
         continue;

      strncpy(conf_name, name, sizeof(conf_name) - 1);
      strncpy(conf_value, value, sizeof(conf_value) - 1);

      // the nasty part
      if(strcasecmp(conf_name, "context_name") == 0) {
         if((var_context_name = strdup(conf_value)) == (char *)NULL) {
            die_err("parse_config() -- strdup() failed");
         }

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set context_name=\"%s\"", var_context_name);
         }
      }
      else if(strcasecmp(conf_name, "port") == 0) {
         var_port = atoi(conf_value);

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set port=\"%d\"", var_port);
         }
      }
      else if(strcasecmp(conf_name, "read_timeout") == 0) {
         var_read_timeout = atoi(conf_value);

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set read_timeout=\"%d\"", var_read_timeout);
         }
      }
      else if(strcasecmp(conf_name, "recovery_token") == 0) {
         if((var_recovery_token = strdup(conf_value)) == (char *)NULL) {
            die_err("parse_config() -- strdup() failed");
         }

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set recovery_token=\"%s\"", var_recovery_token);
         }
      }
#ifdef PROXY_SUPPORT
      else if(strcasecmp(conf_name, "proxy") == 0) {
         if((var_proxy = strdup(conf_value)) == (char *)NULL) {
            die_err("parse_config() -- strdup() failed");
         }

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set var_proxy=\"%s\"", var_proxy);
         }
      }
#endif
      else if(strcasecmp(conf_name, "script_exec_dir") == 0) {
         if((var_script_exec_dir = strdup(conf_value)) == (char *)NULL) {
            die_err("parse_config() -- strdup() failed");
         }

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set var_script_exec_dir=\"%s\"", var_script_exec_dir);
         }
      } 
      else if(strcasecmp(conf_name, "script_base_namespace") == 0) {
         if((var_script_base_namespace = strdup(conf_value)) == (char *)NULL) {
            die_err("parse_config() -- strdup() failed");
         }

         sanitize_script_base_namespace();

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set script_base_namespace=\"%s\"", var_script_base_namespace);
         }
      } 
      else if(strcasecmp(conf_name, "listen_unix") == 0) {
         if((var_unix_socket_path = strdup(conf_value)) == (char *)NULL) {
            die_err("parse_config() -- strdup() failed");
         }

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set listen_unix=\"%s\"", var_unix_socket_path);
         }
      }
      else if(strcasecmp(conf_name, "unix_socket_skip_auth") == 0) {
         if(strcasecmp(conf_value, "Yes") == 0 || strcasecmp(conf_value, "On") == 0 || strcasecmp(conf_value, "1") == 0) {
            var_unix_socket_skip_auth = 1;
         } else {
            var_unix_socket_skip_auth = 0;
         }

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set unix_socket_skip_auth=\"%s\"", var_unix_socket_skip_auth ? "True" : "False");
         }
      }
      else if(strcasecmp(conf_name, "interface_map_file") == 0) {
         if((var_interface_map_file = strdup(conf_value)) == (char *)NULL) {
            die_err("parse_config() -- strdup() failed");
         }

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set interface_map_file=\"%s\"", var_interface_map_file);
         } //var_min_spare_workers
      }
      else if(strcasecmp(conf_name, "max_payload") == 0) {
         uintmax_t parsed = strtoumax(conf_value, NULL, 10);
         if(parsed > 0 && parsed < SIZE_MAX) {
            var_max_payload = (size_t)parsed;
            if(log_level > 0) {
               log_event(LOG_INFO, "config:set max_payload=\"%zu\"", var_max_payload);
            }
         }
      }
      else if(strcasecmp(conf_name, "start_delay") == 0) {
         var_start_delay = atoi(conf_value);

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set start_delay=\"%d\"", var_start_delay);
         }
      }
      else if(strcasecmp(conf_name, "worker_model") == 0) {
         if(strcasecmp(conf_value, "prefork") == 0) {
            var_worker_model = WORKER_MODEL_PREFORK;
         } else {
            var_worker_model = WORKER_MODEL_FORK;
         }

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set worker_model=\"%s\"",
               var_worker_model == WORKER_MODEL_PREFORK ? "prefork" : "fork");
         }
      }
      else if(strcasecmp(conf_name, "prefork_workers") == 0) {
         int workers = atoi(conf_value);
         if(workers < PREFORK_WORKERS_MIN) {
            workers = PREFORK_WORKERS_MIN;
         } else if(workers > PREFORK_WORKERS_MAX) {
            workers = PREFORK_WORKERS_MAX;
         }
         var_prefork_workers = workers;

         if(log_level > 0) {
            log_event(LOG_INFO, "config:set prefork_workers=\"%d\"", var_prefork_workers);
         }
      }
#ifdef ENABLE_DEBUG_LOGGING
      else if(strcasecmp(conf_name, "debug") == 0) {
         if(strcasecmp(conf_value, "Yes") == 0 || strcasecmp(conf_value, "On") == 0 || strcasecmp(conf_value, "1") == 0) {
            debug_log_set(1);
         }
         else {
            debug_log_set(0);
         }

         if(log_level > 0) {
            log_event(LOG_INFO,
               "config:set debug=\"%s\"",
               debug_log_enabled() ? "True" : "False");
         }
      }
      else if(strcasecmp(conf_name, "debug_traffic") == 0) {
         if(strcasecmp(conf_value, "Yes") == 0 || strcasecmp(conf_value, "On") == 0 || strcasecmp(conf_value, "1") == 0) {
            debug_traffic_set(1);
         } else {
            debug_traffic_set(0);
         }

         if (log_level > 0) {
            log_event(
               LOG_INFO,
               "config:set debug_traffic=\"%s\"",
               debug_traffic_enabled() ? "True" : "False");
         }
      }
#endif
      else {
         log_event(LOG_INFO, "Unable to set %s=\"%s\"", conf_name, conf_value);
      }
   }

   fclose(fp);

   if (debug_log_enabled())
      DEBUG_LOG("Configuration loaded (unix_socket_skip_auth=%i, script_exec_dir=%s)", var_unix_socket_skip_auth, var_script_exec_dir);

   return 1;
}

void normalize_script_base_namespace()
{
   sanitize_script_base_namespace();
}
