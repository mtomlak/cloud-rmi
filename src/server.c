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
#include "cloud-rmi.h"
#include "config.h"
#include <sys/wait.h>
#include <time.h>
//#include <syscall.h>

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

static void cleanup_listen_socket()
{
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
      printf("Warning: Cannot remove spare child %i: not found", pid);
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
      printf("Error: handle_by_spare_proc(): Cannot find any spare child\n");
      return 0;
   }

   if (var_debug)
      printf("Going to wake up child %i to serve socket %i\n", spare_child->pid, sock);

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
      handle_connection(state.sock); 
      close(state.sock); 
   }

   #ifdef REUSE_WORKER_PROC
   // let's pause till we get new socket assigned
   while(1) {

      // we are resumend from pause
      if (resumed_from_pause) {  

         worker_state = server_get_spare_child((int) getpid());

         if (worker_state == NULL) {
            printf("Warning: Cannot load worker state for resumed proccess %i, exiting spare child\n", (int) getpid());
            server_remove_spare_child((int) getpid());
            break;

         } else if (worker_state->stop_signal) {
            server_remove_spare_child((int) getpid());
            break;

         } else if (worker_state->next_sock < 0) {
            printf("Warning: Cannot load next socket for resumed proccess %i, exiting spare child\n", (int) getpid());
            server_remove_spare_child((int) getpid());
            break;
         }
         state.sock = worker_state->next_sock;
         server_remove_spare_child(worker_state->pid);
         handle_connection(state.sock);
         close(state.sock);
      }

      if (server_spare_children.count < MAX_SPARE_WORKERS) {
         server_add_spare_child((int) getpid());
         resumed_from_pause = 1;
         if (var_debug) printf("|| Putting child %i to sleep\n", (int) getpid());
         kill(getpid(), SIGSTOP);   // pause until we got awaken
         if (var_debug) printf(">> Waking child %i from sleep\n", (int) getpid());

      } else {
         if (var_debug)
            printf("Not keeping child %i for reuse (reached %i/%i max_spare_children)\n", (int) getpid(), server_spare_children.count, MAX_SPARE_WORKERS);

         break;   // if we don't need any more spare proccesses
      }

   };

   #endif

   if (var_debug)
      printf("- Shutting down child with PID %i\n", (int) getpid());

   if (state.sock >= 0)
      close(state.sock);

   child_exit_self(state.stack, state.stack_size);
}


/**
 * Create new child_proc_state struct in memory until it's free()-ed
 * 
 */
struct child_proc_state *new_child_state(int sock, void *stack, int stack_size)
{
   struct child_proc_state *state = malloc(sizeof(struct child_proc_state));

