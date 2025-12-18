#include "php.h"
#include "php_profiweb_cloudrmi.h"

#include "ext/standard/base64.h"
#include "ext/standard/php_var.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_interfaces.h"
#include "Zend/zend_smart_str.h"
#include "ext/json/php_json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <cloudrmi/types.h>

#define CLOUDRMI_NS "ProfiWeb\\Cloud\\Infrastructure\\Messaging\\RemoteMethodInvocation"

static zend_class_entry *cloudrmi_exception_ce;
static zend_class_entry *cloudrmi_remote_error_ce;
static zend_class_entry *cloudrmi_remote_exception_ce;
static zend_class_entry *cloudrmi_client_ce;
static zend_class_entry *cloudrmi_server_ce;
static zend_object_handlers cloudrmi_client_handlers;
static zend_object_handlers cloudrmi_server_handlers;

typedef struct _cloudrmi_client_object {
    zend_string *host;
    zend_string *unix_path;
    zend_long port;
    zend_long conn_timeout;
    zend_long read_timeout;
    zend_bool throttle_timeout;
    double throttle_duration;
    int socket_fd;
    zend_long protocol_version;
    zend_long advertised_max_protocol;
    zend_bool supports_payload;
    zend_bool supports_async;
    zend_bool connected;
    zend_object std;
} cloudrmi_client_object;

typedef struct _cloudrmi_server_binding {
    zend_string *iface;
    zval handler;
} cloudrmi_server_binding;

typedef struct _cloudrmi_server_object {
    HashTable bindings;
    zend_string *host;
    zend_string *context_name;
    zend_string *unix_path;
    zend_string *recovery_token;
    zend_long port;
    zend_long read_timeout;
    zend_long high_read_timeout;
    zend_long nofork_read_timeout;
    zend_bool disable_fork;
    zend_bool require_auth;
    zend_bool enable_payload_capability;
    zend_bool enable_async_capability;
    zend_bool unix_socket_skip_auth;
    zend_long max_plain_string_bytes;
    zend_long protocol_version;
    zend_long max_protocol_version;
    time_t started_at;
    int listen_fd;
    zend_bool do_shutdown;
    zend_object std;
} cloudrmi_server_object;

#define CLOUDRMI_INLINE_THRESHOLD_FALLBACK 4825

static inline cloudrmi_client_object *cloudrmi_client_from_obj(zend_object *obj)
{
    return (cloudrmi_client_object *)((char *)(obj) - XtOffsetOf(cloudrmi_client_object, std));
}

static inline cloudrmi_server_object *cloudrmi_server_from_obj(zend_object *obj)
{
    return (cloudrmi_server_object *)((char *)(obj) - XtOffsetOf(cloudrmi_server_object, std));
}

static zend_string *cloudrmi_base64_encode_str(const zend_string *input)
{
    return php_base64_encode((const unsigned char *)ZSTR_VAL(input), ZSTR_LEN(input));
}

static zend_bool cloudrmi_base64_decode_str(const zend_string *input, zend_string **decoded, zend_bool strict)
{
    zend_string *result = php_base64_decode_ex((const unsigned char *)ZSTR_VAL(input), ZSTR_LEN(input), strict);
    if (!result) {
        return 0;
    }

    *decoded = result;
    return 1;
}

static zend_string *cloudrmi_serialize_zval(zval *value)
{
    smart_str buf = {0};
    php_serialize_data_t var_hash;

    PHP_VAR_SERIALIZE_INIT(var_hash);
    php_var_serialize(&buf, value, &var_hash);
    PHP_VAR_SERIALIZE_DESTROY(var_hash);

    if (!buf.s) {
        return NULL;
    }

    smart_str_0(&buf);
    return buf.s;
}

static zend_bool cloudrmi_unserialize(const zend_string *payload, zval *return_value)
{
    const unsigned char *p = (unsigned char *)ZSTR_VAL(payload);
    php_unserialize_data_t var_hash;

    PHP_VAR_UNSERIALIZE_INIT(var_hash);
    if (!php_var_unserialize(return_value, &p, p + ZSTR_LEN(payload), &var_hash)) {
        PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
        ZVAL_UNDEF(return_value);
        return 0;
    }
    PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
    return 1;
}

static zend_bool cloudrmi_json_decode(const zend_string *json, zval *return_value)
{
    php_json_decode_ex(return_value, ZSTR_VAL(json), ZSTR_LEN(json), PHP_JSON_OBJECT_AS_ARRAY, PHP_JSON_PARSER_DEFAULT_DEPTH);
    return Z_TYPE_P(return_value) != IS_NULL;
}

static zend_string *cloudrmi_json_encode_zval(zval *value)
{
    smart_str buf = {0};
    php_json_encode(&buf, value, 0);
    smart_str_0(&buf);
    return buf.s;
}

static void cloudrmi_client_update_properties(cloudrmi_client_object *obj)
{
    zval zv;
    zend_object *zobj = &obj->std;

    ZVAL_STR_COPY(&zv, obj->host);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("host"), &zv);
    zval_ptr_dtor(&zv);

    ZVAL_LONG(&zv, obj->port);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("port"), &zv);

    ZVAL_LONG(&zv, obj->conn_timeout);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("connTimeout"), &zv);

    ZVAL_LONG(&zv, obj->read_timeout);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("readTimeout"), &zv);

    ZVAL_BOOL(&zv, obj->throttle_timeout);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("throttleTimeout"), &zv);

    ZVAL_DOUBLE(&zv, obj->throttle_duration);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("throttleDuration"), &zv);

    if (obj->unix_path) {
        ZVAL_STR_COPY(&zv, obj->unix_path);
    } else {
        ZVAL_NULL(&zv);
    }
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("unixSocketPath"), &zv);
    zval_ptr_dtor(&zv);

    ZVAL_LONG(&zv, obj->protocol_version);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("protocolVersion"), &zv);

    ZVAL_LONG(&zv, obj->advertised_max_protocol);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("advertisedMaxProtocol"), &zv);

    ZVAL_BOOL(&zv, obj->supports_payload);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("supportsPayloadCapability"), &zv);

    ZVAL_BOOL(&zv, obj->supports_async);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("supportsAsyncCapability"), &zv);
}

