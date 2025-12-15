#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROXY_PROTOCOL_VERSION	"1.0"
#define PROXY_CHILD_LIST_SZ	128
#define PROXY_LIST_SZ		32
#define PROXY_CONN_LIST_SZ	32
#define PROXY_IFACE_MAX_SZ	512
#define PROXY_IFACE_LIST_SZ	16
#define PROXY_HOSTNAME_SZ	256

struct proxy_conn{
	int conn_id;
	int proxy_id;
	int sock;
	int is_busy;
};

struct proxy_server{
	int proxy_id;
	char hostname[256];
	int port;
	struct proxy_conn conns[PROXY_CONN_LIST_SZ];
	int conn_count;
};

struct proxy_iface_entry{
   char *iface;
   int proxy_id;
   struct proxy_iface_entry *next;
};

struct proxy_child_entry{
	int pid;
	struct proxy_conn proxy_conns[PROXY_LIST_SZ];
	struct proxy_child_entry *next;
	struct proxy_child_entry *prev; 
};

extern struct proxy_server proxy_list[PROXY_LIST_SZ];
extern struct proxy_child_entry *proxy_child_list;
extern int proxy_count;
extern struct proxy_iface_entry *proxy_iface_map;

int proxy_start(char *host_addresses);
void proxy_send_stats(int sock);
int proxy_pass_call_cmd(char *iface, char *cmd, char *arg, struct conn_state *state);
int proxy_pass_call_cmd_ex(
   const char *iface,
   const char *cmd,
   const char *arg,
   const uint8_t *payload,
   size_t payload_len,
   const char *encoding,
   struct conn_state *state);
void proxy_print_ifaces(struct conn_state *state);
