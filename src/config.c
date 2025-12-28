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
#include <errno.h>
#include <limits.h>
#include "config.h"
#include "cloud-rmi.h"

/**
 * Parses the configuration file. Some parts borrowed from sSMTP project.
 *
 * @copyright Copyright (C) 2016-2025 ProfiWeb, s.r.o.
 * @project cloud-rmi
 * @author Marian Tomlak
 * @license GNU/GPL
 */

//
// Config parse helpers
//

int config_parse_str(char **dst, const char *value, int allow_empty)
{
   char *copy;

   if (dst == NULL || value == NULL)
      return 0;

   if (!allow_empty && value[0] == '\0')
      return 0;

   copy = strdup(value);
   if (copy == NULL)
      return -1;

   if (*dst != NULL)
      free(*dst);

   *dst = copy;
   return 1;
}

int config_parse_int(int *dst, const char *value, int min, int max)
{
   char *end;
   long parsed;

   if (dst == NULL || value == NULL)
      return 0;

   errno = 0;
   parsed = strtol(value, &end, 10);

   if (errno == ERANGE || end == value || *end != '\0')
      return -1;

   if (parsed < min)
      parsed = min;
   else if (parsed > max)
      parsed = max;

   *dst = (int)parsed;
   return 1;
}

int config_parse_bool(int *dst, const char *value)
{
   if (dst == NULL || value == NULL)
      return 0;

   if (!strcasecmp(value, "yes") ||
       !strcasecmp(value, "on") ||
       !strcasecmp(value, "true") ||
       !strcmp(value, "1")) {
      *dst = 1;
      return 1;
   }

   if (!strcasecmp(value, "no") ||
       !strcasecmp(value, "off") ||
       !strcasecmp(value, "false") ||
       !strcmp(value, "0")) {
      *dst = 0;
      return 1;
   }

   return -1;
}

//
// Global config variables
//

#ifdef ENABLE_DEBUG_LOGGING
int var_debug = 0;
int var_debug_traffic = 0;
#endif

int var_port;
int var_read_timeout;
int var_start_delay;

char *var_recovery_token = NULL;
char *var_context_name = NULL;
char *var_script_exec_dir = NULL;
char *var_script_base_namespace = NULL;
char *var_config_file = CONFIGURATION_FILE;
char *var_unix_socket_path = NULL;
int var_unix_socket_skip_auth = 0;
char *var_interface_map_file = NULL;
size_t var_max_payload;

worker_model_t var_worker_model;
int var_prefork_workers;
int var_adaptive_min_workers;
int var_adaptive_max_workers;
int var_adaptive_spare_workers;

#ifdef PROXY_SUPPORT
char *var_proxy = NULL;
#endif

//
// Defaults
//

static void config_reset_defaults(void)
{
   var_port = DEFAULT_PORT;
   var_read_timeout = DEFAULT_READ_TIMEOUT;
   var_start_delay = DEFAULT_START_DELAY;

   free(var_context_name);
   free(var_script_exec_dir);
   free(var_script_base_namespace);

   var_context_name = strdup(DEFAULT_CONTEXT_NAME);
   var_script_exec_dir = strdup(DEFAULT_SCRIPT_EXEC_DIR);
   var_script_base_namespace = strdup(DEFAULT_SCRIPT_BASE_NS);

   var_unix_socket_path = NULL;
   var_unix_socket_skip_auth = 0;
   var_recovery_token = NULL;

   var_interface_map_file = INTERFACE_MAP_FILE;
   var_max_payload = RMI_DEFAULT_MAX_PAYLOAD;

   var_worker_model = DEFAULT_WORKER_MODEL;
   var_prefork_workers = DEFAULT_PREFORK_WORKERS;
   var_adaptive_min_workers = DEFAULT_ADAPTIVE_MIN_WORKERS;
   var_adaptive_max_workers = DEFAULT_ADAPTIVE_MAX_WORKERS;
   var_adaptive_spare_workers = DEFAULT_ADAPTIVE_SPARE_WORKERS;
}

static void sanitize_script_base_namespace(void)
{
   if (var_script_base_namespace != NULL &&
       var_script_base_namespace[0] == '\\') {
      size_t len = strlen(var_script_base_namespace);
      if (len > 0)
         memmove(var_script_base_namespace,
                 var_script_base_namespace + 1,
                 len);
   }
}

//
// Config reader
//

