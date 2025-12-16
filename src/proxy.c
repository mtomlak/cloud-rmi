#ifdef ENABLE_LINUX_FEATURES
   #define _GNU_SOURCE             /* See feature_test_macros(7) */
   #include <sched.h>
   #include <math.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "cloud-rmi.h"
#include "config.h"
#include "proxy.h"
#include "client.h"

/**
 * Provides proxy server functionality
 *
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license BSD
 */

// Proxy reladed variables
//struct proxy_server proxy_list[PROXY_LIST_SZ];
//int proxy_list_len = 0;
//
struct proxy_server proxy_list[PROXY_LIST_SZ];
struct proxy_child_entry *proxy_child_list = NULL;
int proxy_count;
int proxy_manager_pipe[2];
int var_proxy_min_spare_conns = 3;
struct proxy_iface_entry *proxy_iface_map;

// refered from inside
int proxy_get_spare_conn_count(int proxy_id);

static int proxy_write_all(int fd, const uint8_t *buf, size_t len)
{
   size_t total = 0;

   while (total < len) {
      ssize_t written;

      do {
         written = write(fd, buf + total, len - total);
      } while (written < 0 && errno == EINTR);

      if (written < 0) {
         // Proxy sockets are expected to be blocking; treat EAGAIN/EWOULDBLOCK as fatal to avoid spin
         if (errno == EAGAIN || errno == EWOULDBLOCK)
            errno = EIO;
         return 0;
      }

      if (written == 0)
         return 0;

      total += (size_t)written;
   }

   return 1;
}


//
// Proxy list management
//

int proxy_list_count()
{
   return proxy_count;
}

struct proxy_server *proxy_list_get(int proxy_id)
{
     if (proxy_id > proxy_count)
      die_err("Error: proxy_list_get(): Cannot load proxy from memory");

   return &proxy_list[proxy_id];
}

void proxy_list_set(int proxy_id, struct proxy_server proxy)
{
     if (proxy_id > proxy_count)
      die_err("Error: proxy_list_get(): Cannot load proxy from memory");

   proxy_list[proxy_id] = proxy;
}

int proxy_list_add(struct proxy_server proxy)
{
   int id = proxy_count;

   proxy.proxy_id = id;
   proxy_list[id] = proxy;
   proxy_count++;

   return id;
}

struct proxy_server new_proxy_struct(char *hostname, int port)
{
   struct proxy_server proxy;

   proxy.proxy_id = 0;      // will be set when it gets ID assigned
   proxy.conn_count = 0;
   proxy.port = port;
   strcpy(proxy.hostname, hostname);

   return proxy;
}

int proxy_list_add_new(char *hostname, int port)
{
   return proxy_list_add(new_proxy_struct(hostname, port));
}

//
// Proxy connection list
//

int proxy_add_conn(int proxy_id, struct rmi_client_conn client)
{
   if (proxy_id > proxy_count)
      die_err("Error: proxy_add_conn(): Cannot load proxy from memory");

   int ci = proxy_list[proxy_id].conn_count;

   proxy_list[proxy_id].conns[ci].client = client;
   proxy_list[proxy_id].conns[ci].is_busy = 0;
   proxy_list[proxy_id].conns[ci].proxy_id = proxy_id;
   proxy_list[proxy_id].conn_count++;

   return ci;
}

struct proxy_conn *proxy_get_conn(int proxy_id, int conn_id)
{
   if (proxy_id > proxy_count || conn_id > proxy_list[proxy_id].conn_count)
      die_err("Error: Cannot load proxy connection from memory");

   return &proxy_list[proxy_id].conns[conn_id];
}

/**
 * Starts new proxy connection
 * 
 */
int proxy_start_new_conn(int proxy_id)
{
   struct rmi_client_conn client;

   // Connect
   if (client_connect(
      proxy_list_get(proxy_id)->hostname,
      proxy_list_get(proxy_id)->port,
      &client
      ) < 0)
      die_err("Error: Failed to connect to the server as a proxy");

   // Authenticate
   if (var_recovery_token != NULL)
      if (!client_auth_by_token(&client, var_recovery_token))
         die_err("Error: Token authentication failed");

   // Add socket to the list
   return proxy_add_conn(proxy_id, client);
}

