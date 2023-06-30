#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME "CloudRMI"
#define APP_VERSION "0.9.3"
#define PROTOCOL_VERSION "1.0"
#define BUF_SZ 10240

#ifdef PROXY_SUPPORT
   #define PROTO_EXTENSIONS "PROXY-STATS SERVER-STATS"
#else
   #define PROTO_EXTENSIONS "SERVER-STATS"
#endif

#ifdef ENABLE_LINUX_FEATURES
	#define CHILD_STACK_SZ  	BUF_SZ * 24   // seems safe above BUF_SZ * 10 up to version 0.91

	#ifndef DISABLE_WORKER_REUSE  
		/* These are just default values */
		#define REUSE_WORKER_PROC	1
		#define MIN_SPARE_WORKERS	3
		#define MAX_SPARE_WORKERS	10
		#define EXTRA_WORKER_TTL	60	
	#endif
#endif


// Struct type definitions
struct conn_state{
   int sock;
   int is_authenticated;
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
#ifdef REUSE_WORKER_PROC
extern struct spare_child_workers_list server_spare_children;
#endif

void die_err(char *errstr);
int stream_write(int sock, char *format, ...);
int stream_writeln(int sock, char *format, ...);	// TODO: now some code still uses write instead writeln
int stream_read(int sock, char* buf, int buf_size);
int stream_readln(int sock, char* line, int max_size);
int stream_readlns(int sock, char* line, int max_size);
void log_event(int priority, char *format, ...);
void substring(char *string, char *sub, int start, int len);
void split_host_addr(char *host, char *ip, int *port);
char *get_ip_addr(char *hostname);
// base64.c
void to64frombits(unsigned char *out, const unsigned char *in, int inlen);
int from64tobits(char *out, const char *in);
// server.c
int server_start(int port_num);
void server_add_iface_entry(char *iface_name, char *file_name);
struct server_iface_entry *server_get_iface_entry(char *iface_name);
int read_iface_config(char *config_file);
struct helper_proc_state *new_helper_state(void *stack, int stack_size);
void server_send_stats(int sock);
// protocol.c
void handle_connection(int sock);
// handle_call.c
int handle_call_cmd(char *iface, char *cmd, char *arg, struct conn_state *state);

// proxy.c
#ifdef PROXY_SUPPORT
int proxy_start(char *host_addr);
void proxy_all_keepalive_conns();
void proxy_all_check_spare_conns();
#endif