   state->sock = sock;
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
int create_client_handler_proc(int sock, int server_sock)
{
   int pid = 0;
   char *stack = malloc(CHILD_STACK_SZ + 1);
   struct child_proc_state *child_state = new_child_state(sock, stack, CHILD_STACK_SZ);

   if (var_debug)
      printf("Requested new child for socket #%i\n", sock);

   #ifdef REUSE_WORKER_PROC
   if (sock != -1 && server_spare_children.count) {
      if (var_debug)
         printf("Trying to reuse existing child proccess for socket #%i\n", sock);

      // We can re-use sleeping spare children instead of creating new one
      pid = handle_by_spare_proc(sock);

      if (!pid) {
         printf("Error: handle_by_spare_proc() failed");
         return 0;
      }
      
      if (var_debug)
         printf("* Reused child with PID %i\n", pid);

   } else {
      if (var_debug && sock == -1)
         printf("Going to create spare child\n");

      if (var_debug && !server_spare_children.count)
         printf("Going to new child, no spare ones present\n");

   #endif

      if (var_debug)
         printf("Creating new child proccess with Linux clone() for socket #%i\n", sock);

      // start new proccess and execute handle_cloned_proc() as a child
      pid = clone(&handle_cloned_proc, (char *)stack + CHILD_STACK_SZ, CLONE_VM|CLONE_FILES|SIGCHLD, child_state);//CLONE_VM|CLONE_FS|CLONE_SIGHAND|

      if (pid < 0) {
         printf("Error: clone() failed");
         return 0;
      }

      if (var_debug)
         printf("+ Started new child with PID %i\n", pid);

   #ifdef REUSE_WORKER_PROC
   }
   #endif

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
      create_client_handler_proc(-1, -1);

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

   state->stack = stack;
   state->stack_size = stack_size;

   return state;
}

void manager_handle_sigterm(int signum)
{
   shutdown_handler(signum);

   if (var_debug)
      printf("WorkerManager: Terminating all children\n");

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
         if (var_debug)
               printf("WorkerManager: Checking number of spare worker proccesses\n");

         if (server_spare_children.count > MIN_SPARE_WORKERS) {
            if (var_debug)
               printf("WorkerManager: Terminating extra children (got %i of %i required)\n", server_spare_children.count, MIN_SPARE_WORKERS);
            terminate_extra_children_proc(MIN_SPARE_WORKERS);
         }
         
         if (server_spare_children.count < MIN_SPARE_WORKERS) {
            if (var_debug)
               printf("WorkerManager: Creating new children (got %i of %i required)\n", server_spare_children.count, MIN_SPARE_WORKERS);
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

   if (var_debug) {
      printf("Starting worker proccess manager\n");
      printf("Creating new manager proccess with Linux clone() ...\n");
   }

   // start new proccess and execute handle_cloned_proc() as a child
   //sleep(1);//don't fork too fast
   pid = clone(&start_worker_manager, (char *)stack + CHILD_STACK_SZ, CLONE_VM|CLONE_FILES|CLONE_SIGHAND, helper_state);//CLONE_VM|CLONE_FS|CLONE_SIGHAND|

   if (pid < 0) {
      printf("Error: clone() failed");
      return 0;
   }

   if (var_debug)
      printf("+ Started new manager proccess with PID %i\n", pid);

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
int create_client_handler_proc(int sock, int server_sock)
{
   int pid = 0;

   if (var_debug)
      printf("Creating new child proccess with POSIX fork() socket #%i\n", sock);

   #ifdef PROXY_SUPPORT
      if (var_proxy)
         printf("Warning: Using proxy when compiled without Linux glibc support is very experimental!\n");
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

      if (var_debug)
         printf("+ Started new child with PID %i\n", (int) getpid());

      close(server_sock); // Parent has it's own copy now, close it
      handle_connection(sock);

      if (var_debug)
         printf("- Shutting down child with PID %i\n", (int) getpid());

      close(sock);
      exit(0);
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

   server_should_run = 1;
   server_socket_fd = -1;
   server_socket_path[0] = '\0';

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
      create_client_handler_proc(newsockfd, sockfd);
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

int read_iface_config(char *config_file)
{
   int r = 1;
   int count = 0;
   FILE *fp;
   char *line = malloc(sizeof(char) * (BUF_SZ + 1));
   char *iface = malloc(sizeof(char) * (BUF_SZ + 1));
   char *file = malloc(sizeof(char) * (BUF_SZ + 1));
   char *iface_short = malloc(sizeof(char) * (BUF_SZ + 1));
   char *file_short = malloc(sizeof(char) * (BUF_SZ + 1));

   bzero(line, BUF_SZ);
   bzero(iface, BUF_SZ);
   bzero(file, BUF_SZ);
   bzero(iface_short, BUF_SZ);
   bzero(file_short, BUF_SZ);

   if((fp = fopen(config_file, "r")) == NULL) { 
      free(line);
      free(iface);
      free(file);
      free(iface_short);
      free(file_short);
      return 0;
   }

   // Wipe existing interface entries (in case config gets reloaded or like)
   server_wipe_iface_entries();

   r = fscanf(fp,"%[^\t\n# ] %[^\t\n ]\n", iface_short, file_short);

   while(r != EOF) {
      if (r == 2) {
            // Complete relative namespace names and file names
            if (iface_short[0] == '\\')
               strcpy(iface, iface_short);
            else
               sprintf(iface, "%s\\%s", var_script_base_namespace, iface_short);

            if (file_short[0] == '/')
               strcpy(file, file_short);
            else
               sprintf(file, "%s/%s", var_script_exec_dir, file_short);

               server_add_iface_entry(iface, file);
               count++;
      } else
         fscanf(fp,"%*[^\n]\n"); // skip unparsable lines

      r = fscanf(fp,"%[^\t\n# ] %[^\t\n ]\n", iface_short, file_short);
   }

   fclose(fp);
   free(line);
   free(iface);
   free(file);
   free(iface_short);
   free(file_short);

   if (var_debug)
      printf("Loaded %d interface map entries from %s\n", count, config_file);

   return 1;
}

void server_send_stats(int sock)
{
    #ifdef REUSE_WORKER_PROC
        struct spare_child_worker *current_child;
        
        stream_writeln(sock, "Server Worker Info:\nPID:\tNext Socket:\tStop Signal:");

        current_child = server_spare_children.first;

        while (current_child != NULL) {
            stream_writeln(sock, "%i\t%i\t\t%i", current_child->pid, current_child->next_sock, current_child->stop_signal);
            current_child = current_child->next;
        }

        stream_writeln(sock, "\nTotal Spare Worker: %i", server_spare_children.count);
        stream_writeln(sock, "\nMin Spare Worker: %i", MIN_SPARE_WORKERS);
        stream_writeln(sock, "\nMax Spare Worker: %i", MAX_SPARE_WORKERS);

    #else
        stream_writeln(sock, "REUSE_WORKER_PROC flag not enabled. No spare children information available.");
    #endif

    stream_write(sock, "\n\n");   // end data with an empty line
}