int proxy_restart_conn(struct proxy_conn *conn)
{
   int proxy_id;

   if (var_debug)
      printf("Proxy: Restarting connection to proxy #%i", conn->proxy_id);

   proxy_id = conn->proxy_id;
   client_disconnect(&conn->client);
   if (client_connect(
      proxy_list_get(proxy_id)->hostname,
      proxy_list_get(proxy_id)->port,
      &conn->client
      ) < 0)
      die_err("Error: Failed to connect to the server as a proxy");

   if (var_recovery_token != NULL)
      if (!client_auth_by_token(&conn->client, var_recovery_token))
         die_err("Error: Token authentication failed");

   return 1;
}

/**
 * Gets first unused connection to the given proxy.
 */
struct proxy_conn *proxy_get_free_conn(int proxy_id)
{
   int i;

   if (proxy_id > proxy_count)
      die_err("Error: Cannot load proxy connection from memory");

   for (i = 0; i < proxy_list[proxy_id].conn_count; i++) {
      if (!proxy_list[proxy_id].conns[i].is_busy)
         return &proxy_list[proxy_id].conns[i];
   }

   // If we got here, all connections are busy, need to create new one
   i = proxy_start_new_conn(proxy_id);

   return &proxy_list[proxy_id].conns[i];
}

void proxy_set_conn_busy(struct proxy_conn *conn)
{
   //if (var_debug)
   //   printf("Proxy: Locking connection #%i/%i as busy\n", conn->proxy_id, conn->conn_id);

   conn->is_busy = 1;
}

void proxy_set_conn_free(struct proxy_conn *conn)
{
   //if (var_debug)
   //   printf("Proxy: Unlocking connection #%i/%i as free\n", conn->proxy_id, conn->conn_id);

   conn->is_busy = 0;
}

//
// Proxy interface list
//


struct proxy_iface_entry *new_proxy_iface_entry(char *iface_name, int proxy_id)
{
   struct proxy_iface_entry *result = malloc(sizeof(struct proxy_iface_entry));

   result->iface = strdup(iface_name);
   result->proxy_id = proxy_id;
   result->next = NULL;

   return result;
}

struct proxy_iface_entry *proxy_get_last_iface_entry()
{
   struct proxy_iface_entry *cur;

   if (proxy_iface_map == NULL)
      return NULL;

   cur = proxy_iface_map;

   while (cur->next != NULL) {
      cur = cur->next;
   }

   return cur;
}

struct proxy_iface_entry *proxy_get_iface_entry(char *iface_name)
{
   struct proxy_iface_entry *cur;

   if (proxy_iface_map == NULL)
      return NULL;

   cur = proxy_iface_map;

   while (cur != NULL) {
      if (strcmp(iface_name, cur->iface) == 0)
         return cur;

      cur = cur->next;
   }

   return NULL;
}

void proxy_add_iface_entry(int proxy_id, char *iface_name)
{
   if (var_debug)
      printf("Proxy: Adding interface %s for proxy #%i\n", iface_name, proxy_id);

   if (proxy_iface_map == NULL) // adding first item
      proxy_iface_map = new_proxy_iface_entry(iface_name, proxy_id);
   else
      proxy_get_last_iface_entry()->next = new_proxy_iface_entry(iface_name, proxy_id);
}