int read_config(char *config_file)
{
   FILE *fp;
   char line[BUF_SZ + 1];
   char conf_name[BUF_SZ + 1];
   char conf_value[BUF_SZ + 1];
   char *name;
   char *value;

   bzero(line, sizeof(line));
   bzero(conf_name, sizeof(conf_name));
   bzero(conf_value, sizeof(conf_value));

   config_reset_defaults();

   if ((fp = fopen(config_file, "r")) == NULL)
      return 0;

   while (fgets(line, sizeof(line), fp) != NULL) {
      if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
         continue;

      name = line;
      value = strchr(line, '=');
      if (value == NULL)
         continue;

      *value++ = '\0';

      while (isspace((unsigned char)*name))
         name++;
      while (isspace((unsigned char)*value))
         value++;

      char *ne = name + strlen(name);
      while (ne > name && isspace((unsigned char)ne[-1]))
         *--ne = '\0';

      char *ve = value + strlen(value);
      while (ve > value && (isspace((unsigned char)ve[-1]) || ve[-1] == '#'))
         *--ve = '\0';

      if (*name == '\0' || *value == '\0')
         continue;

      strncpy(conf_name, name, sizeof(conf_name) - 1);
      strncpy(conf_value, value, sizeof(conf_value) - 1);

      if (!strcasecmp(conf_name, "context_name")) {
         if (config_parse_str(&var_context_name, conf_value, 0) < 0) goto invalid;
         DEBUG_LOG("config:set context_name=\"%s\"", var_context_name);
      }
      else if (!strcasecmp(conf_name, "port")) {
         if (config_parse_int(&var_port, conf_value, 1, 65535) < 0) goto invalid;
         DEBUG_LOG("config:set port=%d", var_port);
      }
      else if (!strcasecmp(conf_name, "read_timeout")) {
         if (config_parse_int(&var_read_timeout, conf_value, 0, INT_MAX) < 0) goto invalid;
         DEBUG_LOG("config:set read_timeout=%d", var_read_timeout);
      }
      else if (!strcasecmp(conf_name, "recovery_token")) {
         if (config_parse_str(&var_recovery_token, conf_value, 0) < 0) goto invalid;
         DEBUG_LOG("config:set recovery_token=\"%s\"", var_recovery_token);
      }
#ifdef PROXY_SUPPORT
      else if (!strcasecmp(conf_name, "proxy")) {
         if (config_parse_str(&var_proxy, conf_value, 0) < 0) goto invalid;
         DEBUG_LOG("config:set proxy=\"%s\"", var_proxy);
      }
#endif
      else if (!strcasecmp(conf_name, "script_exec_dir")) {
         if (config_parse_str(&var_script_exec_dir, conf_value, 0) < 0) goto invalid;
         DEBUG_LOG("config:set script_exec_dir=\"%s\"", var_script_exec_dir);
      }
      else if (!strcasecmp(conf_name, "script_base_namespace")) {
         if (config_parse_str(&var_script_base_namespace, conf_value, 0) < 0) goto invalid;
         sanitize_script_base_namespace();
         DEBUG_LOG("config:set script_base_namespace=\"%s\"", var_script_base_namespace);
      }
      else if (!strcasecmp(conf_name, "listen_unix")) {
         if (config_parse_str(&var_unix_socket_path, conf_value, 0) < 0) goto invalid;
         DEBUG_LOG("config:set listen_unix=\"%s\"", var_unix_socket_path);
      }
      else if (!strcasecmp(conf_name, "unix_socket_skip_auth")) {
         if (config_parse_bool(&var_unix_socket_skip_auth, conf_value) < 0) goto invalid;
         DEBUG_LOG("config:set unix_socket_skip_auth=%d", var_unix_socket_skip_auth);
      }
      else if (!strcasecmp(conf_name, "interface_map_file")) {
         if (config_parse_str(&var_interface_map_file, conf_value, 0) < 0) goto invalid;
         DEBUG_LOG("config:set interface_map_file=\"%s\"", var_interface_map_file);
      }
      else if (!strcasecmp(conf_name, "max_payload")) {
         uintmax_t parsed = strtoumax(conf_value, NULL, 10);
         if (parsed == 0 || parsed >= SIZE_MAX) goto invalid;
         var_max_payload = (size_t)parsed;
         DEBUG_LOG("config:set max_payload=%zu", var_max_payload);
      }
      else if (!strcasecmp(conf_name, "start_delay")) {
         if (config_parse_int(&var_start_delay, conf_value, 0, INT_MAX) < 0) goto invalid;
         DEBUG_LOG("config:set start_delay=%d", var_start_delay);
      }
      else if (!strcasecmp(conf_name, "worker_model")) {
         if (!strcasecmp(conf_value, "fork"))
            var_worker_model = WORKER_MODEL_FORK;
         else if (!strcasecmp(conf_value, "prefork"))
            var_worker_model = WORKER_MODEL_PREFORK;
         else if (!strcasecmp(conf_value, "adaptive"))
            var_worker_model = WORKER_MODEL_ADAPTIVE;
         else
            goto invalid;
         DEBUG_LOG("config:set worker_model=\"%s\"", worker_model_to_string(var_worker_model));
      }
      else if (!strcasecmp(conf_name, "prefork_workers")) {
         if (config_parse_int(&var_prefork_workers, conf_value, PREFORK_WORKERS_MIN, PREFORK_WORKERS_MAX) < 0)
            goto invalid;
         DEBUG_LOG("config:set prefork_workers=%d", var_prefork_workers);
      }
      else if (!strcasecmp(conf_name, "adaptive_min_workers")) {
         if (config_parse_int(&var_adaptive_min_workers, conf_value, ADAPTIVE_WORKERS_MIN, ADAPTIVE_WORKERS_MAX) < 0)
            goto invalid;
         DEBUG_LOG("config:set adaptive_min_workers=%d", var_adaptive_min_workers);
      }
      else if (!strcasecmp(conf_name, "adaptive_max_workers")) {
         if (config_parse_int(&var_adaptive_max_workers, conf_value, ADAPTIVE_WORKERS_MIN, ADAPTIVE_WORKERS_MAX) < 0)
            goto invalid;
         DEBUG_LOG("config:set adaptive_max_workers=%d", var_adaptive_max_workers);
      }
      else if (!strcasecmp(conf_name, "adaptive_spare_workers")) {
         if (config_parse_int(&var_adaptive_spare_workers, conf_value, 0, ADAPTIVE_WORKERS_MAX) < 0)
            goto invalid;
         DEBUG_LOG("config:set adaptive_spare_workers=%d", var_adaptive_spare_workers);
      }
#ifdef ENABLE_DEBUG_LOGGING
      else if (!strcasecmp(conf_name, "debug")) {
         int v;
         if (config_parse_bool(&v, conf_value) < 0) goto invalid;
         debug_log_set(v);
      }
      else if (!strcasecmp(conf_name, "debug_traffic")) {
         int v;
         if (config_parse_bool(&v, conf_value) < 0) goto invalid;
         debug_traffic_set(v);
      }
#endif
   }

   fclose(fp);
   normalize_worker_config();
   return 1;

invalid:
   fclose(fp);
   log_unexpected_event(LOG_ERR,
                        "Invalid value for config option \"%s\"",
                        conf_name);
   return 0;
}

