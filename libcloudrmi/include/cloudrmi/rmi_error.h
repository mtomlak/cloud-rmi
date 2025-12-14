#ifndef CLOUDRMI_RMI_ERROR_H
#define CLOUDRMI_RMI_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rmi_error {
    int code;
    char message[512];
} rmi_error_t;

void rmi_error_clear(rmi_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
