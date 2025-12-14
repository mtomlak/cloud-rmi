#include <stdlib.h>
#include <string.h>

#include "cloudrmi/rmi.h"
#include "cloudrmi/rmi_payload.h"

void rmi_payload_init(rmi_payload_t *payload, rmi_payload_type_t type)
{
    if (payload == NULL)
        return;

    payload->type = type;
    payload->data = NULL;
    payload->size = 0;
}

void rmi_payload_free(rmi_payload_t *payload)
{
    if (payload == NULL)
        return;

    if (payload->data != NULL) {
        free(payload->data);
        payload->data = NULL;
    }
    payload->size = 0;
}

int rmi_payload_from_base64(rmi_payload_t *payload, const char *encoded)
{
    char *decoded;

    if (payload == NULL || encoded == NULL)
        return 0;

    decoded = malloc(sizeof(char) * (BUF_SZ + 1));
    if (decoded == NULL)
        return 0;

    memset(decoded, 0, BUF_SZ + 1);

    if (!from64tobits(decoded, encoded)) {
        free(decoded);
        return 0;
    }

    rmi_payload_free(payload);
    payload->data = decoded;
    payload->size = strlen(decoded);

    return 1;
}

int rmi_payload_to_base64(const rmi_payload_t *payload, char *out, size_t out_size)
{
    if (payload == NULL || out == NULL || payload->data == NULL)
        return 0;

    memset(out, 0, out_size);
    to64frombits((unsigned char *)out, (const unsigned char *)payload->data, (int)payload->size);
    return 1;
}