void normalize_script_base_namespace(void)
{
   sanitize_script_base_namespace();
}

void normalize_worker_config(void)
{
   if (var_prefork_workers < PREFORK_WORKERS_MIN)
      var_prefork_workers = PREFORK_WORKERS_MIN;
   else if (var_prefork_workers > PREFORK_WORKERS_MAX)
      var_prefork_workers = PREFORK_WORKERS_MAX;

   if (var_adaptive_min_workers < ADAPTIVE_WORKERS_MIN)
      var_adaptive_min_workers = ADAPTIVE_WORKERS_MIN;
   else if (var_adaptive_min_workers > ADAPTIVE_WORKERS_MAX)
      var_adaptive_min_workers = ADAPTIVE_WORKERS_MAX;

   if (var_adaptive_max_workers < ADAPTIVE_WORKERS_MIN)
      var_adaptive_max_workers = ADAPTIVE_WORKERS_MIN;
   else if (var_adaptive_max_workers > ADAPTIVE_WORKERS_MAX)
      var_adaptive_max_workers = ADAPTIVE_WORKERS_MAX;

   if (var_adaptive_min_workers > var_adaptive_max_workers)
      var_adaptive_min_workers = var_adaptive_max_workers;

   if (var_adaptive_spare_workers < 0)
      var_adaptive_spare_workers = 0;
   else if (var_adaptive_spare_workers > ADAPTIVE_WORKERS_MAX)
      var_adaptive_spare_workers = ADAPTIVE_WORKERS_MAX;

   if (var_adaptive_spare_workers > var_adaptive_max_workers)
      var_adaptive_spare_workers = var_adaptive_max_workers;
}

const char *worker_model_to_string(worker_model_t model)
{
   switch (model) {
      case WORKER_MODEL_PREFORK:
         return "prefork";
      case WORKER_MODEL_ADAPTIVE:
         return "adaptive";
      case WORKER_MODEL_FORK:
      default:
         return "fork";
   }
}
