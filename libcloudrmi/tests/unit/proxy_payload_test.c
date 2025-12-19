#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static void test_payload_forwarding(void)
{
   const uint8_t payload[] = { 'a', 'b', 'c' };
   const size_t payload_len = sizeof(payload);
   const char expected[] = "CALL demo method PAYLOAD JSON 3\nabc";
   const size_t expected_len = sizeof(expected) - 1;
   const char upstream_response[] = "200 OK\n";

   int upstream[2];
   int client_pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, upstream) == 0);
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client_pair) == 0);

   prepare_proxy(upstream[1], "demo");

   struct conn_state client_state;
   client_state.sock = client_pair[1];
   client_state.is_authenticated = 1;

   assert(write(upstream[0], upstream_response, strlen(upstream_response)) == (ssize_t)strlen(upstream_response));

   int result = proxy_pass_call_cmd_ex(
      "demo",
      "method",
      "",
      payload,
      payload_len,
      "JSON",
      &client_state);
   assert(result == 1);

   uint8_t buffer[64];
   read_exact(upstream[0], buffer, expected_len);
   assert(memcmp(buffer, expected, expected_len) == 0);

   char response_buffer[16];
   read_exact(client_pair[0], (uint8_t *)response_buffer, strlen(upstream_response));
   response_buffer[strlen(upstream_response)] = '\0';
   assert(strcmp(response_buffer, upstream_response) == 0);

   close(upstream[0]);
   close(upstream[1]);
   close(client_pair[0]);
   close(client_pair[1]);
}

static void test_legacy_forwarding(void)
{
   const char expected[] = "CALL demo method Zm9v\n";
   const size_t expected_len = sizeof(expected) - 1;
   const char upstream_response[] = "200 OK\n";

   int upstream[2];
   int client_pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, upstream) == 0);
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client_pair) == 0);

   prepare_proxy(upstream[1], "demo");

   struct conn_state client_state;
   client_state.sock = client_pair[1];
   client_state.is_authenticated = 1;

   assert(write(upstream[0], upstream_response, strlen(upstream_response)) == (ssize_t)strlen(upstream_response));

   int result = proxy_pass_call_cmd_ex(
      "demo",
      "method",
      "Zm9v",
      NULL,
      0,
      NULL,
      &client_state);
   assert(result == 1);

   uint8_t buffer[64];
   read_exact(upstream[0], buffer, expected_len);
   assert(memcmp(buffer, expected, expected_len) == 0);

   char response_buffer[16];
   read_exact(client_pair[0], (uint8_t *)response_buffer, strlen(upstream_response));
   response_buffer[strlen(upstream_response)] = '\0';
   assert(strcmp(response_buffer, upstream_response) == 0);

   close(upstream[0]);
   close(upstream[1]);
   close(client_pair[0]);
   close(client_pair[1]);
}

int main(void)
{
   test_payload_forwarding();
   test_legacy_forwarding();
   reset_proxy_state();
   return 0;
}
