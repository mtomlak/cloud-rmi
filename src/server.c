#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>  // good for getpid()
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <stdarg.h>
#include <errno.h>
#include "cloud-rmi.h"
#include "config.h"
#include <sys/wait.h>
#include <time.h>
//#include <syscall.h>

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define IFACE_NAME_MAX 384
#define IFACE_SCAN_FMT "%" STR(IFACE_NAME_MAX) "[^\\t\\n# ] %" STR(BUF_SZ) "[^\\t\\n ]\n"

/**
 * Provides TCP server functionality, handles multiple connections by forking.
 *
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license BSD
 */

static int server_socket_fd = -1;
static char server_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)] = {0};
static volatile sig_atomic_t server_should_run = 1;
static int server_use_unix_socket = 0;
static pid_t server_owner_pid = 0;

static void cleanup_listen_socket()
{
   if (server_owner_pid != 0 && getpid() != server_owner_pid)
      return;

   log_unexpected_event(
      LOG_INFO,
      "server-cleanup pid=%i owner=%i unix=%i",
      (int)getpid(),
      (int)server_owner_pid,
      server_use_unix_socket);

   server_should_run = 0;

   if (server_socket_fd >= 0) {
      shutdown(server_socket_fd, SHUT_RDWR);
      close(server_socket_fd);
      server_socket_fd = -1;
   }

   if (server_socket_path[0]) {
      unlink(server_socket_path);
      server_socket_path[0] = '\0';
   }
}

static void shutdown_handler(int signo)
{
   (void)signo;

   cleanup_listen_socket();
}

static void install_shutdown_handlers()
{
   struct sigaction sa;

   sa.sa_handler = shutdown_handler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = 0;

   sigaction(SIGINT, &sa, NULL);
   sigaction(SIGTERM, &sa, NULL);
}

static void reap_children_handler(int signo)
{
   (void)signo;

   /* Reap all exited children to avoid zombies. */
   while (waitpid(-1, NULL, WNOHANG) > 0)
      ;
}

static void install_reaper()
{
   struct sigaction sa;

   sa.sa_handler = reap_children_handler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_RESTART;

   sigaction(SIGCHLD, &sa, NULL);
}

/**
 * Creates a child proccess to handle new client connection.
 * The child proccess behaves like a forked proccess, having its own memory
 * and file descriptors inherited from the parent proccess.
 * New proccess is created by fork() POSIX syscall.
 * 
 * @param  sock        Client socket (the socket created with accept())
 * @param  server_sock Server socket (the socket created with bind())
 * @return             1 on success or 0 on failure
 */
int create_client_handler_proc(int sock, int server_sock, int is_unix_socket)
{
   int pid = 0;

   DEBUG_LOG("Creating new child proccess with POSIX fork() socket #%i", sock);

   // start new proccess and continue execution below
   pid = fork();

   if (pid < 0)
      die_err("Error: fork() failed");

   if (pid == 0) {   // We are in child proccess
      struct sigaction sa_default;

      /* The child may spawn grandchildren (e.g. handle_shell_call) and relies on
       * explicit waitpid calls. Reset SIGCHLD to default so the parent's reaper
       * does not interfere with child-owned lifecycles. */
      sa_default.sa_handler = SIG_DFL;
      sigemptyset(&sa_default.sa_mask);
      sa_default.sa_flags = 0;
      sigaction(SIGCHLD, &sa_default, NULL);

      DEBUG_LOG("+ Started new child with PID %i", (int) getpid());

      close(server_sock); // Parent has it's own copy now, close it
      handle_connection(sock, is_unix_socket);

      DEBUG_LOG("- Shutting down child with PID %i", (int) getpid());

      close(sock);
      _exit(0);
   }
   close(sock); // We're back in parent, child has it's own copy now
   
   return pid;
}


/**
 * Starts the server
 * 
 * @return [description]
 */
