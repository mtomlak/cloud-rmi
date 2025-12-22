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
#ifdef REUSE_WORKER_PROC
extern int var_min_spare_workers;
extern int var_max_spare_workers;
#endif
extern char *var_config_file;
extern char *var_recovery_token;
extern char *var_context_name;
extern char *var_script_exec_dir;
extern char *var_script_base_namespace;
extern char *var_unix_socket_path;
extern int var_unix_socket_skip_auth;
extern char *var_interface_map_file;
extern size_t var_max_payload;
#ifdef PROXY_SUPPORT
extern char *var_proxy;
#endif

// Provided by config.c
char *firsttok(char **s, const char *delim);
int read_config(char *config_file);
int read_iface_config(char *config_file);
void normalize_script_base_namespace();