int proxy_fetch_interfaces(int proxy_num)
{
   int status_code, byte; 
   int size = 0;
   int ln = 0;
   struct proxy_conn *conn;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));
   char *iface = malloc(sizeof(char) * (BUF_SZ + 1));

   bzero(response, BUF_SZ);
   bzero(iface, BUF_SZ);

   if (proxy_num > proxy_count)
      die_err("Error: Cannot load interfaces from non-existend proxy");

   // Get interfaces
   conn = proxy_get_free_conn(proxy_num);
   proxy_set_conn_busy(conn);
   status_code = client_raw_cmd(&conn->client, "IFACES", response);
   proxy_set_conn_free(conn);

   if (status_code != 200)
      die_err("Error: Cannot load interfaces from non-existend proxy");

   // Load each line of data into array
   for(byte = 0; byte <= strlen(response); byte++) {
      if (response[byte] == '\r')
         continue;

      if (response[byte] == '\n' || response[byte] == '\0') {
         if (size > 0) {
            proxy_add_iface_entry(proxy_num, iface);
         }
         bzero(iface, BUF_SZ);
         size = 0;
         ln++;
         continue;
      }

      if (ln > 0) {
         iface[size] = response[byte];
         size++;
      }
   }

   free(iface);
   free(response);

   return 1;
}


//
// Other proxy methods
//


/**
 * Prints information about proxy usage
 * 
 */
void proxy_send_stats(int sock)
{
   int i, j;
   struct proxy_server *proxy;

   stream_writeln(sock, "Server Connections:\nHostname:\tPort:\tConnections (free/total):");

   for (i = 0; i < proxy_count; i++) {
      proxy = proxy_list_get(i);
      stream_writeln(sock,  "%s\t%i\t%i/%i", proxy->hostname, proxy->port, proxy_get_spare_conn_count(i), proxy->conn_count);
   }

   stream_writeln(sock, "\nClient Connections:");
   stream_writeln(sock, "Connection ID:\tProxy ID:\tSocket:\tIs Busy:");

   for (i = 0; i < proxy_count; i++) {
      proxy = proxy_list_get(i);

      if (proxy->conn_count > 0) {
          for (j = 0; j < proxy->conn_count; j++) {
              struct proxy_conn *conn = &proxy->conns[j];
              stream_writeln(sock, "%i\t\t%i\t\t%i\t%i", conn->conn_id, conn->proxy_id, conn->client.sock, conn->is_busy);
          }
      }
   }
   stream_write(sock, "\n\n");   // end data with empty line
}


int proxy_pass_raw_cmd(int proxy_num, struct conn_state *client_conn, char *cmd)
{
   int status_code, ret = 1; 
   struct proxy_conn *conn;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));

   bzero(response, BUF_SZ);

   if (proxy_num > proxy_list_count())
      die_err("Error: proxy_pass_raw_cmd(): Cannot find proxy by ID");

   // Get interfaces
   conn = proxy_get_free_conn(proxy_num);
   proxy_set_conn_busy(conn);
   status_code = client_raw_cmd(&conn->client, cmd, response);

   if (!status_code || status_code == 408) {  // 408 - request timeout
      if (var_debug)
         printf("Proxy: Could not get status code, trying to reconnect\n");

      proxy_restart_conn(conn);
      status_code = client_raw_cmd(&conn->client, cmd, response);
   }

   if (!status_code) {
      status_code = 503;
      strcpy(response, "Service Unavailable");

      if (var_debug)
         printf("Proxy: Failed get status code from proxied server #%i\n", proxy_num);

      ret = 0;
   }

   stream_writeln(client_conn->sock, "%i %s", status_code, response);
   proxy_set_conn_free(conn);

   free(response);

   return ret;
}

