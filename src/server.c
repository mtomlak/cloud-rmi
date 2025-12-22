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
#include <sys/types.h>  // good for getpid()
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <stdarg.h>
#include <stdbool.h>
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

#ifdef ENABLE_LINUX_FEATURES

void child_exit_self(void *stack, int stack_size)
{
   // stack size not used in this implementation
    free(stack);
    _exit(0);
}

/*
void child_exit_self(void *stack, int stack_size) 
{
    if (stack_size < 0)
        return;

    syscall(SYS_munmap, stack, (size_t)stack_size);
    syscall(SYS_exit, 0);
}

void child_exit_self(void *stack, int stack_size)
{
   // Call that allows child to clean up it's own stack
    __asm__ volatile (
            "mov $11,%%rax\n"  //call munmap
             "mov $0,%%rdi\n"
             "mov $1,%%rsi\n"
             "syscall\n"
            "xor %%rdi,%%rdi\n"
            "mov $60,%%rax\n"//manually call exit
             "syscall\n" :: "g" (stack), "g" (stack_size)
             );
}
*/

#ifdef REUSE_WORKER_PROC

void init_spare_children_list()
{
   server_spare_children.count = 0;
   server_spare_children.first = NULL;
   server_spare_children.last = NULL;
}

/**
 * Adds child proccess to the list of spare children proccesses
 * 
 * @param  pid
 * @return     NULL on failure
 */
struct spare_child_worker *server_get_spare_child(int pid)
{
   struct spare_child_worker *cur = server_spare_children.first;

   while(cur != NULL) {
      if (cur->pid == pid)
         return cur;

      cur = cur->next;
   }

   return NULL;
}

/**
 * Gets first not-busy child
 * 
 * @param  pid
 * @return     0 on failure, index in the list on success
 */
struct spare_child_worker *server_get_first_spare_child()
{
   struct spare_child_worker *cur = server_spare_children.first;

   while(cur != NULL) {
      if (cur->next_sock < 0) // make sure the chind it's really free
         return cur;

      cur = cur->next;
   }

   return NULL;
}

/**
 * Gets last spare child
 * 
 * @param  pid
 * @return     0 on failure, index in the list on success
 */
struct spare_child_worker *server_get_last_spare_child()
{
   struct spare_child_worker *cur = server_spare_children.last;

   while(cur != NULL) {
      if (cur->next_sock < 0) // make sure the chind it's really free
         return cur;

      cur = cur->prev;
   }

   return NULL;
}

/**
 * Adds child proccess to the list of spare children proccesses
 * 
 * @param  pid
 * @return     0 on failure
 */
int server_add_spare_child(int pid)
{
   struct spare_child_worker *spare_child = malloc(sizeof(struct spare_child_worker));

   if (spare_child == NULL) {
      log_event(LOG_ERR, "server_add_spare_child() -- malloc() failed");
      return 0;
   }

   // Putting new child to the beginning
   spare_child->pid = pid;
   spare_child->next_sock = -1;
   spare_child->stop_signal = 0;
   spare_child->next = server_spare_children.first;
   spare_child->prev = NULL;

   if (server_spare_children.first != NULL)
      server_spare_children.first->prev = spare_child;

   if (server_spare_children.last == NULL)
      server_spare_children.last = spare_child;

   server_spare_children.first = spare_child;
   server_spare_children.count++;

   return 1;
}

/**
 * Removes child proccess from the list of spare children proccesses
 * 
 * @param  pid
 * @return     0 on failure, index in the list on success
 */
int server_remove_spare_child(int pid)
{
   struct spare_child_worker *spare_child = server_get_spare_child(pid);

   if (spare_child == NULL) {
      log_unexpected_event(LOG_WARNING, "spare-child-missing pid=%i", pid);
      return 0;
   }

   if (spare_child->next == NULL)
      server_spare_children.last = spare_child->prev;
   else
      spare_child->next->prev = spare_child->prev;

   if (spare_child->prev == NULL)
      server_spare_children.first = spare_child->next;
   else
      spare_child->prev->next = spare_child->next;

   free(spare_child);
   server_spare_children.count--;

   return 1;
}

/**
 * Resumes first spare child from sleep and lets it handle the connection
 * 
 * @param  sock
 * @return
 */
int handle_by_spare_proc(int sock)
{
   struct spare_child_worker *spare_child = server_get_last_spare_child();

   if (spare_child == NULL) {
      log_unexpected_event(LOG_ERR, "handle_by_spare_proc(): Cannot find any spare child");
      return 0;
   }

   DEBUG_LOG("Reusing frozen child %i to serve socket %i", spare_child->pid, sock);

   spare_child->next_sock = sock;
   kill(spare_child->pid, SIGCONT);

   return spare_child->pid;
}

   #endif

