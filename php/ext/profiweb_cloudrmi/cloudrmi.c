#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "php_profiweb_cloudrmi.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <cloudrmi/protocol.h>

static int le_cloudrmi_ctx;

typedef struct _cloudrmi_context_wrapper {
    rmi_protocol_ctx_t *ctx;
} cloudrmi_context_wrapper;

static void cloudrmi_ctx_dtor(zend_resource *res)
{
    cloudrmi_context_wrapper *wrapper = (cloudrmi_context_wrapper *)res->ptr;
    if (wrapper != NULL) {
        if (wrapper->ctx != NULL) {
            rmi_protocol_destroy(wrapper->ctx);
            wrapper->ctx = NULL;
        }
        efree(wrapper);
    }
}

static cloudrmi_context_wrapper *cloudrmi_fetch_ctx(zval *zctx)
{
    return (cloudrmi_context_wrapper *)zend_fetch_resource_ex(
        zctx,
        "cloudrmi_protocol_ctx",
        le_cloudrmi_ctx);
}

static void cloudrmi_set_error_assoc(zval *array, const rmi_error_t *err)
{
    if (err == NULL || err->message == NULL) {
        add_assoc_null(array, "err_message");
    } else {
        add_assoc_string(array, "err_message", err->message);
    }

    add_assoc_long(array, "err_code", err == NULL ? (zend_long)RMI_ERROR_NONE : (zend_long)err->code);
}

static void cloudrmi_populate_event_array(zval *target, const rmi_event_t *event)
{
    array_init(target);
    add_assoc_long(target, "type", event->type);
    add_assoc_string(target, "cmd", event->cmd);
    add_assoc_long(target, "argc", event->argc);
    add_assoc_string(target, "arg1", event->arg1);
    add_assoc_string(target, "arg2", event->arg2);
    add_assoc_string(target, "arg3", event->arg3);
    add_assoc_string(target, "arg4", event->arg4);
    add_assoc_string(target, "arg5", event->arg5);
    add_assoc_string(target, "encoding", event->encoding);

    if (event->payload == NULL) {
        add_assoc_stringl(target, "payload", "", 0);
        add_assoc_long(target, "payload_len", 0);
        return;
    }

    zend_string *payload = zend_string_init((const char *)event->payload, event->payload_len, 0);
    add_assoc_str(target, "payload", payload);
    add_assoc_long(target, "payload_len", event->payload_len);
}

PHP_FUNCTION(cloudrmi_protocol_create)
{
    if (zend_parse_parameters_none() == FAILURE) {
        RETURN_THROWS();
    }

    cloudrmi_context_wrapper *wrapper = ecalloc(1, sizeof(cloudrmi_context_wrapper));
    wrapper->ctx = rmi_protocol_create();
    if (wrapper->ctx == NULL) {
        efree(wrapper);
        RETURN_FALSE;
    }

    RETURN_RES(zend_register_resource(wrapper, le_cloudrmi_ctx));
}

PHP_FUNCTION(cloudrmi_protocol_destroy)
{
    zval *zctx;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(zctx)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_context_wrapper *wrapper = cloudrmi_fetch_ctx(zctx);
    if (wrapper == NULL) {
        RETURN_FALSE;
    }

    if (wrapper->ctx != NULL) {
        rmi_protocol_destroy(wrapper->ctx);
        wrapper->ctx = NULL;
    }
}

PHP_FUNCTION(cloudrmi_protocol_reset)
{
    zval *zctx;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(zctx)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_context_wrapper *wrapper = cloudrmi_fetch_ctx(zctx);
    if (wrapper == NULL || wrapper->ctx == NULL) {
        RETURN_FALSE;
    }

    rmi_protocol_reset(wrapper->ctx);
}

PHP_FUNCTION(cloudrmi_protocol_feed)
{
    zval *zctx;
    zend_string *data;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_RESOURCE(zctx)
        Z_PARAM_STR(data)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_context_wrapper *wrapper = cloudrmi_fetch_ctx(zctx);
    if (wrapper == NULL || wrapper->ctx == NULL) {
        RETURN_FALSE;
    }

    rmi_error_t err;
    rmi_status_t status = rmi_protocol_feed(wrapper->ctx, (const uint8_t *)ZSTR_VAL(data), ZSTR_LEN(data), &err);

    array_init(return_value);
    add_assoc_long(return_value, "status", status);
    cloudrmi_set_error_assoc(return_value, &err);
}

