#ifndef CLOUD_RMI_H
#define CLOUD_RMI_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cloudrmi/version.h>
#include <cloudrmi/types.h>
#include <cloudrmi/error.h>
#include <cloudrmi/payload.h>
#include <cloudrmi/protocol.h>
#include "app_version.h"

#define PROTOCOL_VERSION CLOUDRMI_PROTOCOL_VERSION
#define PROTOCOL_VERSION_STRING CLOUDRMI_PROTOCOL_VERSION_STRING
#define LIBCLOUDRMI_VERSION CLOUDRMI_LIBRARY_VERSION
#define LIBCLOUDRMI_VERSION_MAJOR CLOUDRMI_LIBRARY_VERSION_MAJOR
#define LIBCLOUDRMI_VERSION_MINOR CLOUDRMI_LIBRARY_VERSION_MINOR
#define LIBCLOUDRMI_VERSION_PATCH CLOUDRMI_LIBRARY_VERSION_PATCH
#define BUF_SZ RMI_BUF_SZ

#ifndef ENABLE_SYSLOG_TRACE
#define ENABLE_SYSLOG_TRACE 1
#endif

#ifndef ENABLE_DEBUG_LOGGING
#define ENABLE_DEBUG_LOGGING 1
#endif

#ifndef LOG_DEBUG
#define LOG_DEBUG 7
#endif
#ifndef LOG_INFO
#define LOG_INFO 6
#endif
#ifndef LOG_NOTICE
#define LOG_NOTICE 5
#endif
#ifndef LOG_WARNING
#define LOG_WARNING 4
#endif
#ifndef LOG_ERR
#define LOG_ERR 3
#endif

#ifdef ENABLE_LINUX_FEATURES
        #define CHILD_STACK_SZ          BUF_SZ * 24   // seems safe above BUF_SZ * 10 up to version 0.91

        #ifndef DISABLE_WORKER_REUSE
                /* These are just default values */
                #define REUSE_WORKER_PROC       1
                #define MIN_SPARE_WORKERS       3
                #define MAX_SPARE_WORKERS       10
                #define EXTRA_WORKER_TTL        60
        #endif
#endif


// Struct type definitions
struct conn_state{
   int sock;
   int is_authenticated;
   int protocol_version;
   int is_unix_socket;
   char username[256];
};

struct server_iface_entry{
   char *iface;
   char *file;
   struct server_iface_entry *next;
   struct server_iface_entry *prev;
};

#ifdef ENABLE_LINUX_FEATURES
struct child_proc_state{
   int sock;
   int is_unix_socket;
   void * stack;
   int stack_size;
};

        #ifdef REUSE_WORKER_PROC
struct helper_proc_state{
   void * stack;
   int stack_size;
};

struct spare_child_worker{
   int pid;
   int next_sock;
   int stop_signal;
   struct spare_child_worker *next;
   struct spare_child_worker *prev;
};

struct spare_child_workers_list{
   struct spare_child_worker *first;
   struct spare_child_worker *last;
   int count;
};
        #endif
#endif

// Globals that are not config variables
extern int log_level;
extern int var_server_start_time;
extern struct server_iface_entry *var_server_iface_map;
extern int var_run_as_daemon;
#ifdef REUSE_WORKER_PROC
extern struct spare_child_workers_list server_spare_children;
#endif

void die_err(char *errstr);
int stream_write(int sock, char *format, ...);
int stream_writeln(int sock, char *format, ...);        // TODO: now some code still uses write instead writeln
int stream_read(int sock, char* buf, int buf_size);
int stream_readln(int sock, char* line, int max_size);
int stream_readlns(int sock, char* line, int max_size);
void log_event(int priority, char *format, ...);
void log_unexpected_event(int priority, const char *format, ...);

#ifdef ENABLE_DEBUG_LOGGING
extern int var_debug;
extern int var_debug_traffic;
int debug_log_enabled(void);
void debug_log_set(int enabled);
int debug_traffic_enabled(void);
void debug_traffic_set(int enabled);
void debug_log_message(const char *format, ...);
void debug_log_stream(char direction, const char *payload);
void log_traffic_event(const struct conn_state *state, const char *format, ...);
#define DEBUG_LOG(...) debug_log_message(__VA_ARGS__)
#define DEBUG_STREAM(direction, payload) debug_log_stream(direction, payload)
#else
static inline int debug_log_enabled(void) { return 0; }
static inline void debug_log_set(int enabled) { (void)enabled; }
static inline int debug_traffic_enabled(void) { return 0; }
static inline void debug_traffic_set(int enabled) { (void)enabled; }
static inline void debug_log_message(const char *format, ...) { (void)format; }
static inline void debug_log_stream(char direction, const char *payload) { (void)direction; (void)payload; }
static inline void log_traffic_event(const struct conn_state *state, const char *format, ...) { (void)state; (void)format; }
#define DEBUG_LOG(...) do {} while(0)
#define DEBUG_STREAM(direction, payload) do { (void)(direction); (void)(payload); } while(0)
#endif

void substring(char *string, char *sub, int start, int len);
void split_host_addr(char *host, char *ip, int *port);
char *get_ip_addr(char *hostname);
// server.c
int server_start(int port_num, const char *unix_socket_path);
void server_add_iface_entry(char *iface_name, char *file_name);
struct server_iface_entry *server_get_iface_entry(char *iface_name);
int read_iface_config(char *config_file);
struct helper_proc_state *new_helper_state(void *stack, int stack_size);
void server_send_stats(int sock);
void child_exit_self(void *stack, int stack_size);
// protocol.c
void handle_connection(int sock, int is_unix_socket);
// handle_call.c
int handle_call_cmd(
   char *iface,
   char *cmd,
   char *arg,
   const uint8_t *payload,
   size_t payload_len,
   const char *encoding,
   struct conn_state *state);

// proxy.c
#ifdef PROXY_SUPPORT
int proxy_start(char *host_addr);
void proxy_all_keepalive_conns();
void proxy_all_check_spare_conns();
#endif

#endif /* CLOUD_RMI_H */