int handle_cloned_proc(void *arg)
{
   struct child_proc_state *rcvd_state;
   struct child_proc_state state;
   #ifdef REUSE_WORKER_PROC
   int resumed_from_pause = 0;
   struct spare_child_worker *worker_state = NULL;
   #endif

   // we can copy the data and free target now
   rcvd_state = (struct child_proc_state *) arg;
   state = *rcvd_state;
   free(rcvd_state);

   /* Child handlers spawn their own grandchildren (shell calls). They must
    * manage those children explicitly, so disable the parent's SIGCHLD reaper
    * in this process. */
   {
      struct sigaction sa_default;

      sa_default.sa_handler = SIG_DFL;
      sigemptyset(&sa_default.sa_mask);
      sa_default.sa_flags = 0;
      sigaction(SIGCHLD, &sa_default, NULL);
   }

   if (state.sock >= 0) {
      handle_connection(state.sock, state.is_unix_socket); 
      close(state.sock); 
   }

   #ifdef REUSE_WORKER_PROC
   // let's pause till we get new socket assigned
   while(1) {

      // we are resumend from pause
      if (resumed_from_pause) {  

         worker_state = server_get_spare_child((int) getpid());

         if (worker_state == NULL) {
            log_unexpected_event(
               LOG_WARNING,
               "missing-worker-state pid=%i",
               (int) getpid());
            server_remove_spare_child((int) getpid());
            break;

         } else if (worker_state->stop_signal) {
            server_remove_spare_child((int) getpid());
            break;

         } else if (worker_state->next_sock < 0) {
            log_unexpected_event(
               LOG_WARNING,
               "missing-next-sock pid=%i",
               (int) getpid());
            server_remove_spare_child((int) getpid());
            break;
         }
         state.sock = worker_state->next_sock;
         DEBUG_LOG("Resumed frozen child %i for socket #%i", (int) getpid(), state.sock);
         server_remove_spare_child(worker_state->pid);
         handle_connection(state.sock, state.is_unix_socket);
         close(state.sock);
      }

      if (server_spare_children.count < MAX_SPARE_WORKERS) {
         if (!server_add_spare_child((int) getpid())) {
            break;
         }
         resumed_from_pause = 1;
         DEBUG_LOG("|| Putting child %i to sleep (spares=%i)", (int) getpid(), server_spare_children.count);
         kill(getpid(), SIGSTOP);   // pause until we got awaken
         DEBUG_LOG(">> Waking child %i from sleep", (int) getpid());

      } else {
         DEBUG_LOG(
            "Not keeping child %i for reuse (reached %i/%i max_spare_children)",
            (int) getpid(),
            server_spare_children.count,
            MAX_SPARE_WORKERS);

         break;   // if we don't need any more spare proccesses
      }

   };

   #endif

   DEBUG_LOG("- Shutting down child with PID %i", (int) getpid());

   if (state.sock >= 0)
      close(state.sock);

   child_exit_self(state.stack, state.stack_size);
}


/**
 * Create new child_proc_state struct in memory until it's free()-ed
 * 
 */
struct child_proc_state *new_child_state(int sock, int is_unix_socket, void *stack, int stack_size)
{
   struct child_proc_state *state = malloc(sizeof(struct child_proc_state));

   if (state == NULL)
      return NULL;

   state->sock = sock;
   state->is_unix_socket = is_unix_socket;
   state->stack = stack;
   state->stack_size = stack_size;

   return state;
}

/**
 * Creates a child proccess to handle new client connection.
 * The child proccess behaves like a thread, sharing all changes in memory
 * and file descriptors with the parent proccess.
 * New proccess is created by call() Linux syscall.
 * 
 * @param  sock        Client socket (the socket created with accept())
 * @param  server_sock Server socket (the socket created with bind())
 * @return             0 on failure, child pid on success
 */
