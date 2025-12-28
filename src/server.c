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
#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "cloud-rmi.h"
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
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

/* Shared snapshot for worker metadata to make WORKERS introspection available in worker processes. */
typedef struct {
   pid_t pid;
   int idle;
   int terminating;
} worker_snapshot_slot_t;

typedef struct {
   int initialized;
   pid_t owner_pid;
   worker_model_t model;
   int prefork_workers;
   int adaptive_min_workers;
   int adaptive_max_workers;
   int adaptive_spare_workers;
   int active_workers;
   int idle_workers;
   worker_snapshot_slot_t slots[ADAPTIVE_WORKERS_MAX];
} worker_snapshot_t;

static worker_snapshot_t *worker_snapshot = NULL;

/* Prefork worker tracking */
static pid_t *prefork_worker_pids = NULL;
static int prefork_worker_count = 0;
static volatile sig_atomic_t worker_pool_shutdown_requested = 0;

typedef struct {
   pid_t pid;
   int control_fd;      /* Read end of worker control pipe */
   int idle_reported;   /* Whether worker reported idle */
   int terminating;     /* Whether worker is being trimmed */
} adaptive_worker_slot_t;

static adaptive_worker_slot_t *adaptive_workers = NULL;
static int adaptive_worker_capacity = 0;
static int adaptive_idle_workers = 0;

/* Forward declarations for adaptive helpers */
static int adaptive_find_worker_slot(pid_t pid);
static void adaptive_clear_slot(int index);

static void worker_snapshot_recalc(void)
{
   if (worker_snapshot == NULL)
      return;

   int active = 0;
   int idle = 0;

   for (int i = 0; i < ADAPTIVE_WORKERS_MAX; i++) {
      if (worker_snapshot->slots[i].pid > 0) {
         active++;
         if (worker_snapshot->slots[i].idle)
            idle++;
      }
   }

   worker_snapshot->active_workers = active;
   worker_snapshot->idle_workers = idle;
}