PHP_FUNCTION(cloudrmi_protocol_next_event)
{
    zval *zctx;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(zctx)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_context_wrapper *wrapper = cloudrmi_fetch_ctx(zctx);
    if (wrapper == NULL || wrapper->ctx == NULL) {
        RETURN_FALSE;
    }

    rmi_error_t err;
    rmi_event_t event;
    rmi_status_t status = rmi_protocol_next_event(wrapper->ctx, &event, &err);

    array_init(return_value);
    add_assoc_long(return_value, "status", status);
    cloudrmi_set_error_assoc(return_value, &err);

    if (status != RMI_OK) {
        add_assoc_null(return_value, "event");
        return;
    }

    zval event_arr;
    cloudrmi_populate_event_array(&event_arr, &event);
    add_assoc_zval(return_value, "event", &event_arr);
}

PHP_FUNCTION(cloudrmi_protocol_set_version)
{
    zval *zctx;
    zend_long version;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_RESOURCE(zctx)
        Z_PARAM_LONG(version)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_context_wrapper *wrapper = cloudrmi_fetch_ctx(zctx);
    if (wrapper == NULL || wrapper->ctx == NULL) {
        RETURN_FALSE;
    }

    int ok = rmi_protocol_set_version(wrapper->ctx, (int)version);
    RETURN_BOOL(ok != 0);
}

