#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#if HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include "cloud-rmi.h"
#include "config.h"

/**
 * C POSIX implementation of Cloud Simple Remote Method Invocation server.
 * This file declares functions commonly through the project.
 *
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license BSD
 */

/* Defaults for globals other than config vars */
int log_level = 1;
int var_server_start_time = 0;
int var_run_as_daemon = 0;
struct server_iface_entry *var_server_iface_map = NULL;

/**
 * Outputs an error message end exits program with the error status code
 * 
 * @param char* errstr Error string
 */
void die_err(char *errstr)
{
   perror(errstr);
   exit(1);
}

#ifdef LEGACY_STREAM
/**
 * Writes formatted string to the stream, similar usage as sprintf().
 * 
 */
int stream_writeln(int sock, char *format, ...)
{
   char buf[(BUF_SZ + 1)];
   int bytes;

   va_list argptr;
   va_start(argptr, format);

   if (vsnprintf(buf, BUF_SZ, format, argptr) == -1) {
      printf("stream_write() -- vsnprintf() failed");
      return -1;
   }
   va_end(argptr);
   strcat(buf, "\n");

   bytes = write(sock, buf, strlen(buf));

   if (bytes < 0) {
      printf("stream_write() -- ERROR writing to socket");
      return -1;
   }
   DEBUG_STREAM('>', buf);

   return bytes;
}


/**
 * Writes formatted string to the stream, similar usage as sprintf().
 * 
 *
int stream_writeln(int sock, char *format, ...)
{
   va_list argptr;
   va_start(argptr, format);
   stream_write(sock, format, argptr);
   stream_write(sock, "\r\n");
   va_end(argptr);
}*/

/**
 * Reads from stream to the given buffer of specified size
 * 
 */
int stream_read(int sock, char* buf, int buf_size)
{
   int bytes;

   bytes = read(sock, buf, buf_size);

   if (bytes < 0) {
      printf("stream_read() -- ERROR reading from socket");
      return -1;
   }

   // Exit on null character
   if (bytes == 1 && buf[0] == 0)
      return 0;

   if (bytes > 0) {
      int copy_len = bytes < BUF_SZ ? bytes : BUF_SZ;
      char log_line[BUF_SZ + 1];
      memcpy(log_line, buf, (size_t)copy_len);
      log_line[copy_len] = '\0';
      DEBUG_STREAM('<', log_line);
   }

   return bytes;
}

/**
 * Reads next non-empty line from stream to the given buffer of specified size
 * 
 */
int stream_readln(int sock, char* line, int max_size)
{
   int bytes, size = 0;
   char chr_buf;

   bzero(line, max_size - 1);

   while((bytes = read(sock, &chr_buf, 1)) > 0)  {
      if (bytes < 0) {
         printf("stream_read() -- ERROR reading from socket");
         return -1;
      }

      // Exit on null character
      if (chr_buf == 0)
         break;

      // Skipt first empty lines
      if (chr_buf == 13 || chr_buf == 10) {
         if (size)
            break;

         continue;
      }

      line[size] = chr_buf;
      size++;
   }

   DEBUG_STREAM('<', line);

   return size;
}

/**
 * Reads next non-empty lines until first empty line occours
 * 
 */
int stream_readlns(int sock, char* line, int max_size)
{
   int bytes, size = 0;
   int lnbr_count = 0;
   char chr_buf;

   bzero(line, max_size - 1);

   do  {
      bytes = read(sock, &chr_buf, 1);

      if (bytes < 0) {
         printf("stream_read() -- ERROR reading from socket");
         return -1;
      }

      // Exit on null character
      if (chr_buf == 0)
         break;

      // Filter carridge return
      if (chr_buf == 13)
         continue;      
       
      // Stop on empty line
      if (chr_buf == 10) {
         lnbr_count++;

         if (lnbr_count > 1) {
            line[size - 1] = '\0';  // remove last linefeed
            break;
         }

      } else
         lnbr_count = 0;
   

      line[size] = chr_buf;
      size++;
   } while(bytes > 0);

   DEBUG_STREAM('<', line);

   return size;
}

#else

void handle_error(const char *message) {
    perror(message);
    // Optionally, you can exit or handle the error differently
}

