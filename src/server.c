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
#ifdef PROXY_SUPPORT
#include "proxy.h"
#endif
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

/* Prefork worker tracking */
static pid_t *prefork_worker_pids = NULL;
static int prefork_worker_count = 0;
static volatile sig_atomic_t prefork_shutdown_requested = 0;

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
   struct sigaction sa_ign;

   sa.sa_handler = shutdown_handler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = 0;

   sigaction(SIGINT, &sa, NULL);
   sigaction(SIGTERM, &sa, NULL);

   /* Prevent process termination on write() to a closed socket (e.g. readiness probes). */
   sa_ign.sa_handler = SIG_IGN;
   sigemptyset(&sa_ign.sa_mask);
   sa_ign.sa_flags = 0;
   sigaction(SIGPIPE, &sa_ign, NULL);
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

/* Prefork mode: find worker slot by PID */
static int prefork_find_worker_slot(pid_t pid)
{
   for (int i = 0; i < prefork_worker_count; i++) {
      if (prefork_worker_pids[i] == pid)
         return i;
   }
   return -1;
}

/* Prefork mode: spawn a single worker */
static pid_t prefork_spawn_worker(int listen_fd, int is_unix_socket, int worker_index)
{
   pid_t pid = fork();

   if (pid < 0) {
      log_unexpected_event(LOG_ERR, "prefork-spawn-failed worker=%d errno=%d", worker_index, errno);
      return -1;
   }

   if (pid == 0) {
      /* Worker process */
      struct sigaction sa_ign, sa_dfl;

      /* Ignore SIGPIPE in worker */
      sa_ign.sa_handler = SIG_IGN;
      sigemptyset(&sa_ign.sa_mask);
      sa_ign.sa_flags = 0;
      sigaction(SIGPIPE, &sa_ign, NULL);

      /* Reset SIGCHLD to default - worker may spawn grandchildren and uses
       * explicit waitpid. Do NOT let parent's reaper interfere. */
      sa_dfl.sa_handler = SIG_DFL;
      sigemptyset(&sa_dfl.sa_mask);
      sa_dfl.sa_flags = 0;
      sigaction(SIGCHLD, &sa_dfl, NULL);

      /* Worker needs to handle SIGTERM/SIGINT for clean shutdown */
      sa_dfl.sa_handler = shutdown_handler;
      sigemptyset(&sa_dfl.sa_mask);
      sa_dfl.sa_flags = 0;
      sigaction(SIGTERM, &sa_dfl, NULL);
      sigaction(SIGINT, &sa_dfl, NULL);

#ifdef PROXY_SUPPORT
      /* In prefork mode with proxy, each worker must have its own upstream pool.
       * Reset and initialize connections before accepting clients.
       * Prefork mode pre-warms spare connections for better performance. */
      if (var_proxy) {
         proxy_reset_after_fork();
         proxy_init_connections_ex(1);  /* prewarm_spares = true */
      }
#endif

      log_event(LOG_INFO, "worker-start pid=%d worker=%d", (int)getpid(), worker_index);

      /* Worker accept loop - does NOT fork per connection */
      while (server_should_run) {
         struct sockaddr_storage cli_addr;
         socklen_t clilen = is_unix_socket ? sizeof(struct sockaddr_un) : sizeof(struct sockaddr_in);
         int newsockfd;

         newsockfd = accept(listen_fd, (struct sockaddr *)&cli_addr, &clilen);

         if (newsockfd < 0) {
            if (!server_should_run)
               break;
            if (errno == EINTR)
               continue;
            /* Socket was closed/shutdown by parent - exit cleanly */
            if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK) {
               DEBUG_LOG("worker-socket-closed pid=%d errno=%d", (int)getpid(), errno);
               break;
            }
            log_unexpected_event(LOG_ERR, "worker-accept-failed pid=%d errno=%d", (int)getpid(), errno);
            continue;
         }

         /* Handle connection directly - no fork in prefork mode */
         handle_connection(newsockfd, is_unix_socket);
         close(newsockfd);
      }

      DEBUG_LOG("worker-exit pid=%d worker=%d", (int)getpid(), worker_index);
      _exit(0);
   }

   /* Parent: log worker spawn */
   log_event(LOG_INFO, "prefork-spawned worker=%d pid=%d", worker_index, (int)pid);
   return pid;
}