int proxy_pass_call_cmd_ex(
   const char *iface,
   const char *cmd,
   const char *arg,
   const uint8_t *payload,
   size_t payload_len,
   const char *encoding,
   struct conn_state *client_conn)
{
   int ret = 1;
   struct proxy_iface_entry *iface_entry;
   struct proxy_conn *conn;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));
   char *message = malloc(sizeof(char) * (BUF_SZ + 1));
   char *payload_response = NULL;

   if (response == NULL || message == NULL) {
      free(response);
      free(message);
      return 0;
   }

   iface_entry = proxy_get_iface_entry((char *)iface);

   if (iface_entry == NULL) {
      stream_writeln(client_conn->sock, "400 Unsupported interface: %s", iface);
      free(response);
      free(message);
      return ret;
   }

   if (var_debug)
      printf("Proxy: Passing CALL command to proxy #%i\n", iface_entry->proxy_id);

   conn = proxy_get_free_conn(iface_entry->proxy_id);
   proxy_set_conn_busy(conn);

   bzero(response, BUF_SZ);
   bzero(message, BUF_SZ);

   if (payload != NULL) {
      const char *enc_value = (encoding != NULL && encoding[0] != '\0') ? encoding : "NONE";

      if (stream_writeln(conn->client.sock, "CALL %s %s", iface, cmd) < 0) {
         ret = 0;
         goto cleanup;
      }

      if (stream_writeln(conn->client.sock, "PAYLOAD %s %zu", enc_value, payload_len) < 0) {
         ret = 0;
         goto cleanup;
      }

      if (payload_len > 0 && !proxy_write_all(conn->client.sock, payload, payload_len)) {
         ret = 0;
         goto cleanup;
      }
   } else {
      if (stream_writeln(conn->client.sock, "CALL %s %s %s", iface, cmd, arg) < 0) {
         ret = 0;
         goto cleanup;
      }
   }

   if (stream_readln(conn->client.sock, response, BUF_SZ) < 0) {
      ret = 0;
      goto cleanup;
   }

   int status_code = 0;
   if (!client_parse_status_code(response, &status_code, message)) {
      ret = 0;
      goto cleanup;
   }

   if (response[strlen(response) - 1] == ':') {
      if (stream_readlns(conn->client.sock, response, BUF_SZ) < 0) {
         ret = 0;
         goto cleanup;
      }

      size_t remaining = BUF_SZ - strlen(message) - 1;
      strncat(message, "\n", remaining);
      remaining = BUF_SZ - strlen(message) - 1;
      strncat(message, response, remaining);
   }

   char response_enc[BUF_SZ + 1];
   char response_keyword[BUF_SZ + 1];
   unsigned long long response_len = 0;
   int is_payload_response = 0;

   bzero(response_enc, sizeof(response_enc));
   bzero(response_keyword, sizeof(response_keyword));

   if (status_code == 200 || status_code == 500) {
      if (sscanf(message, "%s %s %llu", response_keyword, response_enc, &response_len) == 3 &&
          strcmp(response_keyword, "PAYLOAD") == 0 &&
          response_len <= SIZE_MAX) {
         is_payload_response = 1;
         payload_response = malloc((size_t)response_len + 1);

         if (payload_response == NULL) {
            ret = 0;
            goto cleanup;
         }

         size_t received = 0;
         while (received < (size_t)response_len) {
            ssize_t n = read(conn->client.sock, payload_response + received, (size_t)response_len - received);

            if (n < 0) {
               ret = 0;
               goto cleanup;
            }

            if (n == 0)
               break;

            received += (size_t)n;
         }

         if (received < (size_t)response_len) {
            ret = 0;
            goto cleanup;
         }

         payload_response[(size_t)response_len] = '\0';

         // Consume trailing newline if present
         char trailing;
         if (read(conn->client.sock, &trailing, 1) < 0) {
            ret = 0;
            goto cleanup;
         }
      }
   }

   if (is_payload_response && payload_response != NULL) {
      if (client_conn->protocol_version >= 2) {
         stream_writeln(client_conn->sock, "%i PAYLOAD %s %llu", status_code, response_enc, response_len);

         if (response_len > 0)
            proxy_write_all(client_conn->sock, (const uint8_t *)payload_response, (size_t)response_len);

         stream_write(client_conn->sock, "\n");
      } else {
         size_t encoded_len = (((size_t)response_len + 2) / 3) * 4 + 1;
         char *encoded = malloc(encoded_len);

         if (encoded == NULL) {
            ret = 0;
            goto cleanup;
         }

         if (response_len > 0)
            to64frombits((unsigned char *)encoded, (const unsigned char *)payload_response, (int)response_len);
         else
            encoded[0] = '\0';

         stream_writeln(client_conn->sock, "%i %s", status_code, encoded);
         free(encoded);
      }
   } else {
      stream_writeln(client_conn->sock, "%i %s", status_code, message);
   }