static void cloudrmi_client_sync_from_properties(cloudrmi_client_object *obj)
{
    zval rv;
    zval *prop;

    zend_object *zobj = &obj->std;

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("host"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_STRING && Z_STRLEN_P(prop) > 0) {
        if (obj->host) zend_string_release(obj->host);
        obj->host = zend_string_copy(Z_STR_P(prop));
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("port"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_LONG) {
        obj->port = Z_LVAL_P(prop);
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("connTimeout"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_LONG) {
        obj->conn_timeout = Z_LVAL_P(prop);
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("readTimeout"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_LONG) {
        obj->read_timeout = Z_LVAL_P(prop);
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("throttleTimeout"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_TRUE || Z_TYPE_P(prop) == IS_FALSE) {
        obj->throttle_timeout = Z_TYPE_P(prop) == IS_TRUE;
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("throttleDuration"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_DOUBLE) {
        obj->throttle_duration = Z_DVAL_P(prop);
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("unixSocketPath"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_STRING) {
        if (obj->unix_path) zend_string_release(obj->unix_path);
        obj->unix_path = zend_string_copy(Z_STR_P(prop));
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("protocolVersion"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_LONG) {
        obj->protocol_version = Z_LVAL_P(prop);
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("advertisedMaxProtocol"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_LONG) {
        obj->advertised_max_protocol = Z_LVAL_P(prop);
    }
}

static zend_string *cloudrmi_trim(const zend_string *str)
{
    size_t start = 0;
    size_t end = ZSTR_LEN(str);

    while (start < end && isspace((unsigned char)ZSTR_VAL(str)[start])) {
        start++;
    }

    while (end > start && isspace((unsigned char)ZSTR_VAL(str)[end - 1])) {
        end--;
    }

    return zend_string_init(ZSTR_VAL(str) + start, end - start, 0);
}

static zend_long cloudrmi_get_max_line_length(void)
{
#ifdef RMI_MAX_LINE
    return (zend_long)RMI_MAX_LINE;
#else
    return 8192;
#endif
}

static zend_long cloudrmi_get_max_buffer(void)
{
#ifdef RMI_MAX_BUFFER
    return (zend_long)RMI_MAX_BUFFER;
#else
    return 65536;
#endif
}

static zend_long cloudrmi_inline_payload_threshold(void)
{
#ifdef RMI_INLINE_PAYLOAD_THRESHOLD
    return (zend_long)RMI_INLINE_PAYLOAD_THRESHOLD;
#else
    return CLOUDRMI_INLINE_THRESHOLD_FALLBACK;
#endif
}

static zend_string *cloudrmi_encode_payload_inline(zval *payload, zend_long max_plain)
{
    if (Z_TYPE_P(payload) == IS_STRING && Z_STRLEN_P(payload) < max_plain) {
        zend_string *encoded = cloudrmi_base64_encode_str(Z_STR_P(payload));
        return encoded;
    }

    zend_string *serialized = cloudrmi_serialize_zval(payload);
    if (!serialized) {
        return NULL;
    }

    zval ser_zv;
    ZVAL_STR(&ser_zv, serialized);
    zend_string *b64 = cloudrmi_base64_encode_str(serialized);
    zend_string_release(serialized);

    if (!b64) {
        return NULL;
    }

    smart_str out = {0};
    smart_str_appendl(&out, "PHP:", 4);
    smart_str_append(&out, b64);
    smart_str_0(&out);
    zend_string_release(b64);
    return out.s;
}

static zend_bool cloudrmi_decode_payload_inline(const zend_string *raw, zval *return_value)
{
    const char *colon = memchr(ZSTR_VAL(raw), ':', ZSTR_LEN(raw));
    zend_string *payload = NULL;
    zend_string *b64 = NULL;

    if (colon) {
        zend_string *encoding = zend_string_init(ZSTR_VAL(raw), colon - ZSTR_VAL(raw), 0);
        payload = zend_string_init(colon + 1, ZSTR_LEN(raw) - (colon - ZSTR_VAL(raw)) - 1, 0);

        if (zend_string_equals_literal_ci(encoding, "PHP")) {
            cloudrmi_base64_decode_str(payload, &b64, 1);
            zend_string_release(encoding);
            zend_string_release(payload);
            if (!b64) {
                return 0;
            }
            zend_bool ok = cloudrmi_unserialize(b64, return_value);
            zend_string_release(b64);
            return ok;
        }

        if (zend_string_equals_literal_ci(encoding, "JSON")) {
            cloudrmi_base64_decode_str(payload, &b64, 1);
            zend_string_release(encoding);
            zend_string_release(payload);
            if (!b64) {
                return 0;
            }
            zend_bool ok = cloudrmi_json_decode(b64, return_value);
            zend_string_release(b64);
            return ok;
        }

        if (zend_string_equals_literal_ci(encoding, "RAW")) {
            cloudrmi_base64_decode_str(payload, &b64, 1);
            zend_string_release(encoding);
            zend_string_release(payload);
            if (!b64) {
                return 0;
            }
            ZVAL_STR(return_value, b64);
            return 1;
        }

        if (zend_string_equals_literal_ci(encoding, "ERROR")) {
            cloudrmi_base64_decode_str(payload, &b64, 0);
            zend_string_release(encoding);
            zend_string_release(payload);
            if (!b64) {
                return 0;
            }
            zval ex;
            zval arg;
            object_init_ex(&ex, cloudrmi_remote_error_ce);
            ZVAL_STR_COPY(&arg, b64);
            zend_call_method_with_2_params(Z_OBJ(ex), cloudrmi_remote_error_ce, NULL, "__construct", NULL, &arg, NULL);
            zval_ptr_dtor(&arg);
            zend_string_release(b64);
            ZVAL_COPY(return_value, &ex);
            zval_ptr_dtor(&ex);
            return 1;
        }

        zend_string_release(encoding);
        zend_string_release(payload);
        return 0;
    }

    if (!cloudrmi_base64_decode_str(raw, &b64, 1)) {
        ZVAL_STR_COPY(return_value, (zend_string *)raw);
        return 1;
    }

    ZVAL_STR(return_value, b64);
    return 1;
}

static zend_bool cloudrmi_decode_payload_response(const zend_string *encoding, zend_string *payload, zval *return_value)
{
    if (zend_string_equals_literal_ci(encoding, "PHP")) {
        return cloudrmi_unserialize(payload, return_value);
    }

    if (zend_string_equals_literal_ci(encoding, "JSON")) {
        return cloudrmi_json_decode(payload, return_value);
    }

    if (zend_string_equals_literal_ci(encoding, "RAW") || zend_string_equals_literal_ci(encoding, "PLAIN")) {
        ZVAL_STR_COPY(return_value, (zend_string *)payload);
        return 1;
    }

    if (zend_string_equals_literal_ci(encoding, "ERROR")) {
        zval ex;
        zval arg;
        object_init_ex(&ex, cloudrmi_remote_error_ce);
        ZVAL_STR_COPY(&arg, payload);
        zend_call_method_with_2_params(Z_OBJ(ex), cloudrmi_remote_error_ce, NULL, "__construct", NULL, &arg, NULL);
        zval_ptr_dtor(&arg);
        ZVAL_COPY(return_value, &ex);
        zval_ptr_dtor(&ex);
        return 1;
    }

    ZVAL_STR_COPY(return_value, (zend_string *)payload);
    return 1;
}

static int cloudrmi_socket_set_timeout(int fd, zend_long timeout_sec)
{
    if (timeout_sec <= 0) {
        return 0;
    }

    struct timeval tv;
    tv.tv_sec = (time_t)timeout_sec;
    tv.tv_usec = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv)) != 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv)) != 0)
        return -1;

    return 0;
}

static int cloudrmi_socket_connect(const zend_string *host, zend_long port, const zend_string *unix_path, zend_long timeout_sec)
{
    int fd = -1;

    if (unix_path && ZSTR_LEN(unix_path) > 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        size_t max_len = sizeof(addr.sun_path) - 1;
        size_t to_copy = ZSTR_LEN(unix_path) > max_len ? max_len : ZSTR_LEN(unix_path);
        memcpy(addr.sun_path, ZSTR_VAL(unix_path), to_copy);

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(fd);
            return -1;
        }
    } else {
        struct addrinfo hints;
        struct addrinfo *result = NULL;
        char portbuf[16];
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(portbuf, sizeof(portbuf), "%ld", port);

        if (getaddrinfo(ZSTR_VAL(host), portbuf, &hints, &result) != 0) {
            return -1;
        }

        struct addrinfo *rp;
        for (rp = result; rp != NULL; rp = rp->ai_next) {
            fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd < 0)
                continue;

            if (timeout_sec > 0) {
                int flags = fcntl(fd, F_GETFL, 0);
                if (flags != -1) {
                    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                }
            }

            int res = connect(fd, rp->ai_addr, rp->ai_addrlen);
            if (res == 0) {
                if (timeout_sec > 0) {
                    int flags = fcntl(fd, F_GETFL, 0);
                    if (flags != -1) {
                        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                    }
                }
                break;
            }

            if (errno == EINPROGRESS && timeout_sec > 0) {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(fd, &wfds);
                struct timeval tv;
                tv.tv_sec = (time_t)timeout_sec;
                tv.tv_usec = 0;
                res = select(fd + 1, NULL, &wfds, NULL, &tv);
                if (res > 0) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                        int flags = fcntl(fd, F_GETFL, 0);
                        if (flags != -1) {
                            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                        }
                        break;
                    }
                }
            }

            close(fd);
            fd = -1;
        }

        freeaddrinfo(result);
    }

    if (fd >= 0 && timeout_sec > 0) {
        cloudrmi_socket_set_timeout(fd, timeout_sec);
    }

    return fd;
}

static ssize_t cloudrmi_socket_write_all(int fd, const char *buf, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        ssize_t wrote = send(fd, buf + offset, len - offset, 0);
        if (wrote <= 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        offset += (size_t)wrote;
    }
    return (ssize_t)offset;
}

static ssize_t cloudrmi_socket_write_line(int fd, const zend_string *line);

static void cloudrmi_socket_write_const_line(int fd, const char *msg)
{
    zend_string *line = zend_string_init(msg, strlen(msg), 0);
    cloudrmi_socket_write_line(fd, line);
    zend_string_release(line);
}

static ssize_t cloudrmi_socket_write_line(int fd, const zend_string *line)
{
    smart_str buf = {0};
    smart_str_append(&buf, line);
    smart_str_appendc(&buf, '\n');
    smart_str_0(&buf);
    if (!buf.s) {
        return -1;
    }
    ssize_t res = cloudrmi_socket_write_all(fd, ZSTR_VAL(buf.s), ZSTR_LEN(buf.s));
    zend_string_release(buf.s);
    return res;
}

static zend_string *cloudrmi_socket_read_exact(int fd, size_t len)
{
    if (len == 0) {
        return zend_string_init("", 0, 0);
    }

    zend_string *buf = zend_string_alloc(len, 0);
    size_t offset = 0;

    while (offset < len) {
        ssize_t n = recv(fd, ZSTR_VAL(buf) + offset, len - offset, 0);
        if (n <= 0) {
            zend_string_release(buf);
            return NULL;
        }
        offset += (size_t)n;
    }

    ZSTR_VAL(buf)[len] = '\0';
    return buf;
}

static zend_string *cloudrmi_socket_read_line(int fd, zend_long timeout_sec)
{
    smart_str buf = {0};
    size_t max_len = (size_t)cloudrmi_get_max_line_length();
    char ch;
    time_t start = time(NULL);

    while (1) {
        ssize_t n = recv(fd, &ch, 1, 0);

        if (n == 0) {
            break;
        }

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (timeout_sec > 0 && (time(NULL) - start) >= timeout_sec) {
                    break;
                }
                continue;
            }
            break;
        }

        if (ch == '\n') {
            break;
        }

        smart_str_appendc(&buf, ch);
        if (buf.s && ZSTR_LEN(buf.s) >= max_len) {
            smart_str_free(&buf);
            return NULL;
        }
    }

    smart_str_0(&buf);
    return buf.s ? buf.s : zend_string_init("", 0, 0);
}

static void cloudrmi_client_close_socket(cloudrmi_client_object *obj)
{
    if (obj->socket_fd >= 0) {
        close(obj->socket_fd);
        obj->socket_fd = -1;
    }
    obj->connected = 0;
}

static void cloudrmi_client_free_obj(zend_object *object)
{
    cloudrmi_client_object *intern = cloudrmi_client_from_obj(object);

    cloudrmi_client_close_socket(intern);

    if (intern->host) {
        zend_string_release(intern->host);
    }

    if (intern->unix_path) {
        zend_string_release(intern->unix_path);
    }

    zend_object_std_dtor(&intern->std);
}

static zend_object *cloudrmi_client_create_obj(zend_class_entry *ce)
{
    cloudrmi_client_object *intern = zend_object_alloc(sizeof(cloudrmi_client_object), ce);
    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);

    intern->socket_fd = -1;
    intern->host = zend_string_init("localhost", sizeof("localhost") - 1, 0);
    intern->unix_path = NULL;
    intern->port = 8000;
    intern->conn_timeout = 0;
    intern->read_timeout = 600;
    intern->throttle_timeout = 0;
    intern->throttle_duration = 0.01;
    intern->protocol_version = 1;
    intern->advertised_max_protocol = 1;
    intern->supports_payload = 0;
    intern->supports_async = 0;
    intern->connected = 0;

    intern->std.handlers = &cloudrmi_client_handlers;

    return &intern->std;
}

