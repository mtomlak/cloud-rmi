#ifndef CLOUD_RMI_LEGACY_H
#define CLOUD_RMI_LEGACY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cloudrmi/rmi.h"
#include "cloudrmi/rmi_server.h"
#include "cloudrmi/rmi_connection.h"
#include "cloudrmi/rmi_protocol.h"
#include "cloudrmi/rmi_payload.h"
#include "cloudrmi/rmi_error.h"

#ifdef ENABLE_LINUX_FEATURES
        #define CHILD_STACK_SZ          BUF_SZ * 24

        #ifndef DISABLE_WORKER_REUSE
                /* These are just default values */
                #define REUSE_WORKER_PROC       1
                #define MIN_SPARE_WORKERS       3
                #define MAX_SPARE_WORKERS       10
                #define EXTRA_WORKER_TTL        60
        #endif
#endif

struct server_iface_entry{
   char *iface;
   char *file;
   struct server_iface_entry *next;
   struct server_iface_entry *prev;
};

#ifdef ENABLE_LINUX_FEATURES
struct child_proc_state{
   int sock;
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

extern int log_level;
extern int var_server_start_time;
extern struct server_iface_entry *var_server_iface_map;
#ifdef REUSE_WORKER_PROC
extern struct spare_child_workers_list server_spare_children;
#endif

void die_err(char *errstr);
int stream_write(int sock, char *format, ...);
int stream_writeln(int sock, char *format, ...);
int stream_read(int sock, char* buf, int buf_size);
int stream_readln(int sock, char* line, int max_size);
int stream_readlns(int sock, char* line, int max_size);
void log_event(int priority, char *format, ...);
void substring(char *string, char *sub, int start, int len);
void split_host_addr(char *host, char *ip, int *port);
char *get_ip_addr(char *hostname);
void server_add_iface_entry(char *iface_name, char *file_name);
struct server_iface_entry *server_get_iface_entry(char *iface_name);
int read_iface_config(char *config_file);
struct helper_proc_state *new_helper_state(void *stack, int stack_size);
void server_send_stats(int sock);
void child_exit_self(void *stack, int stack_size);
int server_start(rmi_server_t *server, int port_num);

#endif