static void worker_snapshot_init(void)
{
   if (worker_snapshot != NULL)
      return;

   worker_snapshot = mmap(NULL, sizeof(worker_snapshot_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
   if (worker_snapshot == MAP_FAILED) {
      worker_snapshot = NULL;
      log_unexpected_event(LOG_WARNING, "worker-snapshot-init-failed errno=%d", errno);
      return;
   }

   memset(worker_snapshot, 0, sizeof(worker_snapshot_t));
}

static void worker_snapshot_seed_config(void)
{
   if (worker_snapshot == NULL)
      return;

   worker_snapshot->initialized = 1;
   worker_snapshot->owner_pid = server_owner_pid;
   worker_snapshot->model = var_worker_model;
   worker_snapshot->prefork_workers = var_prefork_workers;
   worker_snapshot->adaptive_min_workers = var_adaptive_min_workers;
   worker_snapshot->adaptive_max_workers = var_adaptive_max_workers;
   worker_snapshot->adaptive_spare_workers = var_adaptive_spare_workers;
   worker_snapshot_recalc();
}

void worker_snapshot_refresh_config(void)
{
   worker_snapshot_seed_config();
}

int worker_snapshot_read(worker_snapshot_view_t *out)
{
   if (out == NULL)
      return 0;

   memset(out, 0, sizeof(*out));

   if (worker_snapshot == NULL || !worker_snapshot->initialized)
      goto fallback;

   out->available = 1;
   out->model = worker_snapshot->model;
   out->prefork_workers = worker_snapshot->prefork_workers;
   out->adaptive_min_workers = worker_snapshot->adaptive_min_workers;
   out->adaptive_max_workers = worker_snapshot->adaptive_max_workers;
   out->adaptive_spare_workers = worker_snapshot->adaptive_spare_workers;
   out->active_workers = worker_snapshot->active_workers;
   out->idle_workers = worker_snapshot->idle_workers;
   return 1;

fallback:
   if (getpid() == server_owner_pid) {
      int active = 0;

      out->available = 1;
      out->model = var_worker_model;
      out->prefork_workers = var_prefork_workers;
      out->adaptive_min_workers = var_adaptive_min_workers;
      out->adaptive_max_workers = var_adaptive_max_workers;
      out->adaptive_spare_workers = var_adaptive_spare_workers;

      if (var_worker_model == WORKER_MODEL_PREFORK && prefork_worker_pids != NULL) {
         for (int i = 0; i < prefork_worker_count; i++) {
            if (prefork_worker_pids[i] > 0)
               active++;
         }
      } else if (var_worker_model == WORKER_MODEL_ADAPTIVE && adaptive_workers != NULL) {
         for (int i = 0; i < adaptive_worker_capacity; i++) {
            if (adaptive_workers[i].pid > 0)
               active++;
         }
         out->idle_workers = adaptive_idle_workers;
      }

      out->active_workers = active;
      return 1;
   }

   return 0;
}

static void worker_snapshot_set_slot(int index, pid_t pid)
{
   if (worker_snapshot == NULL || index < 0 || index >= ADAPTIVE_WORKERS_MAX)
      return;

   worker_snapshot->slots[index].pid = pid;
   worker_snapshot->slots[index].idle = 0;
   worker_snapshot->slots[index].terminating = 0;
   worker_snapshot_recalc();
}

static void worker_snapshot_set_idle(int index, int idle)
{
   if (worker_snapshot == NULL || index < 0 || index >= ADAPTIVE_WORKERS_MAX)
      return;

   worker_snapshot->slots[index].idle = idle ? 1 : 0;
   worker_snapshot_recalc();
}

static void worker_snapshot_set_terminating(int index, int terminating)
{
   if (worker_snapshot == NULL || index < 0 || index >= ADAPTIVE_WORKERS_MAX)
      return;

   worker_snapshot->slots[index].terminating = terminating ? 1 : 0;
}

static void cleanup_listen_socket()
{
   int is_owner = (server_owner_pid == 0 || getpid() == server_owner_pid);

   if (!is_owner) {
      server_should_run = 0;

      if (server_socket_fd >= 0) {
         close(server_socket_fd);
         server_socket_fd = -1;
      }

      DEBUG_LOG("! worker-cleanup pid=%i owner=%i", (int)getpid(), (int)server_owner_pid);
      return;
   }

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
static void worker_send_state(int control_fd, char state)
{
   if (control_fd < 0)
      return;

   /* Best-effort; failures are ignored to avoid interfering with hot path. */
   (void)write(control_fd, &state, 1);
}

static pid_t prefork_spawn_worker(int listen_fd, int is_unix_socket, int worker_index, int control_fd_write, int control_fd_read, int adaptive_mode)
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

      if (control_fd_read >= 0)
         close(control_fd_read);

#ifdef PROXY_SUPPORT
      /* In prefork mode with proxy, each worker must have its own upstream pool.
       * Reset proxy state; connections are initialized lazily per request to
       * avoid blocking accept() when the upstream is not yet ready. */
      if (var_proxy) {
         proxy_reset_after_fork();
      }
#endif

      log_event(LOG_INFO, "worker-start pid=%d worker=%d", (int)getpid(), worker_index);

      int reported_idle = 0;

      /* Worker accept loop - does NOT fork per connection */
      while (server_should_run) {
         struct sockaddr_storage cli_addr;
         socklen_t clilen = is_unix_socket ? sizeof(struct sockaddr_un) : sizeof(struct sockaddr_in);
         int newsockfd;

         if (adaptive_mode && !reported_idle) {
            worker_send_state(control_fd_write, 'I');
            reported_idle = 1;
         }

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

         if (adaptive_mode && reported_idle) {
            worker_send_state(control_fd_write, 'B');
            reported_idle = 0;
         }

         /* Handle connection directly - no fork in prefork mode */
         handle_connection(newsockfd, is_unix_socket);
         close(newsockfd);
      }

      if (adaptive_mode && reported_idle) {
         worker_send_state(control_fd_write, 'B');
      }
      if (control_fd_write >= 0)
         close(control_fd_write);

      DEBUG_LOG("worker-exit pid=%d worker=%d", (int)getpid(), worker_index);
      _exit(0);
   }

   /* Parent: log worker spawn */
   log_event(LOG_INFO, "%s-spawned worker=%d pid=%d",
      adaptive_mode ? "adaptive" : "prefork", worker_index, (int)pid);
   DEBUG_LOG("+ %s-worker-spawn worker=%d pid=%d",
      adaptive_mode ? "adaptive" : "prefork", worker_index, (int)pid);
   worker_snapshot_set_slot(worker_index, pid);
   return pid;
}

/* Prefork mode: SIGCHLD handler that respawns dead workers */
static void prefork_sigchld_handler(int signo)
{
   (void)signo;

   if (worker_pool_shutdown_requested)
      return;

   pid_t pid;
   int status;

   while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      int slot = prefork_find_worker_slot(pid);
      if (slot >= 0) {
         /* Worker died - mark slot as empty, will respawn in main loop */
         prefork_worker_pids[slot] = 0;
         worker_snapshot_set_slot(slot, 0);
      }
   }
}