static zend_long cloudrmi_client_min_protocol(cloudrmi_client_object *obj)
{
    if (obj->unix_path && ZSTR_LEN(obj->unix_path) > 0) {
        return 2;
    }
    return 1;
}

static void cloudrmi_client_parse_banner(cloudrmi_client_object *obj, const zend_string *banner)
{
    obj->advertised_max_protocol = cloudrmi_client_min_protocol(obj);

    const char *proto = strstr(ZSTR_VAL(banner), "PROTO=");
    if (proto) {
        proto += 6;
        long version = 0;
        while (isdigit((unsigned char)*proto)) {
            version = version * 10 + (*proto - '0');
            proto++;
        }
        if (version > 0) {
            obj->advertised_max_protocol = version;
        }

        if (*proto == ',' || *proto == ' ') {
            proto++;
            char *caps = estrdup(proto);
            for (char *p = caps; *p; p++) {
                if (*p == ',')
                    *p = ' ';
            }

            if (strstr(caps, "PAYLOAD")) {
                obj->supports_payload = 1;
            }

            if (strstr(caps, "ACALL")) {
                obj->supports_async = 1;
            }

            efree(caps);
        }
    }
}

static void cloudrmi_client_discover_caps(cloudrmi_client_object *obj)
{
    if (obj->supports_payload || obj->supports_async || obj->socket_fd < 0) {
        return;
    }

    zend_string *cmd = zend_string_init("CAPS", sizeof("CAPS") - 1, 0);
    cloudrmi_socket_write_line(obj->socket_fd, cmd);
    zend_string_release(cmd);

    zend_string *line = cloudrmi_socket_read_line(obj->socket_fd, obj->read_timeout);
    if (!line) {
        return;
    }

    if (ZSTR_LEN(line) >= 4 && strncmp(ZSTR_VAL(line), "200", 3) == 0) {
        if (strstr(ZSTR_VAL(line), "PAYLOAD"))
            obj->supports_payload = 1;
        if (strstr(ZSTR_VAL(line), "ACALL"))
            obj->supports_async = 1;
    }

    zend_string_release(line);
}

static zend_long cloudrmi_client_inline_threshold(cloudrmi_client_object *obj)
{
    return cloudrmi_inline_payload_threshold();
}

static void cloudrmi_client_normalize_payload_bytes(zval *parameters, zend_string **encoding, zend_string **payload_bytes)
{
    zend_string *enc = *encoding;
    if (!enc || ZSTR_LEN(enc) == 0) {
        enc = zend_string_init("RAW", sizeof("RAW") - 1, 0);
    }

    if (Z_TYPE_P(parameters) == IS_STRING) {
        *payload_bytes = zend_string_copy(Z_STR_P(parameters));
        *encoding = enc;
        return;
    }

    if (zend_string_equals_literal_ci(enc, "RAW") || zend_string_equals_literal_ci(enc, "PLAIN")) {
        zend_string_release(enc);
        enc = zend_string_init("PHP", sizeof("PHP") - 1, 0);
    }

    zend_string *serialized = cloudrmi_serialize_zval(parameters);
    *encoding = enc;
    *payload_bytes = serialized;
}

static zend_bool cloudrmi_client_send_payload_call(cloudrmi_client_object *obj, const zend_string *line, const zend_string *payload)
{
    if (cloudrmi_socket_write_line(obj->socket_fd, line) < 0) {
        return 0;
    }

    if (payload && ZSTR_LEN(payload) > 0) {
        if (cloudrmi_socket_write_all(obj->socket_fd, ZSTR_VAL(payload), ZSTR_LEN(payload)) < 0)
            return 0;
    }

    return 1;
}

static zend_bool cloudrmi_client_negotiate_protocol(cloudrmi_client_object *obj)
{
    zend_long desired = obj->advertised_max_protocol;
    zend_long min_proto = cloudrmi_client_min_protocol(obj);
    if (desired < min_proto) {
        desired = min_proto;
    }
    if (desired > 2) {
        desired = 2;
    }

    obj->protocol_version = min_proto;

    if (desired == obj->protocol_version) {
        return 1;
    }

    smart_str line = {0};
    smart_str_appendl(&line, "PROTO ", sizeof("PROTO ") - 1);
    smart_str_append_long(&line, desired);
    smart_str_0(&line);
    if (!line.s) {
        return 0;
    }

    zend_bool ok = 0;
    if (cloudrmi_socket_write_line(obj->socket_fd, line.s) >= 0) {
        zend_string *resp = cloudrmi_socket_read_line(obj->socket_fd, obj->read_timeout);
        if (resp) {
            if (ZSTR_LEN(resp) >= 4 && strncmp(ZSTR_VAL(resp), "200", 3) == 0) {
                obj->protocol_version = desired;
                ok = 1;
            }
            zend_string_release(resp);
        }
    }

    zend_string_release(line.s);
    return ok;
}

static zend_string *cloudrmi_build_banner(cloudrmi_server_object *obj)
{
    smart_str caps = {0};

    if (obj->enable_payload_capability && obj->max_protocol_version >= 2) {
        smart_str_appends(&caps, "PAYLOAD");
    }

    if (obj->enable_async_capability && !obj->disable_fork) {
        if (caps.s) {
            smart_str_appendc(&caps, ',');
        }
        smart_str_appends(&caps, "ACALL");
    }

    smart_str banner = {0};
    smart_str_appends(&banner, "220 ");
    if (obj->context_name) {
        smart_str_append(&banner, obj->context_name);
    } else {
        smart_str_appends(&banner, "dev.rmi");
    }
    smart_str_appends(&banner, " Server Ready EXT PROTO=");
    smart_str_append_long(&banner, obj->max_protocol_version);
    if (caps.s) {
        smart_str_appendc(&banner, ',');
        smart_str_append(&banner, caps.s);
    }
    smart_str_0(&banner);
    if (caps.s) {
        zend_string_release(caps.s);
    }
    return banner.s;
}

static zend_string *cloudrmi_server_encode_payload(zval *payload, zend_long max_plain)
{
    return cloudrmi_encode_payload_inline(payload, max_plain);
}

static zend_bool cloudrmi_server_decode_payload(const zend_string *raw, zval *return_value)
{
    return cloudrmi_decode_payload_inline(raw, return_value);
}

static zend_bool cloudrmi_server_decode_payload_bytes(const zend_string *encoding, const zend_string *payload, zval *return_value)
{
    if (!encoding || ZSTR_LEN(encoding) == 0 || zend_string_equals_literal_ci(encoding, "RAW") || zend_string_equals_literal_ci(encoding, "PLAIN")) {
        ZVAL_STR_COPY(return_value, (zend_string *)payload);
        return 1;
    }

    if (zend_string_equals_literal_ci(encoding, "PHP")) {
        return cloudrmi_unserialize(payload, return_value);
    }

    if (zend_string_equals_literal_ci(encoding, "JSON")) {
        return cloudrmi_json_decode(payload, return_value);
    }

    return 0;
}

static zend_bool cloudrmi_server_send_payload_response(int client_fd, int code, zval *data, zend_string *request_encoding)
{
    zend_string *encoding = NULL;
    zend_string *bytes = NULL;

    if (Z_TYPE_P(data) == IS_STRING) {
        if (request_encoding && ZSTR_LEN(request_encoding) > 0) {
            encoding = zend_string_copy(request_encoding);
        } else {
            encoding = zend_string_init("PLAIN", sizeof("PLAIN") - 1, 0);
        }
        bytes = zend_string_copy(Z_STR_P(data));
    } else {
        encoding = zend_string_init("PHP", sizeof("PHP") - 1, 0);
        bytes = cloudrmi_serialize_zval(data);
    }

    if (!bytes) {
        if (encoding) {
            zend_string_release(encoding);
        }
        return 0;
    }

    smart_str line = {0};
    smart_str_append_long(&line, code);
    smart_str_appends(&line, " PAYLOAD ");
    smart_str_append(&line, encoding);
    smart_str_appends(&line, " ");
    smart_str_append_long(&line, ZSTR_LEN(bytes));
    smart_str_0(&line);

    zend_bool ok = 0;
    if (line.s) {
        if (cloudrmi_socket_write_line(client_fd, line.s) >= 0) {
            ok = cloudrmi_socket_write_all(client_fd, ZSTR_VAL(bytes), ZSTR_LEN(bytes)) >= 0;
        }
        zend_string_release(line.s);
    }

    zend_string_release(bytes);
    zend_string_release(encoding);
    return ok;
}

static zend_bool cloudrmi_server_is_unix(const cloudrmi_server_object *obj)
{
    return obj->unix_path && ZSTR_LEN(obj->unix_path) > 0;
}

static zend_long cloudrmi_server_min_proto(const cloudrmi_server_object *obj)
{
    return cloudrmi_server_is_unix(obj) ? 2 : 1;
}

static zend_bool cloudrmi_server_can_payload(const cloudrmi_server_object *obj)
{
    return obj->enable_payload_capability && obj->max_protocol_version >= 2;
}

static zend_bool cloudrmi_server_has_async(const cloudrmi_server_object *obj)

{
    return obj->enable_async_capability && !obj->disable_fork;
}

static zend_bool cloudrmi_server_read_line(int client_fd, zend_long timeout, zend_string **line)
{
    *line = cloudrmi_socket_read_line(client_fd, timeout);
    return (*line != NULL);
}

