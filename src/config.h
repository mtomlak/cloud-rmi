#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#define CONFIGURATION_FILE "/etc/cloud/rmi-server.conf"
#define INTERFACE_MAP_FILE "/etc/cloud/rmi-server.interfaces.map"
#define CONFIGURATION_FILE2 "/etc/cloud/rmi/server.conf"
#define INTERFACE_MAP_FILE2 "/etc/cloud/rmi/server.interfaces.map"
#define CONFIGURATION_FILE3 "/usr/local/etc/cloud-rmi/server.cf"
#define INTERFACE_MAP_FILE3 "/usr/local/etc/cloud-rmi/interfaces.map.cf"
#ifdef PROXY_SUPPORT
#define PROXY_CONFIGURATION_FILE "/etc/cloud/rmi-proxy.conf"
#define PROXY_CONFIGURATION_FILE2 "/etc/cloud/rmi/proxy.conf"
#define PROXY_CONFIGURATION_FILE3 "/usr/local/etc/cloud-rmi/proxy.cf"
#endif


// Globals parsed from config file
#ifdef ENABLE_DEBUG_LOGGING
extern int var_debug;
extern int var_debug_traffic;
#endif
extern int var_port;
extern int var_read_timeout;
extern int var_start_delay;
extern char *var_config_file;
extern char *var_recovery_token;
extern char *var_context_name;
extern char *var_script_exec_dir;
extern char *var_script_base_namespace;
extern char *var_unix_socket_path;
extern int var_unix_socket_skip_auth;
extern char *var_interface_map_file;
extern size_t var_max_payload;

typedef enum {
   WORKER_MODEL_FORK = 0,
   WORKER_MODEL_PREFORK = 1, 
   WORKER_MODEL_ADAPTIVE = 2 
} worker_model_t;

extern worker_model_t var_worker_model;
extern int var_prefork_workers;
extern int var_adaptive_min_workers;
extern int var_adaptive_max_workers;
extern int var_adaptive_spare_workers;

#ifdef PROXY_SUPPORT
extern char *var_proxy;
#endif

// Provided by config.c
char *firsttok(char **s, const char *delim);
int read_config(char *config_file);
int read_iface_config(char *config_file);
void normalize_script_base_namespace();
void normalize_worker_config();
const char *worker_model_to_string(worker_model_t model);

// Default values
#define DEFAULT_PORT 9500
#define DEFAULT_READ_TIMEOUT 10
#define DEFAULT_START_DELAY 0

#define DEFAULT_CONTEXT_NAME "default.rmi"
#define DEFAULT_SCRIPT_EXEC_DIR "/usr/local/libexec/cloud-rmi"
#define DEFAULT_SCRIPT_BASE_NS "ProfiWeb\\Cloud\\Interfaces\\RMI"

#define DEFAULT_WORKER_MODEL WORKER_MODEL_FORK

#define DEFAULT_PREFORK_WORKERS 4
#define PREFORK_WORKERS_MIN 1
#define PREFORK_WORKERS_MAX 1024

#define DEFAULT_ADAPTIVE_MIN_WORKERS 2
#define DEFAULT_ADAPTIVE_MAX_WORKERS 8
#define DEFAULT_ADAPTIVE_SPARE_WORKERS 2
#define ADAPTIVE_WORKERS_MIN PREFORK_WORKERS_MIN
#define ADAPTIVE_WORKERS_MAX PREFORK_WORKERS_MAX
