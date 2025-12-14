#ifndef CLOUDRMI_RMI_PAYLOAD_H
#define CLOUDRMI_RMI_PAYLOAD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RMI_PAYLOAD_PLAIN,
    RMI_PAYLOAD_PHP,
    RMI_PAYLOAD_JSON,
    RMI_PAYLOAD_BLOB
} rmi_payload_type_t;

typedef struct {
    rmi_payload_type_t type;
    void *data;
    size_t size;
} rmi_payload_t;

void rmi_payload_init(rmi_payload_t *payload, rmi_payload_type_t type);
void rmi_payload_free(rmi_payload_t *payload);
int rmi_payload_from_base64(rmi_payload_t *payload, const char *encoded);
int rmi_payload_to_base64(const rmi_payload_t *payload, char *out, size_t out_size);
void to64frombits(unsigned char *out, const unsigned char *in, int inlen);
int from64tobits(char *out, const char *in);

#ifdef __cplusplus
}
#endif

#endif