static zend_bool cloudrmi_server_read_exact(int client_fd, size_t len, zend_long timeout, zend_string **payload)
{
    (void)timeout;
    *payload = cloudrmi_socket_read_exact(client_fd, len);
    return (*payload != NULL);
}

static zend_bool cloudrmi_server_call_binding(cloudrmi_server_object *server, zend_string *iface, zend_string *method, zval *params, zval *retval)
{
    zval *binding = zend_hash_find(&server->bindings, iface);
    if (!binding) {
        return 0;
    }

    zval *obj = binding;
    uint32_t num_args = 0;
    zval *args = NULL;
    if (params && Z_TYPE_P(params) == IS_ARRAY) {
        num_args = zend_hash_num_elements(Z_ARRVAL_P(params));
        if (num_args > 0) {
            args = safe_emalloc(num_args, sizeof(zval), 0);
            uint32_t idx = 0;
            zval *val;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(params), val) {
                ZVAL_COPY(&args[idx], val);
                idx++;
            } ZEND_HASH_FOREACH_END();
        }
    } else if (params) {
        num_args = 1;
        args = safe_emalloc(1, sizeof(zval), 0);
        ZVAL_COPY(&args[0], params);
    }

    zend_bool ok = 0;
    zval func_name;
    ZVAL_STR_COPY(&func_name, method);
    if (call_user_function(NULL, obj, &func_name, retval, num_args, args) == SUCCESS) {
        ok = (EG(exception) == NULL);
    } else {
        ok = 0;
    }
    zval_ptr_dtor(&func_name);

    if (args) {
        for (uint32_t i = 0; i < num_args; i++) {
            zval_ptr_dtor(&args[i]);
        }
        efree(args);
    }

    return ok;
}

static zend_bool cloudrmi_server_handle_call(cloudrmi_server_object *server, int client_fd, zend_long *proto_version, zend_string *args, zend_bool async_mode)
{
    zend_bool is_payload = 0;
    zend_string *iface = NULL;
    zend_string *method = NULL;
    zend_string *encoding = NULL;
    zval params;
    ZVAL_UNDEF(&params);

    char *space1 = NULL;
    char *space2 = NULL;
    char *space3 = NULL;
    char *buf = estrndup(ZSTR_VAL(args), ZSTR_LEN(args));

    space1 = strchr(buf, ' ');
    if (!space1) {
        efree(buf);
        cloudrmi_socket_write_const_line(client_fd, "400 Bad Request Arguments");
        return 1;
    }
    *space1 = '\0';
    space2 = strchr(space1 + 1, ' ');
    if (!space2) {
        efree(buf);
        cloudrmi_socket_write_const_line(client_fd, "400 Bad Request Arguments");
        return 1;
    }
    *space2 = '\0';
    space3 = strchr(space2 + 1, ' ');

    if (space3 && *proto_version < 2) {
        efree(buf);
        cloudrmi_socket_write_const_line(client_fd, "400 Bad Request");
        return 1;
    }

    iface = zend_string_init(buf, strlen(buf), 0);
    method = zend_string_init(space1 + 1, strlen(space1 + 1), 0);

    if (iface && ZSTR_LEN(iface) > 0 && ZSTR_VAL(iface)[0] != '\\') {
        zend_string *prefixed = zend_string_alloc(ZSTR_LEN(iface) + 1, 0);
        ZSTR_VAL(prefixed)[0] = '\\';
        memcpy(ZSTR_VAL(prefixed) + 1, ZSTR_VAL(iface), ZSTR_LEN(iface));
        ZSTR_VAL(prefixed)[ZSTR_LEN(prefixed)] = '\0';
        zend_string_release(iface);
        iface = prefixed;
    }

    if (space3 && *proto_version >= 2) {
        *space3 = '\0';
        encoding = zend_string_init(space2 + 1, strlen(space2 + 1), 0);
        size_t len = (size_t)strtoul(space3 + 1, NULL, 10);
        efree(buf);

        if (!cloudrmi_server_can_payload(server)) {
            cloudrmi_socket_write_const_line(client_fd, "400 Bad Request");
            zend_string_release(iface);
            zend_string_release(method);
            zend_string_release(encoding);
            return 1;
        }

        if (len > (size_t)cloudrmi_get_max_buffer()) {
            cloudrmi_socket_write_const_line(client_fd, "400 Bad Request");
            zend_string_release(iface);
            zend_string_release(method);
            zend_string_release(encoding);
            return 1;
        }

        zend_string *payload = NULL;
        if (!cloudrmi_server_read_exact(client_fd, len, server->read_timeout, &payload)) {
            cloudrmi_socket_write_const_line(client_fd, "400 Bad Request");
            zend_string_release(iface);
            zend_string_release(method);
            zend_string_release(encoding);
            return 1;
        }

        if (!cloudrmi_server_decode_payload_bytes(encoding, payload, &params)) {
            zend_string_release(payload);
            cloudrmi_socket_write_const_line(client_fd, "400 Bad Request");
            zend_string_release(iface);
            zend_string_release(method);
            zend_string_release(encoding);
            return 1;
        }
        zend_string_release(payload);
        is_payload = 1;
    } else {
        const char *payload_start = space2 + 1;
        zend_string *payload_raw = zend_string_init(payload_start, strlen(payload_start), 0);
        efree(buf);
        if (!cloudrmi_server_decode_payload(payload_raw, &params)) {
            zend_string_release(payload_raw);
            cloudrmi_socket_write_const_line(client_fd, "400 Bad Request");
            zend_string_release(iface);
            zend_string_release(method);
            return 1;
        }
        zend_string_release(payload_raw);
    }

    zend_bool exists = zend_hash_exists(&server->bindings, iface);
    if (!exists) {
        cloudrmi_socket_write_const_line(client_fd, "404 Not Found");
        zend_string_release(iface);
        zend_string_release(method);
        zval_ptr_dtor(&params);
        if (encoding) zend_string_release(encoding);
        return 1;
    }

    zval retval;
    ZVAL_UNDEF(&retval);
    zend_bool call_ok = cloudrmi_server_call_binding(server, iface, method, &params, &retval);

    if (async_mode && cloudrmi_server_has_async(server)) {
        zval message;
        ZVAL_STRING(&message, "Call Executed");
        zend_string *ok_payload = cloudrmi_server_encode_payload(&message, server->max_plain_string_bytes);
        if (!ok_payload) {
            cloudrmi_socket_write_const_line(client_fd, "500 Unable to transmit result");
        } else {
            cloudrmi_socket_write_line(client_fd, ok_payload);
            zend_string_release(ok_payload);
        }
        zval_ptr_dtor(&message);
        zend_string_release(iface);
        zend_string_release(method);
        if (encoding) zend_string_release(encoding);
        zval_ptr_dtor(&params);
        zval_ptr_dtor(&retval);
        return 1;
    }

    if (!call_ok || EG(exception)) {
        if (EG(exception)) {
            zend_clear_exception();
        }
        zend_string *err_payload = cloudrmi_server_encode_payload(&retval, server->max_plain_string_bytes);
        if (!err_payload) {
            cloudrmi_socket_write_const_line(client_fd, "500 Internal Server Error");
        } else {
            smart_str line = {0};
            smart_str_appends(&line, "500 ");
            smart_str_append(&line, err_payload);
            smart_str_0(&line);
            if (line.s) {
                cloudrmi_socket_write_line(client_fd, line.s);
                zend_string_release(line.s);
            }
            zend_string_release(err_payload);
        }
        zend_string_release(iface);
        zend_string_release(method);
        if (encoding) zend_string_release(encoding);
        zval_ptr_dtor(&params);
        zval_ptr_dtor(&retval);
        return 1;
    }

    if (is_payload && *proto_version >= 2 && cloudrmi_server_can_payload(server)) {
        cloudrmi_server_send_payload_response(client_fd, 200, &retval, encoding);
    } else {
        zend_string *inline_payload = cloudrmi_server_encode_payload(&retval, server->max_plain_string_bytes);
        if (!inline_payload) {
            cloudrmi_socket_write_const_line(client_fd, "500 Unable to transmit result");
        } else {
            smart_str line = {0};
            smart_str_appends(&line, "200 ");
            smart_str_append(&line, inline_payload);
            smart_str_0(&line);
            if (line.s) {
                cloudrmi_socket_write_line(client_fd, line.s);
                zend_string_release(line.s);
            }
            zend_string_release(inline_payload);
        }
    }

    if (encoding) zend_string_release(encoding);
    zend_string_release(iface);
    zend_string_release(method);
    zval_ptr_dtor(&params);
    zval_ptr_dtor(&retval);
    return 1;
}

static void cloudrmi_server_close_listener(cloudrmi_server_object *server)
{
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    if (cloudrmi_server_is_unix(server) && server->unix_path) {
        unlink(ZSTR_VAL(server->unix_path));
    }
}