int server_start(int port_num, const char *unix_socket_path)
{
   int sockfd, newsockfd;
   socklen_t clilen;
   struct sockaddr_in serv_addr;
   struct sockaddr_un serv_addr_un;
   struct sockaddr_storage cli_addr;
   const int use_unix_socket = unix_socket_path && strlen(unix_socket_path);

   server_owner_pid = getpid();
   server_should_run = 1;
   server_socket_fd = -1;
   server_socket_path[0] = '\0';
   server_use_unix_socket = use_unix_socket;

   install_reaper();
   install_shutdown_handlers();
   atexit(cleanup_listen_socket);

   /* Save the boot timestamp */
   var_server_start_time = (int)time(NULL);

   if (use_unix_socket && strlen(unix_socket_path) >= sizeof(serv_addr_un.sun_path))
      die_err("Error: Unix socket path too long");

   /* First call to socket() function */
   sockfd = socket(use_unix_socket ? AF_UNIX : AF_INET, SOCK_STREAM, 0);

   if (sockfd < 0) {
      die_err("Error opening socket");
   }

   server_socket_fd = sockfd;

   if (use_unix_socket) {
      bzero((char *) &serv_addr_un, sizeof(serv_addr_un));
      serv_addr_un.sun_family = AF_UNIX;
      strncpy(server_socket_path, unix_socket_path, sizeof(server_socket_path) - 1);
      unlink(server_socket_path);
      strncpy(serv_addr_un.sun_path, unix_socket_path, sizeof(serv_addr_un.sun_path) - 1);

      /* Now bind the host address using bind() call.*/
      if (bind(sockfd, (struct sockaddr *) &serv_addr_un, sizeof(serv_addr_un)) < 0)
         die_err("Error: Bind failed");

      clilen = sizeof(struct sockaddr_un);
   } else {
      /* Set socket option to enable port reuse */
      if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int)) < 0)
         die_err("setsockopt(SO_REUSEADDR) failed");
      
      /* Initialize socket structure */
      bzero((char *) &serv_addr, sizeof(serv_addr));
      
      serv_addr.sin_family = AF_INET;
      serv_addr.sin_addr.s_addr = INADDR_ANY;
      serv_addr.sin_port = htons(port_num);
      
      /* Now bind the host address using bind() call.*/
      if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
         die_err("Error: Bind failed");

      clilen = sizeof(struct sockaddr_in);
   }
   
   /* Now start listening for the clients, here
    * process will go in sleep mode and will wait
    * for the incoming connection
    */
   
   listen(sockfd, 5);
   
   while (server_should_run) {
      clilen = use_unix_socket ? sizeof(struct sockaddr_un) : sizeof(struct sockaddr_in);
      /* Wait for new connections */
      newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
        
      if (newsockfd < 0) {
         if (!server_should_run)
            break;

         die_err("Error: Accept failed");
      }

      /* Create new child proccess to handle new connection */
      create_client_handler_proc(newsockfd, sockfd, server_use_unix_socket);
      //   die_err("Error: Failed creating new child proccess");
      
#ifdef PROXY_SUPPORT
      /* Check state of proxy connections in main loop */
      if (var_proxy) {
         proxy_all_keepalive_conns();
         proxy_all_check_spare_conns();
      }
#endif

      /* Collect finished child proccesses */
      waitpid(-1, NULL, WNOHANG);

   } /* end of while */

   cleanup_listen_socket();

   /* Reap any remaining children before exiting to avoid zombies when the
    * parent process is terminated by the test harness or the init system.
    */
   while (1) {
      pid_t reap = waitpid(-1, NULL, 0);
      if (reap <= 0) {
         if (reap < 0 && errno == EINTR)
            continue;
         break;
      }
   }

   return 0;
}

struct server_iface_entry *new_server_iface_entry(char *iface_name, char *file_name, struct server_iface_entry *next, struct server_iface_entry *prev)
{
   struct server_iface_entry *result = malloc(sizeof(struct server_iface_entry));

   result->iface = strdup(iface_name);
   result->file = strdup(file_name);
   result->next = next;
   result->prev = prev;

   //char *proxy_ip = malloc(sizeof(char) * (21));
   return result;
}

struct server_iface_entry *server_get_last_iface_entry()
{
   struct server_iface_entry *cur;

   if (var_server_iface_map == NULL)
      return NULL;

   cur = var_server_iface_map;

   while (cur->next != NULL) {
      cur = cur->next;
   }

   return cur;
}

struct server_iface_entry *server_get_iface_entry(char *iface_name)
{
   struct server_iface_entry *cur;

   if (var_server_iface_map == NULL)
      return NULL;

   cur = var_server_iface_map;

   while (cur != NULL) {
      if (strcasecmp(iface_name, cur->iface) == 0)
         return cur;

      cur = cur->next;
   }

   return NULL;
}