/* Prefork mode: shutdown handler */
static void worker_pool_shutdown_handler(int signo)
{
   (void)signo;

   worker_pool_shutdown_requested = 1;
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
   sa_term.sa_handler = worker_pool_shutdown_handler;
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

static void adaptive_sigchld_handler(int signo)
{
   (void)signo;

   if (worker_pool_shutdown_requested)
      return;

   pid_t pid;
   int status;

   while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      int slot = adaptive_find_worker_slot(pid);
      if (slot >= 0) {
         adaptive_clear_slot(slot);
      }
   }
}

static void install_adaptive_handlers()
{
   struct sigaction sa_chld, sa_term, sa_ign;

   sa_chld.sa_handler = adaptive_sigchld_handler;
   sigemptyset(&sa_chld.sa_mask);
   sa_chld.sa_flags = SA_RESTART;
   sigaction(SIGCHLD, &sa_chld, NULL);

   sa_term.sa_handler = worker_pool_shutdown_handler;
   sigemptyset(&sa_term.sa_mask);
   sa_term.sa_flags = 0;
   sigaction(SIGTERM, &sa_term, NULL);
   sigaction(SIGINT, &sa_term, NULL);

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
         worker_snapshot_set_slot(i, 0);
      }
   }
}

/* Prefork mode: respawn any dead workers */
static void prefork_respawn_workers(int listen_fd, int is_unix_socket)
{
   for (int i = 0; i < prefork_worker_count; i++) {
      if (prefork_worker_pids[i] == 0) {
         pid_t new_pid = prefork_spawn_worker(listen_fd, is_unix_socket, i, -1, -1, 0);
         if (new_pid > 0) {
            prefork_worker_pids[i] = new_pid;
            log_event(LOG_INFO, "prefork-respawned worker=%d pid=%d", i, (int)new_pid);
            DEBUG_LOG("+ prefork-worker-respawn worker=%d pid=%d", i, (int)new_pid);
            worker_snapshot_set_slot(i, new_pid);
         }
      }
   }
}

/* Adaptive pool helpers */
static int adaptive_find_worker_slot(pid_t pid)
{
   for (int i = 0; i < adaptive_worker_capacity; i++) {
      if (adaptive_workers[i].pid == pid)
         return i;
   }
   return -1;
}

static void adaptive_clear_slot(int index)
{
   if (index < 0 || index >= adaptive_worker_capacity)
      return;

   if (adaptive_workers[index].idle_reported && adaptive_idle_workers > 0)
      adaptive_idle_workers--;

   if (adaptive_workers[index].control_fd >= 0)
      close(adaptive_workers[index].control_fd);

   DEBUG_LOG("- adaptive-worker-clear idx=%d pid=%d idle_total=%d",
      index, (int)adaptive_workers[index].pid, adaptive_idle_workers);
   adaptive_workers[index].pid = 0;
   adaptive_workers[index].control_fd = -1;
   adaptive_workers[index].idle_reported = 0;
   adaptive_workers[index].terminating = 0;
   worker_snapshot_set_slot(index, 0);
}

static void adaptive_update_worker_state(int index, char state)
{
   if (index < 0 || index >= adaptive_worker_capacity)
      return;

   if (state == 'I') {
      if (!adaptive_workers[index].idle_reported) {
         adaptive_workers[index].idle_reported = 1;
         adaptive_idle_workers++;
         DEBUG_LOG("= adaptive-worker-idle idx=%d pid=%d idle_total=%d",
            index, (int)adaptive_workers[index].pid, adaptive_idle_workers);
         worker_snapshot_set_idle(index, 1);
      }
   } else if (state == 'B') {
      if (adaptive_workers[index].idle_reported) {
         adaptive_workers[index].idle_reported = 0;
         if (adaptive_idle_workers > 0)
            adaptive_idle_workers--;
         DEBUG_LOG("= adaptive-worker-busy idx=%d pid=%d idle_total=%d",
            index, (int)adaptive_workers[index].pid, adaptive_idle_workers);
         worker_snapshot_set_idle(index, 0);
      }
   }
}