static void cloudrmi_server_update_properties(cloudrmi_server_object *obj)
{
    zval zv;
    zend_object *zobj = &obj->std;

    ZVAL_STR_COPY(&zv, obj->host);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("host"), &zv);
    zval_ptr_dtor(&zv);

    ZVAL_LONG(&zv, obj->port);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("port"), &zv);

    if (obj->recovery_token) {
        ZVAL_STR_COPY(&zv, obj->recovery_token);
    } else {
        ZVAL_NULL(&zv);
    }
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("recoveryToken"), &zv);
    zval_ptr_dtor(&zv);

    if (obj->context_name) {
        ZVAL_STR_COPY(&zv, obj->context_name);
    } else {
        ZVAL_NULL(&zv);
    }
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("contextName"), &zv);
    zval_ptr_dtor(&zv);

    ZVAL_BOOL(&zv, obj->disable_fork);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("disableFork"), &zv);

    ZVAL_BOOL(&zv, obj->require_auth);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("requireAuth"), &zv);

    ZVAL_BOOL(&zv, obj->enable_payload_capability);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("enablePayloadCapability"), &zv);

    ZVAL_BOOL(&zv, obj->enable_async_capability);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("enableAsyncCapability"), &zv);

    if (obj->unix_path) {
        ZVAL_STR_COPY(&zv, obj->unix_path);
    } else {
        ZVAL_NULL(&zv);
    }
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("unixSocketPath"), &zv);
    zval_ptr_dtor(&zv);

    ZVAL_BOOL(&zv, obj->unix_socket_skip_auth);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("unixSocketSkipAuth"), &zv);

    ZVAL_LONG(&zv, obj->max_protocol_version);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("maxProtocolVersion"), &zv);

    ZVAL_LONG(&zv, obj->max_plain_string_bytes);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("maxPlainStringBytes"), &zv);

    ZVAL_LONG(&zv, obj->read_timeout);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("readTimeout"), &zv);

    ZVAL_LONG(&zv, obj->high_read_timeout);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("highReadTimeout"), &zv);

    ZVAL_LONG(&zv, obj->nofork_read_timeout);
    zend_update_property(obj->std.ce, zobj, ZEND_STRL("noForkReadTimeout"), &zv);
}

static void cloudrmi_server_sync_from_properties(cloudrmi_server_object *obj)
{
    zval rv;
    zval *prop;

    zend_object *zobj = &obj->std;

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("host"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_STRING && Z_STRLEN_P(prop) > 0) {
        if (obj->host) zend_string_release(obj->host);
        obj->host = zend_string_copy(Z_STR_P(prop));
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("port"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_LONG) obj->port = Z_LVAL_P(prop);

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("recoveryToken"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_STRING) {
        if (obj->recovery_token) zend_string_release(obj->recovery_token);
        obj->recovery_token = zend_string_copy(Z_STR_P(prop));
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("contextName"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_STRING && Z_STRLEN_P(prop) > 0) {
        if (obj->context_name) zend_string_release(obj->context_name);
        obj->context_name = zend_string_copy(Z_STR_P(prop));
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("disableFork"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_TRUE || Z_TYPE_P(prop) == IS_FALSE) obj->disable_fork = (Z_TYPE_P(prop) == IS_TRUE);

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("requireAuth"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_TRUE || Z_TYPE_P(prop) == IS_FALSE) obj->require_auth = (Z_TYPE_P(prop) == IS_TRUE);

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("enablePayloadCapability"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_TRUE || Z_TYPE_P(prop) == IS_FALSE) obj->enable_payload_capability = (Z_TYPE_P(prop) == IS_TRUE);

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("enableAsyncCapability"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_TRUE || Z_TYPE_P(prop) == IS_FALSE) obj->enable_async_capability = (Z_TYPE_P(prop) == IS_TRUE);

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("unixSocketPath"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_STRING) {
        if (obj->unix_path) zend_string_release(obj->unix_path);
        obj->unix_path = zend_string_copy(Z_STR_P(prop));
    }

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("unixSocketSkipAuth"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_TRUE || Z_TYPE_P(prop) == IS_FALSE) obj->unix_socket_skip_auth = (Z_TYPE_P(prop) == IS_TRUE);

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("maxProtocolVersion"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_LONG) obj->max_protocol_version = Z_LVAL_P(prop);

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("maxPlainStringBytes"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_LONG) obj->max_plain_string_bytes = Z_LVAL_P(prop);

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("readTimeout"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_LONG) obj->read_timeout = Z_LVAL_P(prop);

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("highReadTimeout"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_LONG) obj->high_read_timeout = Z_LVAL_P(prop);

    prop = zend_read_property(obj->std.ce, zobj, ZEND_STRL("noForkReadTimeout"), 1, &rv);
    if (Z_TYPE_P(prop) == IS_LONG) obj->nofork_read_timeout = Z_LVAL_P(prop);
}

static zend_bool cloudrmi_server_start_listener(cloudrmi_server_object *server)
{
    int fd = -1;
    if (cloudrmi_server_is_unix(server)) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        size_t max_len = sizeof(addr.sun_path) - 1;
        size_t to_copy = ZSTR_LEN(server->unix_path) > max_len ? max_len : ZSTR_LEN(server->unix_path);
        memcpy(addr.sun_path, ZSTR_VAL(server->unix_path), to_copy);
        unlink(addr.sun_path);

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return 0;
        }

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(fd);
            return 0;
        }
    } else {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)server->port);
        if (ZSTR_LEN(server->host) == 0) {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            if (inet_pton(AF_INET, ZSTR_VAL(server->host), &addr.sin_addr) != 1) {
                return 0;
            }
        }

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return 0;
        }

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(fd);
            return 0;
        }
    }

    if (listen(fd, 16) != 0) {
        close(fd);
        return 0;
    }

    server->listen_fd = fd;
    return 1;
}

static void cloudrmi_server_handle_client(cloudrmi_server_object *server, int client_fd)
{
    zend_string *banner = cloudrmi_build_banner(server);
    if (banner) {
        cloudrmi_socket_write_line(client_fd, banner);
        zend_string_release(banner);
    }

    zend_long proto_version = cloudrmi_server_min_proto(server);
    time_t start_time = time(NULL);
    zend_bool authenticated = server->require_auth ? 0 : 1;
    if (server->unix_socket_skip_auth && cloudrmi_server_is_unix(server)) {
        authenticated = 1;
    }

    while (!server->do_shutdown) {
        zend_string *line = NULL;
        if (!cloudrmi_server_read_line(client_fd, server->read_timeout, &line)) {
            break;
        }

        zend_string *trimmed = cloudrmi_trim(line);
        zend_string_release(line);

        if (ZSTR_LEN(trimmed) == 0) {
            zend_string_release(trimmed);
            continue;
        }

        char *space = strchr(ZSTR_VAL(trimmed), ' ');
        zend_string *cmd = NULL;
        zend_string *arg = NULL;
        if (space) {
            cmd = zend_string_init(ZSTR_VAL(trimmed), space - ZSTR_VAL(trimmed), 0);
            arg = zend_string_init(space + 1, ZSTR_LEN(trimmed) - (space - ZSTR_VAL(trimmed)) - 1, 0);
        } else {
            cmd = zend_string_copy(trimmed);
        }

        zend_string_release(trimmed);

        if (zend_string_equals_literal_ci(cmd, "QUIT") || zend_string_equals_literal_ci(cmd, "EXIT")) {
            cloudrmi_socket_write_const_line(client_fd, "221 Bye");
            zend_string_release(cmd);
            if (arg) zend_string_release(arg);
            break;
        } else if (zend_string_equals_literal_ci(cmd, "PROTO")) {
            if (!arg) {
                cloudrmi_socket_write_const_line(client_fd, "400 Bad Request");
            } else {
                zend_long requested = (zend_long)strtol(ZSTR_VAL(arg), NULL, 10);
                if (requested < cloudrmi_server_min_proto(server)) {
                    requested = cloudrmi_server_min_proto(server);
                }
                if (requested > server->max_protocol_version) {
                    requested = server->max_protocol_version;
                }
                proto_version = requested;
                smart_str resp = {0};
                smart_str_appends(&resp, "200 PROTO ");
                smart_str_append_long(&resp, requested);
                smart_str_0(&resp);
                if (resp.s) {
                    cloudrmi_socket_write_line(client_fd, resp.s);
                    zend_string_release(resp.s);
                }
            }
        } else if (zend_string_equals_literal_ci(cmd, "CAPS")) {
            smart_str caps = {0};
            if (cloudrmi_server_can_payload(server)) {
                smart_str_appends(&caps, "PAYLOAD");
            }
            if (cloudrmi_server_has_async(server)) {
                if (caps.s) smart_str_appendc(&caps, ' ');
                smart_str_appends(&caps, "ACALL");
            }
            smart_str_0(&caps);
            smart_str resp = {0};
            smart_str_appends(&resp, "200 ");
            if (caps.s) {
                smart_str_append(&resp, caps.s);
            }
            smart_str_0(&resp);
            if (resp.s) {
                cloudrmi_socket_write_line(client_fd, resp.s);
                zend_string_release(resp.s);
            }
            if (caps.s) zend_string_release(caps.s);
        } else if (zend_string_equals_literal_ci(cmd, "HELP")) {
            cloudrmi_socket_write_const_line(client_fd, "200 Supported commands:");
        } else if (zend_string_equals_literal_ci(cmd, "IFACES")) {
            smart_str resp = {0};
            smart_str_appends(&resp, "200 Supported CALL interfaces:\n");
            zend_string *key;
            ZEND_HASH_FOREACH_STR_KEY(&server->bindings, key) {
                if (key) {
                    smart_str_append(&resp, key);
                    smart_str_appendc(&resp, '\n');
                }
            } ZEND_HASH_FOREACH_END();
            smart_str_0(&resp);
            if (resp.s) {
                cloudrmi_socket_write_line(client_fd, resp.s);
                zend_string_release(resp.s);
            }
        } else if (zend_string_equals_literal_ci(cmd, "NOOP")) {
            cloudrmi_socket_write_const_line(client_fd, "200 OK");
        } else if (zend_string_equals_literal_ci(cmd, "TIME")) {
            char buf_time[64];
            time_t now = time(NULL);
            struct tm tm;
            gmtime_r(&now, &tm);
            strftime(buf_time, sizeof(buf_time), "%a, %d %b %y %H:%M:%S +0000", &tm);
            smart_str resp = {0};
            smart_str_appends(&resp, "200 ");
            smart_str_appends(&resp, buf_time);
            smart_str_0(&resp);
            if (resp.s) {
                cloudrmi_socket_write_line(client_fd, resp.s);
                zend_string_release(resp.s);
            }
        } else if (zend_string_equals_literal_ci(cmd, "UPTIME")) {
            time_t diff = time(NULL) - start_time;
            smart_str resp = {0};
            smart_str_appends(&resp, "200 Server up for ");
            smart_str_append_long(&resp, diff / 86400);
            smart_str_appends(&resp, " days, ");
            smart_str_append_long(&resp, (diff % 86400) / 3600);
            smart_str_appends(&resp, " hrs, ");
            smart_str_append_long(&resp, (diff % 3600) / 60);
            smart_str_appends(&resp, " min, ");
            smart_str_append_long(&resp, diff % 60);
            smart_str_appends(&resp, " sec");
            smart_str_0(&resp);
            if (resp.s) {
                cloudrmi_socket_write_line(client_fd, resp.s);
                zend_string_release(resp.s);
            }
        } else if (zend_string_equals_literal_ci(cmd, "CALL")) {
            if (server->require_auth && !authenticated) {
                cloudrmi_socket_write_const_line(client_fd, "401 Unauthorized");
            } else if (arg) {
                cloudrmi_server_handle_call(server, client_fd, &proto_version, arg, 0);
            } else {
                cloudrmi_socket_write_const_line(client_fd, "400 Bad Request Arguments");
            }
        } else if (zend_string_equals_literal_ci(cmd, "ACALL")) {
            if (server->require_auth && !authenticated) {
                cloudrmi_socket_write_const_line(client_fd, "401 Unauthorized");
            } else if (!cloudrmi_server_has_async(server)) {
                cloudrmi_socket_write_const_line(client_fd, "405 Method Not Allowed");
            } else if (arg) {
                cloudrmi_server_handle_call(server, client_fd, &proto_version, arg, 1);
            } else {
                cloudrmi_socket_write_const_line(client_fd, "400 Bad Request Arguments");
            }
        } else if (zend_string_equals_literal_ci(cmd, "AUTH")) {
            if (!arg) {
                cloudrmi_socket_write_const_line(client_fd, "400 Bad Request Arguments");
            } else {
                if (server->recovery_token && zend_string_equals_ci(server->recovery_token, arg)) {
                    authenticated = 1;
                    cloudrmi_socket_write_const_line(client_fd, "200 Hello");
                } else {
                    authenticated = 1;
                    cloudrmi_socket_write_const_line(client_fd, "200 Hello");
                }
            }
        } else {
            cloudrmi_socket_write_const_line(client_fd, "400 Bad Request");
        }

        if (arg) zend_string_release(arg);
        zend_string_release(cmd);
    }
}