/* Prefork mode: SIGCHLD handler that respawns dead workers */
static void prefork_sigchld_handler(int signo)
{
   (void)signo;

   if (prefork_shutdown_requested)
      return;

   pid_t pid;
   int status;

   while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      int slot = prefork_find_worker_slot(pid);
      if (slot >= 0) {
         /* Worker died - mark slot as empty, will respawn in main loop */
         prefork_worker_pids[slot] = 0;
      }
   }
}

/* Prefork mode: shutdown handler */
static void prefork_shutdown_handler(int signo)
{
   (void)signo;

   prefork_shutdown_requested = 1;
   server_should_run = 0;

   /* Close the listen socket to help workers exit accept() faster */
   if (server_socket_fd >= 0) {
      shutdown(server_socket_fd, SHUT_RDWR);
   }
}

/* Prefork mode: install signal handlers for parent */
static void install_prefork_handlers()
{
   struct sigaction sa_chld, sa_term, sa_ign;

   /* SIGCHLD for reaping and respawning */
   sa_chld.sa_handler = prefork_sigchld_handler;
   sigemptyset(&sa_chld.sa_mask);
   sa_chld.sa_flags = SA_RESTART;
   sigaction(SIGCHLD, &sa_chld, NULL);

   /* Shutdown handlers */
   sa_term.sa_handler = prefork_shutdown_handler;
   sigemptyset(&sa_term.sa_mask);
   sa_term.sa_flags = 0;
   sigaction(SIGTERM, &sa_term, NULL);
   sigaction(SIGINT, &sa_term, NULL);

   /* Ignore SIGPIPE */
   sa_ign.sa_handler = SIG_IGN;
   sigemptyset(&sa_ign.sa_mask);
   sa_ign.sa_flags = 0;
   sigaction(SIGPIPE, &sa_ign, NULL);
}

/* Prefork mode: terminate all workers */
static void prefork_terminate_workers()
{
   int i;

   for (i = 0; i < prefork_worker_count; i++) {
      if (prefork_worker_pids[i] > 0) {
         kill(prefork_worker_pids[i], SIGTERM);
      }
   }

   /* Wait for all workers to exit */
   for (i = 0; i < prefork_worker_count; i++) {
      if (prefork_worker_pids[i] > 0) {
         int status;
         pid_t pid = waitpid(prefork_worker_pids[i], &status, 0);
         if (pid > 0) {
            log_event(LOG_INFO, "prefork-reaped worker=%d pid=%d", i, (int)pid);
         }
         prefork_worker_pids[i] = 0;
      }
   }
}

