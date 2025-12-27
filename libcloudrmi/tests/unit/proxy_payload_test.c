#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>

#include "cloud-rmi.h"
#include "proxy.h"

static void reset_proxy_state(void)
{
   while (proxy_iface_map != NULL) {
      struct proxy_iface_entry *next = proxy_iface_map->next;
      free(proxy_iface_map->iface);
      free(proxy_iface_map);
      proxy_iface_map = next;
   }

   proxy_count = 0;
   memset(proxy_list, 0, sizeof(proxy_list));
}

static void prepare_proxy(int upstream_sock, const char *iface)
{
   reset_proxy_state();

   proxy_count = 1;
   proxy_list[0].proxy_id = 0;
   proxy_list[0].conn_count = 1;
   proxy_list[0].conns[0].conn_id = 0;
   proxy_list[0].conns[0].proxy_id = 0;
   proxy_list[0].conns[0].client.sock = upstream_sock;
   proxy_list[0].conns[0].client.protocol_version = 2;  /* Assume v2 for tests */
   proxy_list[0].conns[0].client.capabilities = 0;
   proxy_list[0].conns[0].client.capabilities_known = 1;
   proxy_list[0].conns[0].is_busy = 0;

   struct proxy_iface_entry *entry = malloc(sizeof(struct proxy_iface_entry));
   assert(entry != NULL);
   entry->iface = strdup(iface);
   assert(entry->iface != NULL);
   entry->proxy_id = 0;
   entry->next = NULL;

   proxy_iface_map = entry;
}

static void read_exact(int fd, uint8_t *buf, size_t len)
{
   size_t total = 0;

   while (total < len) {
      ssize_t n = read(fd, buf + total, len - total);
      assert(n > 0);
      total += (size_t)n;
   }
}

static void *upstream_responder(void *arg)
{
   int upstream_fd = *(int *)arg;
   const char *response = "200 OK\n";
   char request[256];
   ssize_t n;
   
   /* Read request line */
   n = read(upstream_fd, request, sizeof(request) - 1);
   if (n > 0) {
      request[n] = '\0';
      /* Check if it's a payload request - if so, read payload bytes */
      if (strstr(request, "PAYLOAD") != NULL) {
         /* Parse payload length from "CALL ... PAYLOAD ... <len>" */
         char *len_str = strrchr(request, ' ');
         if (len_str != NULL) {
            int payload_len = atoi(len_str + 1);
            if (payload_len > 0) {
               uint8_t payload_buf[256];
               read_exact(upstream_fd, payload_buf, payload_len);
            }
         }
      }
      /* Write response */
      write(upstream_fd, response, strlen(response));
   }
   return NULL;
}

static void test_payload_forwarding(void)
{
   fprintf(stderr, "test_payload_forwarding: starting\n");
   const uint8_t payload[] = { 'a', 'b', 'c' };
   const size_t payload_len = sizeof(payload);

   int upstream[2];
   int client_pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, upstream) == 0);
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client_pair) == 0);

   fprintf(stderr, "test_payload_forwarding: preparing proxy\n");
   prepare_proxy(upstream[1], "demo");

   struct conn_state client_state;
   client_state.sock = client_pair[1];
   client_state.is_authenticated = 1;

   /* Start thread to handle upstream side (read request, write response) */
   pthread_t responder_thread;
   assert(pthread_create(&responder_thread, NULL, upstream_responder, &upstream[0]) == 0);

   fprintf(stderr, "test_payload_forwarding: calling proxy_pass_call_cmd_ex\n");
   int result = proxy_pass_call_cmd_ex(
      "demo",
      "method",
      "",
      payload,
      payload_len,
      "JSON",
      &client_state);
   fprintf(stderr, "test_payload_forwarding: proxy_pass_call_cmd_ex returned %d\n", result);
   
   pthread_join(responder_thread, NULL);

   assert(result == 1);

   fprintf(stderr, "test_payload_forwarding: reading from client\n");
   char response_buffer[16];
   read_exact(client_pair[0], (uint8_t *)response_buffer, strlen("200 OK\n"));
   response_buffer[strlen("200 OK\n")] = '\0';
   assert(strcmp(response_buffer, "200 OK\n") == 0);

   fprintf(stderr, "test_payload_forwarding: closing sockets\n");
   close(upstream[0]);
   close(upstream[1]);
   close(client_pair[0]);
   close(client_pair[1]);
   fprintf(stderr, "test_payload_forwarding: completed\n");
}

static void *upstream_responder_legacy(void *arg)
{
   int upstream_fd = *(int *)arg;
   const char *response = "200 OK\n";
   char request[256];
   ssize_t n;
   
   n = read(upstream_fd, request, sizeof(request) - 1);
   if (n > 0) {
      request[n] = '\0';
      write(upstream_fd, response, strlen(response));
   }
   return NULL;
}

static void test_legacy_forwarding(void)
{
   fprintf(stderr, "test_legacy_forwarding: starting\n");

   int upstream[2];
   int client_pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, upstream) == 0);
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client_pair) == 0);

   fprintf(stderr, "test_legacy_forwarding: preparing proxy\n");
   prepare_proxy(upstream[1], "demo");

   struct conn_state client_state;
   client_state.sock = client_pair[1];
   client_state.is_authenticated = 1;

   /* Start thread to handle upstream side */
   pthread_t responder_thread;
   assert(pthread_create(&responder_thread, NULL, upstream_responder_legacy, &upstream[0]) == 0);

   fprintf(stderr, "test_legacy_forwarding: calling proxy_pass_call_cmd_ex\n");
   int result = proxy_pass_call_cmd_ex(
      "demo",
      "method",
      "Zm9v",
      NULL,
      0,
      NULL,
      &client_state);
   fprintf(stderr, "test_legacy_forwarding: proxy_pass_call_cmd_ex returned %d\n", result);
   
   pthread_join(responder_thread, NULL);
   
   assert(result == 1);

   fprintf(stderr, "test_legacy_forwarding: reading from client\n");
   char response_buffer[16];
   read_exact(client_pair[0], (uint8_t *)response_buffer, strlen("200 OK\n"));
   response_buffer[strlen("200 OK\n")] = '\0';
   assert(strcmp(response_buffer, "200 OK\n") == 0);

   fprintf(stderr, "test_legacy_forwarding: closing sockets\n");
   close(upstream[0]);
   close(upstream[1]);
   close(client_pair[0]);
   close(client_pair[1]);
   fprintf(stderr, "test_legacy_forwarding: completed\n");
}

int main(void)
{
   fprintf(stderr, "proxy_payload_test: starting\n");
   test_payload_forwarding();
   fprintf(stderr, "proxy_payload_test: test_payload_forwarding passed\n");
   test_legacy_forwarding();
   fprintf(stderr, "proxy_payload_test: test_legacy_forwarding passed\n");
   reset_proxy_state();
   fprintf(stderr, "proxy_payload_test: all tests passed\n");
   return 0;
}