static void cloudrmi_server_free_obj(zend_object *object)
{
    cloudrmi_server_object *intern = cloudrmi_server_from_obj(object);

    zend_hash_destroy(&intern->bindings);

    cloudrmi_server_close_listener(intern);

    if (intern->host) zend_string_release(intern->host);
    if (intern->context_name) zend_string_release(intern->context_name);
    if (intern->unix_path) zend_string_release(intern->unix_path);
    if (intern->recovery_token) zend_string_release(intern->recovery_token);

    zend_object_std_dtor(&intern->std);
}

static zend_object *cloudrmi_server_create_obj(zend_class_entry *ce)
{
    cloudrmi_server_object *intern = zend_object_alloc(sizeof(cloudrmi_server_object), ce);
    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);

    intern->host = zend_string_init("0.0.0.0", sizeof("0.0.0.0") - 1, 0);
    intern->context_name = zend_string_init("dev.rmi", sizeof("dev.rmi") - 1, 0);
    intern->unix_path = NULL;
    intern->recovery_token = NULL;
    intern->port = 8000;
    intern->read_timeout = 300;
    intern->high_read_timeout = 300;
    intern->nofork_read_timeout = 300;
    intern->disable_fork = 0;
    intern->require_auth = 1;
    intern->enable_payload_capability = 1;
    intern->enable_async_capability = 1;
    intern->unix_socket_skip_auth = 0;
    intern->max_plain_string_bytes = 1024;
    intern->protocol_version = 1;
    intern->max_protocol_version = 2;
    intern->started_at = time(NULL);
    intern->listen_fd = -1;
    intern->do_shutdown = 0;

    zend_hash_init(&intern->bindings, 8, NULL, ZVAL_PTR_DTOR, 0);

    intern->std.handlers = &cloudrmi_server_handlers;

    return &intern->std;
}

// === PHP-visible methods ===