int stream_writeln(int sock, char *format, ...)
{
    char stack_buf[(BUF_SZ + 1)];
    char *buf = stack_buf;
    size_t buf_cap = BUF_SZ;
    size_t to_write;
    size_t written = 0;

    va_list argptr;
    va_start(argptr, format);

    int bytes = vsnprintf(buf, buf_cap, format, argptr);
    va_end(argptr);

    int rc = -1;

    if (bytes < 0) {
        handle_error("stream_writeln() -- vsnprintf() failed");
        goto cleanup;
    }

    if ((size_t)bytes >= buf_cap - 1) {
        size_t needed;

        if (__builtin_add_overflow((size_t)bytes, 2, &needed)) {
            handle_error("stream_writeln() -- vsnprintf() failed");
            goto cleanup;
        }

        buf = malloc(needed);
        if (buf == NULL) {
            handle_error("stream_writeln() -- vsnprintf() failed");
            goto cleanup;
        }

        buf_cap = needed;

        va_start(argptr, format);
        bytes = vsnprintf(buf, buf_cap, format, argptr);
        va_end(argptr);

        if (bytes < 0 || (size_t)bytes >= buf_cap - 1) {
            handle_error("stream_writeln() -- vsnprintf() failed");
            goto cleanup;
        }
    }

    buf[bytes] = '\n';   // append newline
    buf[bytes + 1] = '\0'; // null-terminate

    to_write = (size_t)(bytes + 1);

    while (written < to_write) {
        ssize_t just_written;

        do {
            just_written = write(sock, buf + written, to_write - written);
        } while (just_written < 0 && errno == EINTR);

        if (just_written < 0) {
            // Sockets are expected to be blocking; treat EAGAIN/EWOULDBLOCK as fatal
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                errno = EIO;
            handle_error("stream_writeln() -- ERROR writing to socket");
            goto cleanup;
        }

        if (just_written == 0) {
            handle_error("stream_writeln() -- ERROR writing to socket");
            goto cleanup;
        }

        written += (size_t)just_written;
    }
    DEBUG_STREAM('>', buf);

    rc = (int)written;

cleanup:
    if (buf != stack_buf)
        free(buf);

    return rc;
}

int stream_read(int sock, char* buf, int buf_size)
{
    int bytes;

    do {
        bytes = read(sock, buf, buf_size - 1);
    } while (bytes < 0 && errno == EINTR);

    if (bytes < 0) {
        handle_error("stream_read() -- ERROR reading from socket");
        return -1;
    }
    
    buf[bytes] = '\0'; // Null-terminate the buffer to safely print it

    DEBUG_STREAM('<', buf);

    return bytes;
}

int stream_readln(int sock, char* line, int max_size)
{
    ssize_t bytes = 0;
    size_t size = 0;
    int found_newline = 0;
    char buf[BUF_SZ];

    memset(line, 0, (size_t)max_size);

    while (!found_newline && size < (size_t)(max_size - 1)) {
        size_t peek_len = (size_t)(max_size - 1 - size);

        if (peek_len > sizeof(buf))
            peek_len = sizeof(buf);

        bytes = recv(sock, buf, peek_len, MSG_PEEK);

        if (bytes < 0) {
            if (errno == EINTR)
                continue;

            handle_error("stream_readln() -- ERROR reading from socket");
            return -1;
        }

        if (bytes == 0)
            break;

        char *newline = memchr(buf, '\n', (size_t)bytes);
        size_t to_consume = (size_t)bytes;

        if (newline != NULL)
            to_consume = (size_t)(newline - buf) + 1;

        do {
            bytes = read(sock, buf, to_consume);
        } while (bytes < 0 && errno == EINTR);

        if (bytes < 0) {
            handle_error("stream_readln() -- ERROR reading from socket");
            return -1;
        }

        if (bytes == 0)
            break;

        for (ssize_t i = 0; i < bytes && size < (size_t)(max_size - 1); i++) {
            if (buf[i] == '\r')
                continue;

            if (buf[i] == '\n') {
                found_newline = 1;
                break;
            }

            line[size++] = buf[i];
        }
    }

    DEBUG_STREAM('<', line);

    return (int)size;
}

int stream_readlns(int sock, char* line, int max_size)
{
    int bytes, size = 0;
    int lnbr_count = 0;
    char chr_buf;

    memset(line, 0, max_size);

    do {
        do {
            bytes = read(sock, &chr_buf, 1);
        } while (bytes < 0 && errno == EINTR);

        if (bytes < 0) {
            handle_error("stream_readlns() -- ERROR reading from socket");
            return -1;
        }

        // Exit on null character
        if (chr_buf == '\0')
            break;

        // Filter carriage return
        if (chr_buf == '\r')
            continue;

        // Stop on empty line
        if (chr_buf == '\n') {
            lnbr_count++;

            if (lnbr_count > 1) {
                line[size - 1] = '\0';  // remove last linefeed
                break;
            }
        } else {
            lnbr_count = 0;
        }

        if (size < max_size - 1) {
            line[size++] = chr_buf;
        }

    } while (bytes > 0);

    DEBUG_STREAM('<', line);

    return size;
}


#endif


/* Debug logging helpers */
#ifdef ENABLE_DEBUG_LOGGING

int debug_log_enabled(void)
{
   return var_debug;
}