cleanup:
   proxy_set_conn_free(conn);
   free(response);
   free(message);
   free(payload_response);

   return ret;
}

int proxy_pass_call_cmd(char *iface, char *cmd, char *arg, struct conn_state *client_conn)
{
   return proxy_pass_call_cmd_ex(iface, cmd, arg, NULL, 0, NULL, client_conn);
}

void proxy_print_ifaces(struct conn_state *state)
{   
   struct proxy_iface_entry *entry;

   stream_writeln(state->sock, "200 Supported CALL interfaces:");

   if (proxy_iface_map == NULL)
      stream_writeln(state->sock, "- Empty proxy interface map -");

   entry = proxy_iface_map;

   while (entry != NULL) {
      stream_writeln(state->sock, "%s", entry->iface);
      entry = entry->next;
   }
   stream_write(state->sock, "\n");
}

int proxy_get_spare_conn_count(int proxy_id)
{
   int i, count = 0;

   for (i = 0; i < proxy_list[proxy_id].conn_count; i++) {
      if (!proxy_list[proxy_id].conns[i].is_busy)
         count++;
   }

   return count;
}

void proxy_check_spare_conns(int proxy_id)
{
   int i, spare_conns = proxy_get_spare_conn_count(proxy_id);

   for(i = spare_conns; i < var_proxy_min_spare_conns; i++)
      proxy_start_new_conn(proxy_id);
}

void proxy_all_check_spare_conns()
{
   int id;

   for(id = 0; id < proxy_count; id++)
      proxy_check_spare_conns(id);  
}

//
// Proxy child list
//


struct proxy_child_entry *proxy_get_last_child_entry()
{
   struct proxy_child_entry *cur;

   if (proxy_child_list == NULL)
      return NULL;

   cur = proxy_child_list;

   while (cur->next != NULL) {
      cur = cur->next;
   }

   return cur;
}

struct proxy_child_entry *proxy_get_child_entry(int pid)
{
   struct proxy_child_entry *cur;

   if (proxy_child_list == NULL)
      return NULL;

   cur = proxy_child_list;

   while (cur != NULL) {
      if (cur->pid == pid)
         return cur;

      cur = cur->next;
   }

   return NULL;
}

struct proxy_child_entry *new_proxy_child_entry()
{
   struct proxy_child_entry *result = malloc(sizeof(struct proxy_child_entry));

   result->pid = 0;
   result->prev = proxy_get_last_child_entry();
   result->next = NULL;

   return result;
}

struct proxy_child_entry *proxy_add_child_entry()
{
   struct proxy_child_entry *entry = new_proxy_child_entry();

   if (proxy_child_list == NULL) // adding first item
      proxy_child_list = entry;
   else
      proxy_get_last_child_entry()->next = entry;

   return entry;
}


int proxy_conn_keepalive(struct proxy_conn *conn)
{
   int status_code, ret = 1;
   char *response = malloc(sizeof(char) * (BUF_SZ + 1));

   bzero(response, BUF_SZ);

   proxy_set_conn_busy(conn);
   status_code = client_raw_cmd(&conn->client, "NOOP", response);

   // Try to reconnect stale connection
   if (!status_code || status_code == 408) {  // 408 - request timeout
      proxy_restart_conn(conn);
      status_code = client_raw_cmd(&conn->client, "NOOP", response);
   }

   if (!status_code) {
      status_code = 503;
      printf("Warning: Failed to connect to server %i", conn->proxy_id);
      ret = 0;
   }
   proxy_set_conn_free(conn);

   free(response);
}

void proxy_all_keepalive_conns()
{
   int i, j;
   struct proxy_server *proxy;

   for (i = 0; i < proxy_list_count(); i++) {
      proxy = proxy_list_get(i);

      if (proxy == NULL)
         continue;

      for (j = 0; j < proxy_list[i].conn_count; j++) {
         if (!proxy_list[i].conns[j].is_busy)
            proxy_conn_keepalive(&proxy_list[i].conns[j]);
      }
   }
}

