#ifndef CLOUDRMI_ERROR_H
#define CLOUDRMI_ERROR_H

typedef enum {
    RMI_ERROR_NONE = 0,
    RMI_ERROR_BAD_REQUEST,
    RMI_ERROR_BAD_ARGS
} rmi_error_code_t;

typedef struct {
    rmi_error_code_t code;
    const char *message;
} rmi_error_t;

#endif /* CLOUDRMI_ERROR_H */