static void adaptive_process_control_fd(int index)
{
   int fd = adaptive_workers[index].control_fd;
   char buf[64];
   ssize_t rd;

   if (fd < 0)
      return;

   while ((rd = read(fd, buf, sizeof(buf))) > 0) {
      for (ssize_t i = 0; i < rd; i++) {
         adaptive_update_worker_state(index, buf[i]);
      }
   }

   if (rd < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      log_unexpected_event(LOG_DEBUG, "adaptive-control-read-failed idx=%d errno=%d", index, errno);
   }
}

static void adaptive_poll_worker_states()
{
   if (adaptive_worker_capacity == 0)
      return;

   struct pollfd pfds[ADAPTIVE_WORKERS_MAX];
   int slot_map[ADAPTIVE_WORKERS_MAX];
   int nfds = 0;

   for (int i = 0; i < adaptive_worker_capacity && nfds < ADAPTIVE_WORKERS_MAX; i++) {
      if (adaptive_workers[i].pid > 0 && adaptive_workers[i].control_fd >= 0) {
         pfds[nfds].fd = adaptive_workers[i].control_fd;
         pfds[nfds].events = POLLIN;
         slot_map[nfds] = i;
         nfds++;
      }
   }

   if (nfds == 0)
      return;

   int ret = poll(pfds, nfds, 250);
   if (ret <= 0) {
      return;
   }

   for (int i = 0; i < nfds; i++) {
      if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
         DEBUG_LOG("- adaptive-control-closed idx=%d fd=%d revents=%d", slot_map[i], pfds[i].fd, pfds[i].revents);
         adaptive_clear_slot(slot_map[i]);
         continue;
      }

      if (pfds[i].revents & POLLIN) {
         adaptive_process_control_fd(slot_map[i]);
      }
   }
}

static int adaptive_active_workers()
{
   int count = 0;
   for (int i = 0; i < adaptive_worker_capacity; i++) {
      if (adaptive_workers[i].pid > 0)
         count++;
   }
   return count;
}

static int adaptive_find_free_slot()
{
   for (int i = 0; i < adaptive_worker_capacity; i++) {
      if (adaptive_workers[i].pid == 0 && !adaptive_workers[i].terminating)
         return i;
   }
   return -1;
}

static void adaptive_trim_idle_worker()
{
   for (int i = 0; i < adaptive_worker_capacity; i++) {
      if (adaptive_workers[i].pid > 0 && adaptive_workers[i].idle_reported && !adaptive_workers[i].terminating) {
         adaptive_workers[i].terminating = 1;
         if (adaptive_workers[i].idle_reported && adaptive_idle_workers > 0) {
            adaptive_idle_workers--;
            adaptive_workers[i].idle_reported = 0;
         }
         kill(adaptive_workers[i].pid, SIGTERM);
         log_event(LOG_INFO, "adaptive-trim worker=%d pid=%d", i, (int)adaptive_workers[i].pid);
         DEBUG_LOG("- adaptive-trim-request worker=%d pid=%d idle_total=%d",
            i, (int)adaptive_workers[i].pid, adaptive_idle_workers);
         worker_snapshot_set_terminating(i, 1);
         return;
      }
   }
}

static pid_t adaptive_spawn_worker(int listen_fd, int is_unix_socket, int worker_index)
{
   int pipefd[2];

   if (pipe(pipefd) < 0) {
      log_unexpected_event(LOG_ERR, "adaptive-pipe-failed worker=%d errno=%d", worker_index, errno);
      return -1;
   }

   if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0) {
      log_unexpected_event(LOG_ERR, "adaptive-pipe-nonblock-failed worker=%d errno=%d", worker_index, errno);
   }
   fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
   fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

   pid_t pid = prefork_spawn_worker(listen_fd, is_unix_socket, worker_index, pipefd[1], pipefd[0], 1);

   if (pid < 0) {
      close(pipefd[0]);
      close(pipefd[1]);
      return -1;
   }

   close(pipefd[1]);
   adaptive_workers[worker_index].pid = pid;
   adaptive_workers[worker_index].control_fd = pipefd[0];
   adaptive_workers[worker_index].idle_reported = 0;
   adaptive_workers[worker_index].terminating = 0;
   DEBUG_LOG("+ adaptive-worker-spawn idx=%d pid=%d ctrl_fd=%d capacity=%d",
      worker_index, (int)pid, pipefd[0], adaptive_worker_capacity);
   worker_snapshot_set_slot(worker_index, pid);
   return pid;
}