// When we have Linux clone() with shared memory, we can do the management more nicely
#ifdef ENABLE_LINUX_FEATURES
int start_proxy_manager(void *arg)
{
   int sec_passed = 0;
   struct helper_proc_state *rcvd_state;
   struct helper_proc_state state;

   // we can copy the data and free target now
   rcvd_state = (struct helper_proc_state *) arg;
   state = *rcvd_state;
   free(rcvd_state);

   while (1) {
      sleep(10);

      if ((int) fmod(sec_passed, 60) == 0) { // do every minute
         if (var_debug)
            printf("ProxyManager: Checking all connections\n");
         proxy_all_keepalive_conns();  // send NOOP command to keep alive all connections
         if (var_debug)
            printf("ProxyManager: Checking number of spare connections per each proxy\n");
         proxy_all_check_spare_conns();    // check number of spare connections
      }
      sec_passed += 10;
   }

   child_exit_self(state.stack, state.stack_size);
}

int create_proxy_manager_proc()
{
   int pid = 0;
   char *stack = malloc(CHILD_STACK_SZ + 1);
   struct helper_proc_state *helper_state = new_helper_state(stack, CHILD_STACK_SZ);

   if (var_debug) {
      printf("Starting proxy proccess manager\n");
      printf("Creating new manager proccess with Linux clone() ...\n");
   }

   pid = clone(&start_proxy_manager, (char *)stack + CHILD_STACK_SZ, CLONE_VM|CLONE_FILES|CLONE_SIGHAND, helper_state);//CLONE_VM|CLONE_FS|CLONE_SIGHAND|

   if (pid < 0) {
      printf("Error: clone() failed");
      return 0;
   }

   if (var_debug)
      printf("+ Started new manager proccess with PID %i\n", pid);

   return pid;
}
#endif

//void proxy_

/**
 * Starts new proxy link
 * 
 */
int proxy_start_new(char *proxy_ip, int proxy_port)
{
   int sock, id, i;
   struct proxy_server proxy;

   if (var_debug)
      printf("Starting proxy to %s:%i\n", proxy_ip, proxy_port);

   // Add new proxy struct to the list
   id = proxy_list_add_new(proxy_ip, proxy_port);

   for(i = 0; i < var_proxy_min_spare_conns; i++)
      proxy_start_new_conn(id);

   proxy_fetch_interfaces(id);
}

/**
 * Starts the proxy server functionality
 * 
 */
int proxy_start(char *host_addresses)
{
   int proxy_port, byte, size = 0;
   char *hostname = malloc(sizeof(char) * (PROXY_HOSTNAME_SZ + 1));
   char *address = malloc(sizeof(char) * (PROXY_HOSTNAME_SZ + 1));
   char *proxy_ip;

   bzero(hostname, PROXY_HOSTNAME_SZ);
   bzero(address, PROXY_HOSTNAME_SZ);

   // Parse list of proxy servers separated by ","
   for(byte = 0; byte <= strlen(host_addresses); byte++) {
      if (host_addresses[byte] == ' ') // skip spaces
         continue;

      if (host_addresses[byte] == ',' || host_addresses[byte] == '\0') {
         split_host_addr(address, hostname, &proxy_port);
         proxy_ip = get_ip_addr(hostname);

         if (proxy_ip == NULL) {
            die_err("failed to resolve proxy hostname");
         }
         proxy_start_new(proxy_ip, proxy_port);

         bzero(hostname, PROXY_HOSTNAME_SZ);
         bzero(address, PROXY_HOSTNAME_SZ);
         size = 0;
         continue;
      }

      address[size] = host_addresses[byte];
      size++;
   }

#ifdef ENABLE_LINUX_FEATURES
   // Create proxy manager proccess
   create_proxy_manager_proc();
#endif

   //free(proxy_ip);
   //free(address);
}