int create_client_handler_proc(int sock, int server_sock, int is_unix_socket)
{
   int pid = 0;
   char *stack = NULL;
   struct child_proc_state *child_state = NULL;

   (void)server_sock;

   DEBUG_LOG("Requested new child for socket #%i", sock);

   #ifdef REUSE_WORKER_PROC
   if (sock != -1 && server_spare_children.count) {
      DEBUG_LOG("Trying to reuse existing child proccess for socket #%i", sock);

      // We can re-use sleeping spare children instead of creating new one
      pid = handle_by_spare_proc(sock);

      if (!pid) {
         log_unexpected_event(LOG_ERR, "handle_by_spare_proc() failed");
         return 0;
      }
      
      DEBUG_LOG("* Reused child with PID %i", pid);

      return pid;
   }

   if (sock == -1)
      DEBUG_LOG("Going to create spare child");

   if (!server_spare_children.count)
      DEBUG_LOG("Going to new child, no spare ones present");
   #endif

   stack = malloc(CHILD_STACK_SZ + 1);
   child_state = new_child_state(sock, is_unix_socket, stack, CHILD_STACK_SZ);

   if (stack == NULL || child_state == NULL) {
      log_event(LOG_ERR, "Error: failed allocating child process resources");
      free(stack);
      free(child_state);
      return 0;
   }

   DEBUG_LOG("Creating new child proccess with Linux clone() for socket #%i", sock);

   // start new proccess and execute handle_cloned_proc() as a child
   pid = clone(&handle_cloned_proc, (char *)stack + CHILD_STACK_SZ, CLONE_VM|CLONE_FILES|SIGCHLD, child_state);//CLONE_VM|CLONE_FS|CLONE_SIGHAND|

   if (pid < 0) {
      printf("Error: clone() failed");
      free(child_state);
      free(stack);
      return 0;
   }

   DEBUG_LOG("+ Started new child with PID %i", pid);

   // not touching server socket, it's shared resource

   return pid;
}

   #ifdef REUSE_WORKER_PROC
/**
 * Creates neccessary amount of spare child proccesses that are ready to
 * proccess new connections.
 * 
 * @param max_spare_children
 */
void create_spare_children_proc(int min_spare_children)
{
   int i;

   for(i = server_spare_children.count; i < min_spare_children; i++)
      create_client_handler_proc(-1, -1, server_use_unix_socket);

}

void terminate_extra_children_proc(int min_spare_children)
{
   int i;
   struct spare_child_worker *cur = server_spare_children.last;

   for(i = server_spare_children.count; i > min_spare_children; i--) {
      if(cur != NULL) {
         if (cur->next_sock < 0) {// make sure the chind it's really free
            cur->stop_signal = 1;
            kill(cur->pid, SIGCONT); // unfreeze
         }
         cur = cur->prev;
      } else
         break;
   }
}

struct helper_proc_state *new_helper_state(void *stack, int stack_size)
{
   struct helper_proc_state *state = malloc(sizeof(struct helper_proc_state));

   if (state == NULL)
      return NULL;

   state->stack = stack;
   state->stack_size = stack_size;

   return state;
}

void manager_handle_sigterm(int signum)
{
   shutdown_handler(signum);

   DEBUG_LOG("WorkerManager: Terminating all children");

   // Wake up and terminate sleeping children
   terminate_extra_children_proc(server_spare_children.count);
}

int start_worker_manager(void *arg)
{
   int sec_passed = 0;
   struct helper_proc_state *rcvd_state;
   struct helper_proc_state state;
   struct sigaction action;

   // we can copy the data and free target now
   rcvd_state = (struct helper_proc_state *) arg;
   state = *rcvd_state;
   free(rcvd_state);

   // lets wait a minute until workers settle down
   sleep(60);

   // set signal handler
   memset(&action, 0, sizeof(struct sigaction));
   action.sa_handler = manager_handle_sigterm;
   sigaction(SIGTERM, &action, NULL);

   while (1) {
      if ((int) fmod(sec_passed, 60) == 0) { // actions to be done every minute
         // check spare worker proccesses
         DEBUG_LOG("WorkerManager: Checking number of spare worker proccesses");

         if (server_spare_children.count > MIN_SPARE_WORKERS) {
            DEBUG_LOG("WorkerManager: Terminating extra children (got %i of %i required)", server_spare_children.count, MIN_SPARE_WORKERS);
            terminate_extra_children_proc(MIN_SPARE_WORKERS);
         }
         
         if (server_spare_children.count < MIN_SPARE_WORKERS) {
            DEBUG_LOG("WorkerManager: Creating new children (got %i of %i required)", server_spare_children.count, MIN_SPARE_WORKERS);
            create_spare_children_proc(MIN_SPARE_WORKERS);
         }
      }
      waitpid(-1, NULL, WNOHANG);   // keep collecting child signals // server_remove_spare_child
      sleep(1);
      sec_passed++;
   }

   manager_handle_sigterm(SIGTERM);
   child_exit_self(state.stack, state.stack_size);
}