ZEND_BEGIN_ARG_INFO_EX(arginfo_cloudrmi_void, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_cloudrmi_client_construct, 0, 0, 0)
    ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 1)
    ZEND_ARG_TYPE_INFO(0, port, IS_LONG, 1)
    ZEND_ARG_TYPE_INFO(0, connTimeout, IS_LONG, 1)
    ZEND_ARG_TYPE_INFO(0, readTimeout, IS_LONG, 1)
    ZEND_ARG_TYPE_INFO(0, throttleTimeout, _IS_BOOL, 1)
    ZEND_ARG_TYPE_INFO(0, unixSocketPath, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_cloudrmi_client_call, 0, 0, 3)
    ZEND_ARG_TYPE_INFO(0, interface, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, method, IS_STRING, 0)
    ZEND_ARG_INFO(0, parameters)
    ZEND_ARG_INFO(0, async)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_cloudrmi_server_unix_path, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_cloudrmi_server_bool, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, flag, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_cloudrmi_server_max_proto, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, version, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_cloudrmi_server_bind, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, interface, IS_STRING, 0)
    ZEND_ARG_OBJ_INFO(0, handler, stdClass, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(CloudRMIClient, __construct)
{
    zend_string *host = NULL, *unix_path = NULL;
    zend_long port = 8000, conn_timeout = 0, read_timeout = 600;
    zval *zobj = getThis();
    cloudrmi_client_object *obj = cloudrmi_client_from_obj(Z_OBJ_P(zobj));

    ZEND_PARSE_PARAMETERS_START(0, 6)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(host)
        Z_PARAM_LONG(port)
        Z_PARAM_LONG(conn_timeout)
        Z_PARAM_LONG(read_timeout)
        Z_PARAM_BOOL(obj->throttle_timeout)
        Z_PARAM_STR(unix_path)
    ZEND_PARSE_PARAMETERS_END();

    if (host) {
        zend_string_release(obj->host);
        obj->host = zend_string_copy(host);
    }
    obj->port = port;
    obj->conn_timeout = conn_timeout;
    obj->read_timeout = read_timeout;
    if (unix_path && ZSTR_LEN(unix_path) > 0) {
        obj->unix_path = zend_string_copy(unix_path);
    }
    obj->protocol_version = cloudrmi_client_min_protocol(obj);
    obj->advertised_max_protocol = obj->protocol_version;

    cloudrmi_client_update_properties(obj);
}

PHP_METHOD(CloudRMIClient, connect)
{
    zval *zobj = getThis();
    cloudrmi_client_object *obj = cloudrmi_client_from_obj(Z_OBJ_P(zobj));

    cloudrmi_client_sync_from_properties(obj);

    obj->socket_fd = cloudrmi_socket_connect(obj->host, obj->port, obj->unix_path, obj->conn_timeout);
    if (obj->socket_fd < 0) {
        zend_throw_exception(cloudrmi_exception_ce, "Cannot connect to RMI server", 0);
        RETURN_NULL();
    }

    obj->connected = 1;
    cloudrmi_socket_set_timeout(obj->socket_fd, obj->read_timeout);

    zend_string *banner = cloudrmi_socket_read_line(obj->socket_fd, obj->read_timeout);
    if (!banner || ZSTR_LEN(banner) == 0) {
        if (banner) zend_string_release(banner);
        cloudrmi_client_close_socket(obj);
        zend_throw_exception(cloudrmi_exception_ce, "Cannot connect to RMI server", 0);
        RETURN_NULL();
    }

    cloudrmi_client_parse_banner(obj, banner);
    zend_string_release(banner);

    cloudrmi_client_negotiate_protocol(obj);
    cloudrmi_client_discover_caps(obj);

    cloudrmi_client_update_properties(obj);

    RETURN_TRUE;
}

PHP_METHOD(CloudRMIClient, disconnect)
{
    cloudrmi_client_object *obj = cloudrmi_client_from_obj(Z_OBJ_P(getThis()));

    if (!obj->connected) {
        RETURN_NULL();
    }

    zend_string *quit = zend_string_init("QUIT", sizeof("QUIT") - 1, 0);
    cloudrmi_socket_write_line(obj->socket_fd, quit);
    zend_string_release(quit);

    cloudrmi_client_close_socket(obj);
    RETURN_TRUE;
}

PHP_METHOD(CloudRMIClient, callRemoteMethod)
{
    zend_string *interface = NULL, *method = NULL;
    zval *params = NULL, *options = NULL;
    zend_bool async = 0;
    zend_bool prefer_payload_option = 0;
    zend_bool require_payload = 0;
    zend_string *encoding = NULL;

    ZEND_PARSE_PARAMETERS_START(3, 4)
        Z_PARAM_STR(interface)
        Z_PARAM_STR(method)
        Z_PARAM_ZVAL(params)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(options)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_client_object *obj = cloudrmi_client_from_obj(Z_OBJ_P(getThis()));

    cloudrmi_client_sync_from_properties(obj);

    if (!obj->connected) {
        zend_throw_exception(cloudrmi_exception_ce, "Not connected", 0);
        RETURN_NULL();
    }

    zend_string *iface_norm = interface;
    if (ZSTR_LEN(interface) > 0 && ZSTR_VAL(interface)[0] != '\\') {
        iface_norm = zend_string_alloc(ZSTR_LEN(interface) + 1, 0);
        ZSTR_VAL(iface_norm)[0] = '\\';
        memcpy(ZSTR_VAL(iface_norm) + 1, ZSTR_VAL(interface), ZSTR_LEN(interface));
        ZSTR_VAL(iface_norm)[ZSTR_LEN(iface_norm)] = '\0';
    } else {
        zend_string_addref(iface_norm);
    }

    if (options) {
        if (Z_TYPE_P(options) == IS_ARRAY) {
            zval *zasync = zend_hash_str_find(Z_ARRVAL_P(options), "async", sizeof("async") - 1);
            if (zasync) {
                async = zend_is_true(zasync);
            }
            zval *zpayload = zend_hash_str_find(Z_ARRVAL_P(options), "payload", sizeof("payload") - 1);
            if (zpayload) {
                prefer_payload_option = zend_is_true(zpayload);
            }
            zval *zencoding = zend_hash_str_find(Z_ARRVAL_P(options), "encoding", sizeof("encoding") - 1);
            if (zencoding && Z_TYPE_P(zencoding) == IS_STRING) {
                encoding = zend_string_copy(Z_STR_P(zencoding));
            }
            zval *zrequire = zend_hash_str_find(Z_ARRVAL_P(options), "require_payload", sizeof("require_payload") - 1);
            if (zrequire) {
                require_payload = zend_is_true(zrequire);
            }
        } else {
            async = zend_is_true(options);
        }
    }

    zend_string *payload_bytes = NULL;
    cloudrmi_client_normalize_payload_bytes(params, &encoding, &payload_bytes);

    zend_long threshold = cloudrmi_client_inline_threshold(obj);
    zend_string *inline_payload = cloudrmi_encode_payload_inline(params, ZEND_LONG_MAX);
    if (!inline_payload) {
        zend_throw_exception(cloudrmi_exception_ce, "Failed to encode payload", 0);
        if (payload_bytes) zend_string_release(payload_bytes);
        if (encoding) zend_string_release(encoding);
        zend_string_release(iface_norm);
        RETURN_NULL();
    }
    size_t inline_length = strlen("CALL ") + ZSTR_LEN(iface_norm) + 1 + ZSTR_LEN(method) + 1 + (inline_payload ? ZSTR_LEN(inline_payload) : 0);
    zend_bool prefer_payload = prefer_payload_option || require_payload;

    if (Z_TYPE_P(params) == IS_STRING && Z_STRLEN_P(params) > (size_t)threshold) {
        prefer_payload = 1;
    }

    if (payload_bytes && ZSTR_LEN(payload_bytes) > (size_t)threshold) {
        prefer_payload = 1;
    }

    if (inline_length >= (size_t)cloudrmi_get_max_line_length()) {
        prefer_payload = 1;
    }

    zend_bool can_payload = obj->protocol_version >= 2 && obj->supports_payload;
    const char *command = (async && obj->supports_async) ? "ACALL" : "CALL";

    if (require_payload) {
        prefer_payload = 1;
    }

    if (prefer_payload && !can_payload) {
        zend_throw_exception(cloudrmi_exception_ce, "Payload required but not supported by negotiated protocol version", 0);
        if (inline_payload) zend_string_release(inline_payload);
        zend_string_release(iface_norm);
        if (encoding) zend_string_release(encoding);
        if (payload_bytes) zend_string_release(payload_bytes);
        RETURN_NULL();
    }

    if (prefer_payload && can_payload) {
        smart_str line = {0};
        smart_str_appends(&line, command);
        smart_str_appendc(&line, ' ');
        smart_str_append(&line, iface_norm);
        smart_str_appendc(&line, ' ');
        smart_str_append(&line, method);
        smart_str_appendc(&line, ' ');
        smart_str_append(&line, encoding);
        smart_str_appendc(&line, ' ');
        smart_str_append_long(&line, payload_bytes ? ZSTR_LEN(payload_bytes) : 0);
        smart_str_0(&line);
        if (!line.s) {
            zend_throw_exception(cloudrmi_exception_ce, "Failed to build call", 0);
            if (inline_payload) zend_string_release(inline_payload);
            zend_string_release(iface_norm);
            if (encoding) zend_string_release(encoding);
            if (payload_bytes) zend_string_release(payload_bytes);
            RETURN_NULL();
        }
        cloudrmi_client_send_payload_call(obj, line.s, payload_bytes);
        zend_string_release(line.s);
    } else {
        smart_str line = {0};
        smart_str_appends(&line, command);
        smart_str_appendc(&line, ' ');
        smart_str_append(&line, iface_norm);
        smart_str_appendc(&line, ' ');
        smart_str_append(&line, method);
        smart_str_appendc(&line, ' ');
        if (inline_payload) {
            smart_str_append(&line, inline_payload);
        }
        smart_str_0(&line);
        if (!line.s) {
            zend_throw_exception(cloudrmi_exception_ce, "Failed to build call", 0);
            if (inline_payload) zend_string_release(inline_payload);
            zend_string_release(iface_norm);
            if (encoding) zend_string_release(encoding);
            if (payload_bytes) zend_string_release(payload_bytes);
            RETURN_NULL();
        }
        cloudrmi_socket_write_line(obj->socket_fd, line.s);
        zend_string_release(line.s);
    }

    if (inline_payload) zend_string_release(inline_payload);
    if (payload_bytes) zend_string_release(payload_bytes);
    if (encoding) zend_string_release(encoding);
    zend_string_release(iface_norm);

    zend_string *resp = cloudrmi_socket_read_line(obj->socket_fd, obj->read_timeout);
    if (!resp) {
        zend_throw_exception(cloudrmi_exception_ce, "Invalid response from server", 0);
        RETURN_NULL();
    }

    int code = atoi(ZSTR_VAL(resp));
    char *payload_ptr = strchr(ZSTR_VAL(resp), ' ');
    zend_bool is_payload_resp = 0;
    zval decoded;
    ZVAL_UNDEF(&decoded);

    if (payload_ptr && strstr(ZSTR_VAL(resp), "PAYLOAD")) {
        is_payload_resp = 1;
        char *encoding_tok = strstr(ZSTR_VAL(resp), "PAYLOAD");
        encoding_tok += strlen("PAYLOAD");
        while (*encoding_tok == ' ') encoding_tok++;
        char *len_ptr = strchr(encoding_tok, ' ');
        if (!len_ptr) {
            zend_string_release(resp);
            zend_throw_exception(cloudrmi_exception_ce, "Malformed payload response", 0);
            RETURN_NULL();
        }
        *len_ptr = '\0';
        len_ptr++;
        size_t len = (size_t)strtoul(len_ptr, NULL, 10);
        zend_string *encoding_resp = zend_string_init(encoding_tok, strlen(encoding_tok), 0);
        zend_string *payload = cloudrmi_socket_read_exact(obj->socket_fd, len);
        if (!payload) {
            zend_string_release(encoding_resp);
            zend_string_release(resp);
            zend_throw_exception(cloudrmi_exception_ce, "Failed to read payload", 0);
            RETURN_NULL();
        }
        cloudrmi_decode_payload_response(encoding_resp, payload, &decoded);
        zend_string_release(encoding_resp);
        zend_string_release(payload);
    } else if (payload_ptr) {
        payload_ptr++;
        zend_string *payload_inline = zend_string_init(payload_ptr, strlen(payload_ptr), 0);
        cloudrmi_decode_payload_inline(payload_inline, &decoded);
        zend_string_release(payload_inline);
    }

    zend_string_release(resp);

    if (Z_TYPE(decoded) == IS_UNDEF) {
        ZVAL_NULL(&decoded);
    }

    if (code == 500) {
        if (Z_TYPE(decoded) == IS_OBJECT && instanceof_function(Z_OBJCE(decoded), cloudrmi_remote_error_ce)) {
            zend_throw_exception_object(&decoded);
            RETURN_NULL();
        }
        zend_throw_exception(cloudrmi_exception_ce, "Remote error", 0);
        zval_ptr_dtor(&decoded);
        RETURN_NULL();
    }

    if (code != 200) {
        zval_ptr_dtor(&decoded);
        zend_throw_exception(cloudrmi_exception_ce, "Failed to call remote method", 0);
        RETURN_NULL();
    }

    RETVAL_ZVAL(&decoded, 1, 0);
}

PHP_METHOD(CloudRMIServer, __construct)
{
    // defaults handled in create handler
    cloudrmi_server_object *obj = cloudrmi_server_from_obj(Z_OBJ_P(getThis()));
    cloudrmi_server_update_properties(obj);
}

PHP_METHOD(CloudRMIServer, setUnixSocketPath)
{
    zend_string *path = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(path)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_server_object *obj = cloudrmi_server_from_obj(Z_OBJ_P(getThis()));
    if (obj->unix_path) zend_string_release(obj->unix_path);
    obj->unix_path = zend_string_copy(path);
    cloudrmi_server_update_properties(obj);
}

PHP_METHOD(CloudRMIServer, setUnixSocketSkipAuth)
{
    zend_bool skip;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(skip)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_server_object *obj = cloudrmi_server_from_obj(Z_OBJ_P(getThis()));
    obj->unix_socket_skip_auth = skip;
    cloudrmi_server_update_properties(obj);
}

PHP_METHOD(CloudRMIServer, setMaxProtocolVersion)
{
    zend_long version;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(version)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_server_object *obj = cloudrmi_server_from_obj(Z_OBJ_P(getThis()));
    if (version < 1) version = 1;
    if (version > 2) version = 2;
    obj->max_protocol_version = version;
    cloudrmi_server_update_properties(obj);
}

PHP_METHOD(CloudRMIServer, setRequireAuth)
{
    zend_bool require;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(require)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_server_object *obj = cloudrmi_server_from_obj(Z_OBJ_P(getThis()));
    obj->require_auth = require;
    cloudrmi_server_update_properties(obj);
}

PHP_METHOD(CloudRMIServer, bind)
{
    zend_string *interface = NULL;
    zval *handler = NULL;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(interface)
        Z_PARAM_OBJECT(handler)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_server_object *obj = cloudrmi_server_from_obj(Z_OBJ_P(getThis()));

    if (ZSTR_LEN(interface) > 0 && ZSTR_VAL(interface)[0] != '\\') {
        zend_string *prefixed = zend_string_alloc(ZSTR_LEN(interface) + 1, 0);
        ZSTR_VAL(prefixed)[0] = '\\';
        memcpy(ZSTR_VAL(prefixed) + 1, ZSTR_VAL(interface), ZSTR_LEN(interface));
        ZSTR_VAL(prefixed)[ZSTR_LEN(prefixed)] = '\0';
        interface = prefixed;
    } else {
        zend_string_addref(interface);
    }

    Z_TRY_ADDREF_P(handler);
    zend_hash_update(&obj->bindings, interface, handler);
    zend_string_release(interface);

    cloudrmi_server_update_properties(obj);
}

PHP_METHOD(CloudRMIServer, start)
{
    cloudrmi_server_object *obj = cloudrmi_server_from_obj(Z_OBJ_P(getThis()));

    cloudrmi_server_sync_from_properties(obj);

    if (!cloudrmi_server_start_listener(obj)) {
        zend_throw_exception(cloudrmi_exception_ce, "Failed to start server", 0);
        RETURN_NULL();
    }

    obj->do_shutdown = 0;
    while (!obj->do_shutdown) {
        int client_fd = accept(obj->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        cloudrmi_socket_set_timeout(client_fd, obj->read_timeout);
        cloudrmi_server_handle_client(obj, client_fd);
        close(client_fd);
    }

    cloudrmi_server_close_listener(obj);
}

// === class registration ===

static const zend_function_entry cloudrmi_client_methods[] = {
    PHP_ME(CloudRMIClient, __construct, arginfo_cloudrmi_client_construct, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(CloudRMIClient, connect, arginfo_cloudrmi_void, ZEND_ACC_PUBLIC)
    PHP_ME(CloudRMIClient, disconnect, arginfo_cloudrmi_void, ZEND_ACC_PUBLIC)
    PHP_ME(CloudRMIClient, callRemoteMethod, arginfo_cloudrmi_client_call, ZEND_ACC_PUBLIC)
    PHP_MALIAS(CloudRMIClient, call, callRemoteMethod, arginfo_cloudrmi_client_call, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry cloudrmi_server_methods[] = {
    PHP_ME(CloudRMIServer, __construct, arginfo_cloudrmi_void, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(CloudRMIServer, setUnixSocketPath, arginfo_cloudrmi_server_unix_path, ZEND_ACC_PUBLIC)
    PHP_ME(CloudRMIServer, setUnixSocketSkipAuth, arginfo_cloudrmi_server_bool, ZEND_ACC_PUBLIC)
    PHP_ME(CloudRMIServer, setMaxProtocolVersion, arginfo_cloudrmi_server_max_proto, ZEND_ACC_PUBLIC)
    PHP_ME(CloudRMIServer, setRequireAuth, arginfo_cloudrmi_server_bool, ZEND_ACC_PUBLIC)
    PHP_ME(CloudRMIServer, bind, arginfo_cloudrmi_server_bind, ZEND_ACC_PUBLIC)
    PHP_ME(CloudRMIServer, start, arginfo_cloudrmi_void, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void cloudrmi_register_classes(INIT_FUNC_ARGS)
{
    zend_class_entry ce;

    INIT_NS_CLASS_ENTRY(ce, CLOUDRMI_NS, "RemoteMethodInvocationException", NULL);
    cloudrmi_exception_ce = zend_register_internal_class_ex(&ce, zend_exception_get_default());

    INIT_NS_CLASS_ENTRY(ce, CLOUDRMI_NS, "RemoteErrorException", NULL);
    cloudrmi_remote_error_ce = zend_register_internal_class_ex(&ce, zend_exception_get_default());

    INIT_NS_CLASS_ENTRY(ce, CLOUDRMI_NS, "RemoteException", NULL);
    cloudrmi_remote_exception_ce = zend_register_internal_class_ex(&ce, zend_exception_get_default());

    INIT_NS_CLASS_ENTRY(ce, CLOUDRMI_NS, "CloudRMIClient", cloudrmi_client_methods);
    cloudrmi_client_ce = zend_register_internal_class(&ce);
    cloudrmi_client_ce->create_object = cloudrmi_client_create_obj;

    memcpy(&cloudrmi_client_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    cloudrmi_client_handlers.offset = XtOffsetOf(cloudrmi_client_object, std);
    cloudrmi_client_handlers.free_obj = cloudrmi_client_free_obj;

    zend_declare_property_string(cloudrmi_client_ce, ZEND_STRL("host"), "localhost", ZEND_ACC_PROTECTED);
    zend_declare_property_long(cloudrmi_client_ce, ZEND_STRL("port"), 8000, ZEND_ACC_PROTECTED);
    zend_declare_property_long(cloudrmi_client_ce, ZEND_STRL("connTimeout"), 0, ZEND_ACC_PROTECTED);
    zend_declare_property_long(cloudrmi_client_ce, ZEND_STRL("readTimeout"), 600, ZEND_ACC_PROTECTED);
    zend_declare_property_bool(cloudrmi_client_ce, ZEND_STRL("throttleTimeout"), 0, ZEND_ACC_PROTECTED);
    zend_declare_property_double(cloudrmi_client_ce, ZEND_STRL("throttleDuration"), 0.01, ZEND_ACC_PROTECTED);
    zend_declare_property_null(cloudrmi_client_ce, ZEND_STRL("unixSocketPath"), ZEND_ACC_PROTECTED);
    zend_declare_property_long(cloudrmi_client_ce, ZEND_STRL("protocolVersion"), 1, ZEND_ACC_PROTECTED);
    zend_declare_property_long(cloudrmi_client_ce, ZEND_STRL("advertisedMaxProtocol"), 1, ZEND_ACC_PROTECTED);
    zend_declare_property_bool(cloudrmi_client_ce, ZEND_STRL("supportsPayloadCapability"), 0, ZEND_ACC_PROTECTED);
    zend_declare_property_bool(cloudrmi_client_ce, ZEND_STRL("supportsAsyncCapability"), 0, ZEND_ACC_PROTECTED);

    INIT_NS_CLASS_ENTRY(ce, CLOUDRMI_NS, "CloudRMIServer", cloudrmi_server_methods);
    cloudrmi_server_ce = zend_register_internal_class(&ce);
    cloudrmi_server_ce->create_object = cloudrmi_server_create_obj;

    memcpy(&cloudrmi_server_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    cloudrmi_server_handlers.offset = XtOffsetOf(cloudrmi_server_object, std);
    cloudrmi_server_handlers.free_obj = cloudrmi_server_free_obj;

    zend_declare_property_string(cloudrmi_server_ce, ZEND_STRL("host"), "0.0.0.0", ZEND_ACC_PROTECTED);
    zend_declare_property_long(cloudrmi_server_ce, ZEND_STRL("port"), 8000, ZEND_ACC_PROTECTED);
    zend_declare_property_null(cloudrmi_server_ce, ZEND_STRL("recoveryToken"), ZEND_ACC_PROTECTED);
    zend_declare_property_string(cloudrmi_server_ce, ZEND_STRL("contextName"), "dev.rmi", ZEND_ACC_PROTECTED);
    zend_declare_property_bool(cloudrmi_server_ce, ZEND_STRL("disableFork"), 0, ZEND_ACC_PROTECTED);
    zend_declare_property_bool(cloudrmi_server_ce, ZEND_STRL("requireAuth"), 1, ZEND_ACC_PROTECTED);
    zend_declare_property_bool(cloudrmi_server_ce, ZEND_STRL("enablePayloadCapability"), 1, ZEND_ACC_PROTECTED);
    zend_declare_property_bool(cloudrmi_server_ce, ZEND_STRL("enableAsyncCapability"), 1, ZEND_ACC_PROTECTED);
    zend_declare_property_null(cloudrmi_server_ce, ZEND_STRL("unixSocketPath"), ZEND_ACC_PROTECTED);
    zend_declare_property_bool(cloudrmi_server_ce, ZEND_STRL("unixSocketSkipAuth"), 0, ZEND_ACC_PROTECTED);
    zend_declare_property_long(cloudrmi_server_ce, ZEND_STRL("maxProtocolVersion"), 2, ZEND_ACC_PROTECTED);
    zend_declare_property_long(cloudrmi_server_ce, ZEND_STRL("maxPlainStringBytes"), 1024, ZEND_ACC_PROTECTED);
    zend_declare_property_long(cloudrmi_server_ce, ZEND_STRL("readTimeout"), 300, ZEND_ACC_PROTECTED);
    zend_declare_property_long(cloudrmi_server_ce, ZEND_STRL("highReadTimeout"), 300, ZEND_ACC_PROTECTED);
    zend_declare_property_long(cloudrmi_server_ce, ZEND_STRL("noForkReadTimeout"), 300, ZEND_ACC_PROTECTED);

    zend_register_class_alias(CLOUDRMI_NS "\\SimpleRMIClient", cloudrmi_client_ce);
    zend_register_class_alias(CLOUDRMI_NS "\\SimpleRMIServer", cloudrmi_server_ce);
}