PHP_FUNCTION(cloudrmi_protocol_set_max_payload)
{
    zval *zctx;
    zend_long max_payload;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_RESOURCE(zctx)
        Z_PARAM_LONG(max_payload)
    ZEND_PARSE_PARAMETERS_END();

    cloudrmi_context_wrapper *wrapper = cloudrmi_fetch_ctx(zctx);
    if (wrapper == NULL || wrapper->ctx == NULL) {
        RETURN_FALSE;
    }

    int ok = rmi_protocol_set_max_payload(wrapper->ctx, (size_t)max_payload);
    RETURN_BOOL(ok != 0);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cloudrmi_protocol_create, 0, 0, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cloudrmi_protocol_destroy, 0, 1, IS_VOID, 0)
    ZEND_ARG_INFO(0, ctx)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cloudrmi_protocol_reset, 0, 1, IS_VOID, 0)
    ZEND_ARG_INFO(0, ctx)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cloudrmi_protocol_feed, 0, 2, IS_ARRAY, 0)
    ZEND_ARG_INFO(0, ctx)
    ZEND_ARG_TYPE_INFO(0, bytes, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cloudrmi_protocol_next_event, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_INFO(0, ctx)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cloudrmi_protocol_set_version, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_INFO(0, ctx)
    ZEND_ARG_TYPE_INFO(0, version, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cloudrmi_protocol_set_max_payload, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_INFO(0, ctx)
    ZEND_ARG_TYPE_INFO(0, max_payload, IS_LONG, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry cloudrmi_functions[] = {
    PHP_FE(cloudrmi_protocol_create, arginfo_cloudrmi_protocol_create)
    PHP_FE(cloudrmi_protocol_destroy, arginfo_cloudrmi_protocol_destroy)
    PHP_FE(cloudrmi_protocol_reset, arginfo_cloudrmi_protocol_reset)
    PHP_FE(cloudrmi_protocol_feed, arginfo_cloudrmi_protocol_feed)
    PHP_FE(cloudrmi_protocol_next_event, arginfo_cloudrmi_protocol_next_event)
    PHP_FE(cloudrmi_protocol_set_version, arginfo_cloudrmi_protocol_set_version)
    PHP_FE(cloudrmi_protocol_set_max_payload, arginfo_cloudrmi_protocol_set_max_payload)
    PHP_FE_END
};

typedef struct {
    const char *name;
    zend_long value;
} cloudrmi_constant_entry;

static bool cloudrmi_constant_defined(const char *name)
{
    if (name == NULL) {
        return false;
    }

    return zend_get_constant_str(name, strlen(name)) != NULL;
}

static const cloudrmi_constant_entry cloudrmi_constant_entries[] = {
    {"RMI_OK", (zend_long)RMI_OK},
    {"RMI_E_AGAIN", (zend_long)RMI_E_AGAIN},
    {"RMI_E_OVERFLOW", (zend_long)RMI_E_OVERFLOW},
    {"RMI_E_ERR", (zend_long)RMI_E_ERR},
    {"RMI_ERROR_NONE", (zend_long)RMI_ERROR_NONE},
    {"RMI_ERROR_BAD_REQUEST", (zend_long)RMI_ERROR_BAD_REQUEST},
    {"RMI_ERROR_BAD_ARGS", (zend_long)RMI_ERROR_BAD_ARGS},
    {"RMI_MAX_LINE", (zend_long)RMI_MAX_LINE},
    {"RMI_MAX_PAYLOAD", (zend_long)RMI_MAX_PAYLOAD},
    {"RMI_MAX_BUFFER", (zend_long)RMI_MAX_BUFFER},
    {"RMI_INLINE_PAYLOAD_THRESHOLD", (zend_long)RMI_INLINE_PAYLOAD_THRESHOLD},
    {"RMI_EVENT_NONE", (zend_long)RMI_EVENT_NONE},
    {"RMI_EVENT_AUTH", (zend_long)RMI_EVENT_AUTH},
    {"RMI_EVENT_CALL", (zend_long)RMI_EVENT_CALL},
    {"RMI_EVENT_HELP", (zend_long)RMI_EVENT_HELP},
    {"RMI_EVENT_IFACES", (zend_long)RMI_EVENT_IFACES},
    {"RMI_EVENT_RELOAD", (zend_long)RMI_EVENT_RELOAD},
    {"RMI_EVENT_SHUTDOWN", (zend_long)RMI_EVENT_SHUTDOWN},
    {"RMI_EVENT_TIME", (zend_long)RMI_EVENT_TIME},
    {"RMI_EVENT_UPTIME", (zend_long)RMI_EVENT_UPTIME},
    {"RMI_EVENT_NOOP", (zend_long)RMI_EVENT_NOOP},
    {"RMI_EVENT_DEBUG", (zend_long)RMI_EVENT_DEBUG},
    {"RMI_EVENT_QUIT", (zend_long)RMI_EVENT_QUIT},
    {"RMI_EVENT_PROXY", (zend_long)RMI_EVENT_PROXY},
    {"RMI_EVENT_CAPS", (zend_long)RMI_EVENT_CAPS},
    {"RMI_EVENT_SERVER", (zend_long)RMI_EVENT_SERVER},
    {"RMI_EVENT_PROTO", (zend_long)RMI_EVENT_PROTO},
    {"RMI_EVENT_UNKNOWN", (zend_long)RMI_EVENT_UNKNOWN},
    {"RMI_MAX_PROTOCOL_VERSION", (zend_long)RMI_MAX_PROTOCOL_VERSION},
};

static const size_t cloudrmi_constant_entries_count = sizeof(cloudrmi_constant_entries) / sizeof(cloudrmi_constant_entries[0]);

static void cloudrmi_register_constants(INIT_FUNC_ARGS)
{
    for (size_t i = 0; i < cloudrmi_constant_entries_count; i++) {
        const cloudrmi_constant_entry *entry = &cloudrmi_constant_entries[i];
        if (cloudrmi_constant_defined(entry->name)) {
            continue;
        }
        zend_register_long_constant(
            entry->name,
            strlen(entry->name),
            entry->value,
            CONST_CS | CONST_PERSISTENT,
            module_number
        );
    }
    if (!cloudrmi_constant_defined("RMI_LIBRARY_VERSION")) {
        zend_register_stringl_constant(
            "RMI_LIBRARY_VERSION",
            strlen("RMI_LIBRARY_VERSION"),
            CLOUDRMI_LIBRARY_VERSION,
            strlen(CLOUDRMI_LIBRARY_VERSION),
            CONST_CS | CONST_PERSISTENT,
            module_number
        );
    }
}

PHP_MINIT_FUNCTION(profiweb_cloudrmi)
{
    le_cloudrmi_ctx = zend_register_list_destructors_ex(cloudrmi_ctx_dtor, NULL, "profiweb_cloudrmi_protocol_ctx", module_number);
    cloudrmi_register_constants(INIT_FUNC_ARGS_PASSTHRU);
    return SUCCESS;
}

PHP_MINFO_FUNCTION(profiweb_cloudrmi)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "profiweb_cloudrmi support", "enabled");
    php_info_print_table_row(2, "profiweb_cloudrmi version", PHP_PROFIWEB_CLOUDRMI_VERSION);
    php_info_print_table_row(2, "libcloudrmi protocol", CLOUDRMI_PROTOCOL_VERSION_STRING);
    php_info_print_table_row(2, "libcloudrmi version", CLOUDRMI_LIBRARY_VERSION);
    php_info_print_table_end();

    php_info_print_table_start();
    php_info_print_table_header(2, "Constant", "Value");
    for (size_t i = 0; i < cloudrmi_constant_entries_count; i++) {
        const cloudrmi_constant_entry *entry = &cloudrmi_constant_entries[i];
        char value_buf[32];
        snprintf(value_buf, sizeof(value_buf), ZEND_LONG_FMT, entry->value);
        php_info_print_table_row(2, entry->name, value_buf);
    }
    php_info_print_table_row(
        2,
        "RMI_LIBRARY_VERSION",
        CLOUDRMI_LIBRARY_VERSION
    );
    php_info_print_table_end();
}

zend_module_entry profiweb_cloudrmi_module_entry = {
    STANDARD_MODULE_HEADER,
    "profiweb_cloudrmi",
    cloudrmi_functions,
    PHP_MINIT(profiweb_cloudrmi),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(profiweb_cloudrmi),
    PHP_PROFIWEB_CLOUDRMI_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_PROFIWEB_CLOUDRMI
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(profiweb_cloudrmi)
#endif