int create_worker_manager_proc()
{
   int pid = 0;
   char *stack = malloc(CHILD_STACK_SZ + 1);
   struct helper_proc_state *helper_state = new_helper_state(stack, CHILD_STACK_SZ);

   if (stack == NULL || helper_state == NULL) {
      log_event(LOG_ERR, "Error: failed allocating worker manager resources");
      free(stack);
      free(helper_state);
      return 0;
   }

   DEBUG_LOG("Starting worker proccess manager");
   DEBUG_LOG("Creating new manager proccess with Linux clone() ...");

   // start new proccess and execute handle_cloned_proc() as a child
   //sleep(1);//don't fork too fast
   pid = clone(&start_worker_manager, (char *)stack + CHILD_STACK_SZ, CLONE_VM|CLONE_FILES|CLONE_SIGHAND, helper_state);//CLONE_VM|CLONE_FS|CLONE_SIGHAND|

   if (pid < 0) {
      printf("Error: clone() failed");
      free(helper_state);
      free(stack);
      return 0;
   }

   DEBUG_LOG("+ Started new manager proccess with PID %i", pid);

   return pid;
}

   #endif
#else
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

   #ifdef PROXY_SUPPORT
      if (var_proxy)
         log_unexpected_event(
            LOG_WARNING,
            "proxy-without-linux-features pid=%i",
            (int) getpid());
   #endif

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
#endif


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

#ifdef REUSE_WORKER_PROC
   /* Get ready some spare children */
   init_spare_children_list();
   create_worker_manager_proc();
#endif

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
      /* Check state of proxy connections if there's no separate proccess to handle that */
   #ifndef ENABLE_LINUX_FEATURES
      if (var_proxy) {
         proxy_all_keepalive_conns();
         proxy_all_check_spare_conns();
      }
   #endif
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

#ifdef REUSE_WORKER_PROC
   manager_handle_sigterm(SIGTERM);
#endif

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
      if (strcmp(iface_name, cur->iface) == 0)
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

void server_delete_iface_entry(char *iface_name)
{
   server_destroy_iface_entry(server_get_iface_entry(iface_name));
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

static bool append_json(char *json, size_t json_size, size_t *written, const char *fmt, ...)
{
   if (*written >= json_size)
      return false;

   va_list args;
   va_start(args, fmt);
   int added = vsnprintf(json + *written, json_size - *written, fmt, args);
   va_end(args);

   if (added < 0 || (size_t)added >= json_size - *written) {
      *written = json_size;
      return false;
   }

   *written += (size_t)added;
   return true;
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
#ifdef REUSE_WORKER_PROC
   /* Keep the response safely below the MAX_LINE parser limit. With 25 entries
    * and the chosen field set we stay under ~2 KB. */
   const int max_workers = 25;
   char json[BUF_SZ + 1];
   size_t written = 0;
   int listed = 0;
   bool trimmed = false;
   struct spare_child_worker *current_child = server_spare_children.first;

   append_json(json, sizeof(json), &written, "{\"reuse_enabled\":true,\"workers\":[");

   while (current_child != NULL && listed < max_workers) {
      const char *state = "idle";

      if (current_child->stop_signal) {
         state = "stopping";
      } else if (current_child->next_sock >= 0) {
         state = "assigned";
      }

      append_json(
         json,
         sizeof(json),
         &written,
         "%s{\"pid\":%i,\"next_sock\":%i,\"stop_signal\":%i,\"state\":\"%s\"}",
         listed ? "," : "",
         current_child->pid,
         current_child->next_sock,
         current_child->stop_signal,
         state);

      listed++;
      current_child = current_child->next;
   }

   if (current_child != NULL)
      trimmed = true;

   append_json(
      json,
      sizeof(json),
      &written,
      "],\"total_spare\":%i,\"min_spare\":%i,\"max_spare\":%i,\"listed\":%i,\"trimmed\":%s}",
      server_spare_children.count,
      MIN_SPARE_WORKERS,
      MAX_SPARE_WORKERS,
      listed,
      trimmed ? "true" : "false");

   /* If anything overflowed, fall back to a minimal payload to preserve protocol
    * correctness. */
   if (written >= sizeof(json)) {
      snprintf(
         json,
         sizeof(json),
         "{\"reuse_enabled\":true,\"workers\":[],\"total_spare\":%i,\"min_spare\":%i,\"max_spare\":%i,\"listed\":0,\"trimmed\":true}",
         server_spare_children.count,
         MIN_SPARE_WORKERS,
         MAX_SPARE_WORKERS);
   }

   stream_writeln(sock, "200 %s", json);
#else
   stream_writeln(sock, "200 {\"reuse_enabled\":false,\"workers\":[],\"total_spare\":0,\"min_spare\":0,\"max_spare\":0,\"listed\":0,\"trimmed\":false}");
#endif
}