static void adaptive_terminate_workers()
{
   for (int i = 0; i < adaptive_worker_capacity; i++) {
      if (adaptive_workers[i].pid > 0) {
         kill(adaptive_workers[i].pid, SIGTERM);
      }
   }

   for (int i = 0; i < adaptive_worker_capacity; i++) {
      if (adaptive_workers[i].pid > 0) {
         int status;
         pid_t pid = waitpid(adaptive_workers[i].pid, &status, 0);
         if (pid > 0) {
            log_event(LOG_INFO, "adaptive-reaped worker=%d pid=%d", i, (int)pid);
         }
         adaptive_clear_slot(i);
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
   worker_pool_shutdown_requested = 0;
   worker_snapshot_init();
   worker_snapshot_refresh_config();

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
   int worker_log_count = 0;
   if (var_worker_model == WORKER_MODEL_PREFORK)
      worker_log_count = var_prefork_workers;
   else if (var_worker_model == WORKER_MODEL_ADAPTIVE)
      worker_log_count = var_adaptive_max_workers;

   log_event(LOG_INFO, "server-start worker_model=%s workers=%d pid=%d",
      worker_model_to_string(var_worker_model),
      worker_log_count,
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
         pid_t pid = prefork_spawn_worker(sockfd, use_unix_socket, i, -1, -1, 0);
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
      while (!worker_pool_shutdown_requested) {
         struct timespec ts = {0, 100000000};  /* 100ms */
         nanosleep(&ts, NULL);  /* Interruptible by signals */

         if (worker_pool_shutdown_requested)
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
   else if (var_worker_model == WORKER_MODEL_ADAPTIVE) {
      int i;

      adaptive_worker_capacity = var_adaptive_max_workers;
      adaptive_workers = calloc(adaptive_worker_capacity, sizeof(adaptive_worker_slot_t));
      if (adaptive_workers == NULL)
         die_err("Error: Failed to allocate adaptive worker array");

      for (i = 0; i < adaptive_worker_capacity; i++) {
         adaptive_workers[i].control_fd = -1;
      }

      worker_pool_shutdown_requested = 0;

      install_adaptive_handlers();

      for (i = 0; i < var_adaptive_min_workers; i++) {
         int slot = adaptive_find_free_slot();
         if (slot < 0)
            break;
         pid_t pid = adaptive_spawn_worker(sockfd, use_unix_socket, slot);
         if (pid <= 0) {
            adaptive_clear_slot(slot);
         }
      }

      while (!worker_pool_shutdown_requested) {
         adaptive_poll_worker_states();

         int active = adaptive_active_workers();

         if (active < var_adaptive_min_workers) {
            DEBUG_LOG("+ adaptive-scale active=%d idle=%d target_min=%d spare_target=%d max=%d",
               active, adaptive_idle_workers, var_adaptive_min_workers, var_adaptive_spare_workers, var_adaptive_max_workers);
            int needed = var_adaptive_min_workers - active;
            for (i = 0; i < needed && !worker_pool_shutdown_requested; i++) {
               int slot = adaptive_find_free_slot();
               if (slot < 0)
                  break;
               pid_t pid = adaptive_spawn_worker(sockfd, use_unix_socket, slot);
               if (pid <= 0)
                  adaptive_clear_slot(slot);
            }
         } else if (adaptive_idle_workers < var_adaptive_spare_workers && active < var_adaptive_max_workers) {
            DEBUG_LOG("+ adaptive-scale-spare active=%d idle=%d spare_target=%d max=%d",
               active, adaptive_idle_workers, var_adaptive_spare_workers, var_adaptive_max_workers);
            int slot = adaptive_find_free_slot();
            if (slot >= 0) {
               pid_t pid = adaptive_spawn_worker(sockfd, use_unix_socket, slot);
               if (pid <= 0)
                  adaptive_clear_slot(slot);
            }
         } else if (adaptive_idle_workers > var_adaptive_spare_workers && active > var_adaptive_min_workers) {
            DEBUG_LOG("- adaptive-scale-trim active=%d idle=%d spare_target=%d min=%d",
               active, adaptive_idle_workers, var_adaptive_spare_workers, var_adaptive_min_workers);
            adaptive_trim_idle_worker();
         }

         struct timespec ts = {0, 100000000};  /* 100ms */
         nanosleep(&ts, NULL);  /* Interruptible by signals */
      }

      log_event(LOG_INFO, "adaptive-shutdown pid=%d", (int)getpid());
      adaptive_terminate_workers();

      free(adaptive_workers);
      adaptive_workers = NULL;
      adaptive_worker_capacity = 0;
      adaptive_idle_workers = 0;

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
   const worker_snapshot_t *snap = worker_snapshot;
   worker_snapshot_view_t view;
   memset(&view, 0, sizeof(view));
   worker_snapshot_read(&view);
   const char *model = worker_model_to_string(view.available ? view.model : var_worker_model);
   char workers_json[2048];
   size_t offset = 0;
   int trimmed = 0;
   int listed = 0;
   int total_spare = view.available && view.model == WORKER_MODEL_ADAPTIVE ? view.idle_workers : 0;
   int capacity = 0;

   if (view.available) {
      if (view.model == WORKER_MODEL_PREFORK)
         capacity = view.prefork_workers;
      else if (view.model == WORKER_MODEL_ADAPTIVE)
         capacity = view.adaptive_max_workers;
   }

   if (capacity < 0)
      capacity = 0;
   if (capacity > ADAPTIVE_WORKERS_MAX)
      capacity = ADAPTIVE_WORKERS_MAX;

   offset += snprintf(workers_json + offset, sizeof(workers_json) - offset, "[");

   if (snap != NULL && snap->initialized) {
      for (int i = 0; i < capacity && offset < sizeof(workers_json) - 80; i++) {
         if (snap->slots[i].pid > 0) {
            offset += snprintf(
               workers_json + offset,
               sizeof(workers_json) - offset,
               "%s{\"index\":%d,\"pid\":%d",
               (listed == 0) ? "" : ",",
               i,
               (int)snap->slots[i].pid);

            if (snap->model == WORKER_MODEL_ADAPTIVE)
               offset += snprintf(
                  workers_json + offset,
                  sizeof(workers_json) - offset,
                  ",\"idle\":%s",
                  snap->slots[i].idle ? "true" : "false");

            if (snap->slots[i].terminating)
               offset += snprintf(workers_json + offset, sizeof(workers_json) - offset, ",\"terminating\":true");

            offset += snprintf(workers_json + offset, sizeof(workers_json) - offset, "}");
            listed++;
         }
      }

      if (listed < snap->active_workers && offset >= sizeof(workers_json) - 80)
         trimmed = 1;
      else if (snap->active_workers > listed)
         trimmed = 1;
   } else if (view.available && getpid() == server_owner_pid) {
      if (var_worker_model == WORKER_MODEL_PREFORK && prefork_worker_pids != NULL) {
         for (int i = 0; i < prefork_worker_count && offset < sizeof(workers_json) - 50; i++) {
            if (prefork_worker_pids[i] > 0) {
               offset += snprintf(
                  workers_json + offset,
                  sizeof(workers_json) - offset,
                  "%s{\"index\":%d,\"pid\":%d}",
                  (listed == 0) ? "" : ",",
                  i,
                  (int)prefork_worker_pids[i]);
               listed++;
            }
         }
         if (view.active_workers > listed)
            trimmed = 1;
      } else if (var_worker_model == WORKER_MODEL_ADAPTIVE && adaptive_workers != NULL) {
         for (int i = 0; i < adaptive_worker_capacity && offset < sizeof(workers_json) - 80; i++) {
            if (adaptive_workers[i].pid > 0) {
               offset += snprintf(
                  workers_json + offset,
                  sizeof(workers_json) - offset,
                  "%s{\"index\":%d,\"pid\":%d,\"idle\":%s}",
                  (listed == 0) ? "" : ",",
                  i,
                  (int)adaptive_workers[i].pid,
                  adaptive_workers[i].idle_reported ? "true" : "false");
               listed++;
            }
         }
         if (view.active_workers > listed)
            trimmed = 1;
         total_spare = adaptive_idle_workers;
      }
   }

   offset += snprintf(workers_json + offset, sizeof(workers_json) - offset, "]");

   stream_writeln(
      sock,
      "200 {\"worker_model\":\"%s\",\"workers\":%s,\"reuse_enabled\":false,\"total_spare\":%d,\"listed\":%d,"
      "\"trimmed\":%s,\"config\":{\"prefork_workers\":%d,\"adaptive_min\":%d,\"adaptive_max\":%d,\"adaptive_spare\":%d},"
      "\"active\":%d}",
      model,
      workers_json,
      total_spare,
      listed,
      trimmed ? "true" : "false",
      view.available ? view.prefork_workers : var_prefork_workers,
      view.available ? view.adaptive_min_workers : var_adaptive_min_workers,
      view.available ? view.adaptive_max_workers : var_adaptive_max_workers,
      view.available ? view.adaptive_spare_workers : var_adaptive_spare_workers,
      view.available ? view.active_workers : listed);
}