/* Prefork mode: respawn any dead workers */
static void prefork_respawn_workers(int listen_fd, int is_unix_socket)
{
   for (int i = 0; i < prefork_worker_count; i++) {
      if (prefork_worker_pids[i] == 0) {
         pid_t new_pid = prefork_spawn_worker(listen_fd, is_unix_socket, i);
         if (new_pid > 0) {
            prefork_worker_pids[i] = new_pid;
            log_event(LOG_INFO, "prefork-respawned worker=%d pid=%d", i, (int)new_pid);
         }
      }
   }
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

   /*
    * Note: Proxy mode works correctly with fork-per-connection because:
    * - Parent process never opens upstream connections (proxy_start only parses config)
    * - Each forked child calls proxy_reset_after_fork() + proxy_init_connections()
    *   to establish its own isolated upstream connection pool
    * - Child handles one connection then exits, so no pool sharing issues
    *
    * For better performance with proxy, consider worker_model=prefork which
    * reuses workers across connections.
    */

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

#ifdef PROXY_SUPPORT
      /* Reset and initialize proxy connections in this child process.
       * Each child gets isolated upstream pool. We must fetch interfaces
       * before handling the connection to know how to route CALL commands.
       * In fork mode, we only fetch interfaces (no spare connection pre-warming). */
      if (var_proxy) {
         proxy_reset_after_fork();
         proxy_init_connections();  /* Uses worker_model to decide prewarm */
      }
#endif

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

   /* Log startup info */
   log_event(LOG_INFO, "server-start worker_model=%s workers=%d pid=%d",
      var_worker_model == WORKER_MODEL_PREFORK ? "prefork" : "fork",
      var_worker_model == WORKER_MODEL_PREFORK ? var_prefork_workers : 0,
      (int)getpid());

   if (var_worker_model == WORKER_MODEL_PREFORK) {
      /* Prefork mode: spawn workers before accepting connections */
      int i;

      prefork_worker_count = var_prefork_workers;
      prefork_worker_pids = calloc(prefork_worker_count, sizeof(pid_t));
      if (prefork_worker_pids == NULL)
         die_err("Error: Failed to allocate prefork worker array");

      /* Install prefork-specific signal handlers */
      install_prefork_handlers();

      /* Spawn initial workers */
      for (i = 0; i < prefork_worker_count; i++) {
         pid_t pid = prefork_spawn_worker(sockfd, use_unix_socket, i);
         if (pid > 0) {
            prefork_worker_pids[i] = pid;
         } else {
            prefork_worker_pids[i] = 0;
         }
      }

      /* Parent does NOT accept connections in prefork mode.
       * Just wait for signals and respawn dead workers.
       * Use a simple polling loop with short sleeps to check for:
       * - Shutdown signals (flags set by handler)
       * - Dead workers that need respawning (via SIGCHLD -> flag in handler) */
      while (!prefork_shutdown_requested) {
         struct timespec ts = {0, 100000000};  /* 100ms */
         nanosleep(&ts, NULL);  /* Interruptible by signals */

         if (prefork_shutdown_requested)
            break;

         /* Respawn any dead workers */
         prefork_respawn_workers(sockfd, use_unix_socket);
      }

      /* Shutdown: terminate all workers */
      log_event(LOG_INFO, "prefork-shutdown pid=%d", (int)getpid());
      prefork_terminate_workers();

      free(prefork_worker_pids);
      prefork_worker_pids = NULL;
      prefork_worker_count = 0;

      cleanup_listen_socket();
      return 0;
   }

   /* Fork-per-connection mode (default) */
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
   const char *model = var_worker_model == WORKER_MODEL_PREFORK ? "prefork" : "fork";

   /* Only parent process can enumerate workers. Workers report their model but empty list. */
   if (var_worker_model == WORKER_MODEL_PREFORK && getpid() == server_owner_pid && prefork_worker_pids != NULL) {
      char workers_json[2048];
      size_t offset = 0;
      int first = 1;
      int active_count = 0;

      offset += snprintf(workers_json + offset, sizeof(workers_json) - offset, "[");

      for (int i = 0; i < prefork_worker_count && offset < sizeof(workers_json) - 50; i++) {
         if (prefork_worker_pids[i] > 0) {
            offset += snprintf(workers_json + offset, sizeof(workers_json) - offset,
               "%s{\"index\":%d,\"pid\":%d}",
               first ? "" : ",", i, (int)prefork_worker_pids[i]);
            first = 0;
            active_count++;
         }
      }

      offset += snprintf(workers_json + offset, sizeof(workers_json) - offset, "]");

      /* Include legacy fields for backwards compatibility */
      stream_writeln(sock,
         "200 {\"worker_model\":\"%s\",\"workers\":%s,\"reuse_enabled\":false,\"total_spare\":%d,\"listed\":%d,\"trimmed\":false}",
         model, workers_json, active_count, active_count);
   } else {
      /* Fork mode or worker process: include legacy fields for backwards compatibility */
      stream_writeln(sock,
         "200 {\"worker_model\":\"%s\",\"workers\":[],\"reuse_enabled\":false,\"total_spare\":0,\"listed\":0,\"trimmed\":false}",
         model);
   }
}