void server_add_iface_entry(char *iface_name, char *file_name)
{
   if (var_server_iface_map == NULL) // adding first item
      var_server_iface_map = new_server_iface_entry(
         iface_name, 
         file_name, 
         NULL, 
         NULL
         );
   else
      server_get_last_iface_entry()->next = new_server_iface_entry(
         iface_name, 
         file_name, 
         NULL, 
         server_get_last_iface_entry()
         );
}

void server_destroy_iface_entry(struct server_iface_entry *entry)
{
   if (entry == NULL)
      return;

   if (entry->prev == NULL)   // it's a first entry
      var_server_iface_map = entry->next;
   else
      entry->prev->next = entry->next;

   if (entry->next != NULL)  // it's not a last entry
      entry->next->prev = entry->prev;

   free(entry->iface);
   free(entry->file);
   free(entry);
}

void server_wipe_iface_entries()
{
   struct server_iface_entry *cur;

   cur = server_get_last_iface_entry();

   while (cur != NULL) {
      server_destroy_iface_entry(cur);
      cur = server_get_last_iface_entry();
   }
}

int read_iface_config(char *config_file)
{
   int count = 0;
   FILE *fp;
   char line[BUF_SZ + 1];
   char iface[BUF_SZ + 1];
   char file[BUF_SZ + 1];
   char iface_short[BUF_SZ + 1];
   char file_short[BUF_SZ + 1];

   bzero(iface, sizeof(iface));
   bzero(file, sizeof(file));
   bzero(iface_short, sizeof(iface_short));
   bzero(file_short, sizeof(file_short));
   bzero(line, sizeof(line));

   if((fp = fopen(config_file, "r")) == NULL) {
      return 0;
   }

   // Wipe existing interface entries (in case config gets reloaded or like)
   server_wipe_iface_entries();

   DEBUG_LOG("Using base namespace \"%s\"", var_script_base_namespace ? var_script_base_namespace : "(null)");
   DEBUG_LOG("Using script exec dir \"%s\"", var_script_exec_dir ? var_script_exec_dir : "(null)");

   while (fgets(line, sizeof(line), fp) != NULL) {
      if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
         continue;

      if (sscanf(line, "%" STR(IFACE_NAME_MAX) "s %" STR(BUF_SZ) "s", iface_short, file_short) != 2) {
         log_unexpected_event(LOG_WARNING, "Skipping unparsable interface map line in %s", config_file);
         continue;
      }

      iface[0] = '\0';
      file[0] = '\0';

      // Complete relative namespace names and file names
      if (iface_short[0] == '\\') {
         int iface_written = snprintf(iface, BUF_SZ + 1, "%s", iface_short);
         if (iface_written < 0 || iface_written >= BUF_SZ + 1) {
            log_event(LOG_ERR, "Skipping interface with overlong name");
            continue;
         }
      } else {
         int iface_written = snprintf(iface, BUF_SZ + 1, "%s\\%s", var_script_base_namespace, iface_short);
         if (iface_written < 0 || iface_written >= BUF_SZ + 1) {
            log_event(LOG_ERR, "Skipping interface with overlong namespaced name");
            continue;
         }
      }

      if (file_short[0] == '/') {
         int file_written = snprintf(file, BUF_SZ + 1, "%s", file_short);
         if (file_written < 0 || file_written >= BUF_SZ + 1) {
            log_event(LOG_ERR, "Skipping interface with overlong absolute path");
            continue;
         }
      } else {
         int file_written = snprintf(file, BUF_SZ + 1, "%s/%s", var_script_exec_dir, file_short);
         if (file_written < 0 || file_written >= BUF_SZ + 1) {
            log_event(LOG_ERR, "Skipping interface with overlong resolved path");
            continue;
         }
      }

      server_add_iface_entry(iface, file);
      DEBUG_LOG("Mapped interface %s -> %s", iface, file);
      count++;
   }

   fclose(fp);

   DEBUG_LOG("Loaded %d interface map entries from %s", count, config_file);
   if (count == 0)
      log_unexpected_event(LOG_WARNING, "Interface map empty after load: %s", config_file);

   return 1;
}

void server_send_stats(int sock)
{
   stream_writeln(sock, "200 {\"worker_model\":\"fork\",\"workers\":[],\"reuse_enabled\":false,\"total_spare\":0,\"listed\":0,\"trimmed\":false}");
}