void debug_log_set(int enabled)
{
   var_debug = enabled ? 1 : 0;
   log_level = enabled ? 1 : 0;
}

int debug_traffic_enabled(void)
{
   return var_debug_traffic;
}

void debug_traffic_set(int enabled)
{
   var_debug_traffic = enabled ? 1 : 0;
}

static void debug_emit_line(const char *line)
{
   if (!line)
      return;

   log_event(LOG_DEBUG, "%s", line);

   if (!var_run_as_daemon)
      printf("%s\n", line);
}

void debug_log_message(const char *format, ...)
{
   if (!debug_log_enabled())
      return;

   char buf[(BUF_SZ + 1)];
   va_list ap;

   va_start(ap, format);
   (void)vsnprintf(buf, sizeof(buf), format, ap);
   va_end(ap);

   size_t len = strlen(buf);
   while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
      buf[len - 1] = '\0';
      len--;
   }

   debug_emit_line(buf);
}

void debug_log_stream(char direction, const char *payload)
{
   if (!debug_log_enabled() || payload == NULL)
      return;

   char line[(BUF_SZ + 1)];
   size_t len = 0;
   while (len < BUF_SZ && payload[len] != '\0')
      len++;

   memcpy(line, payload, len);
   line[len] = '\0';

   while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[len - 1] = '\0';
      len--;
   }

   for (size_t i = 0; i < len; i++)
      if (line[i] == '\n' || line[i] == '\r')
         line[i] = ' ';

   char formatted[(BUF_SZ + 3)];
   formatted[0] = direction;
   formatted[1] = ' ';
   strncpy(formatted + 2, line, sizeof(formatted) - 3);
   formatted[sizeof(formatted) - 1] = '\0';
   debug_emit_line(formatted);
}

void log_traffic_event(const struct conn_state *state, const char *format, ...)
{
   if (!debug_traffic_enabled())
      return;

   char detail[256];
   va_list ap;

   va_start(ap, format);
   (void)vsnprintf(detail, sizeof(detail), format, ap);
   va_end(ap);

   if (state != NULL) {
      log_event(
         LOG_DEBUG,
         "traffic:conn=%i proto=%i authed=%i %s",
         state->sock,
         state->protocol_version,
         state->is_authenticated,
         detail);
   } else {
      log_event(LOG_DEBUG, "traffic:%s", detail);
   }
}
#endif

/**
 * Writes a message about an event to the syslog or a log file
 * 
 */
void log_event(int priority, char *format, ...)
{
   char buf[(BUF_SZ + 1)];
   va_list ap;

   va_start(ap, format);
   (void)vsnprintf(buf, BUF_SZ, format, ap);
   va_end(ap);

#if ENABLE_SYSLOG_TRACE
   FILE *fp;

   if((fp = fopen("/tmp/rmi-server.log", "a")) != (FILE *)NULL) {
      (void)fprintf(fp, "%s\n", buf);
      (void)fclose(fp);
   }
   else {
      (void)fprintf(stderr, "Can't write to /tmp/rmi-server.log\n");
   }

#if HAVE_SYSLOG_H
#if OLDSYSLOG
   openlog(APP_NAME, LOG_PID);
#else
   openlog(APP_NAME, LOG_PID, LOG_MAIL);
#endif
   syslog(priority, "%s", buf);
   closelog();
#endif
#else
   (void)priority;
#endif
}

void log_unexpected_event(int priority, const char *format, ...)
{
   char buf[256];
   va_list ap;

   va_start(ap, format);
   (void)vsnprintf(buf, sizeof(buf), format, ap);
   va_end(ap);

   log_event(priority, "unexpected:%s", buf);
}

/**
 * Utility function
 * Gets substring of a string
 *  
 */
void substring(char *string, char *sub, int start, int len) {
   int pos = 0;
 
   while (pos < len) {
      sub[pos] = string[start + pos];
      pos++;
   }
   sub[pos] = '\0';
}

//
// Utility functions
// 

/**
 * Splits hostname in form host:port, uses default port if not given
 * 
 * @param host 
 * @param ip 
 * @param port 
 */
void split_host_addr(char *host, char *ip, int *port)
{
   if (sscanf(host, "%[^:]:%i", ip, port)) {
      return;

   } else if (sscanf(host, ":%i", port)) {
      strcpy(ip, "127.0.0.1");
      return;
   }

   sscanf(host, "%s", ip);
   *port = var_port;
}

char *get_ip_addr(char *hostname)
{
   char *ret = NULL;
   struct hostent *he;

   if ((he = gethostbyname(hostname)) == NULL) {
      return NULL;
   }

   ret = strdup(inet_ntoa(*(struct in_addr*) he->h_addr));

   return ret;
}
