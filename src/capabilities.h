#ifndef CLOUD_RMI_CAPABILITIES_H
#define CLOUD_RMI_CAPABILITIES_H

#include <stddef.h>
#include <stdint.h>

#define RMI_CAP_PROXY   (1u << 1)
#define RMI_CAP_SERVER  (1u << 2)
#define RMI_CAP_PING    (1u << 3)
#define RMI_CAP_CALL    (1u << 4)

uint32_t rmi_get_compiled_capabilities(void);
uint32_t rmi_capability_from_token(const char *token);
uint32_t rmi_parse_capabilities(const char *token_list);
size_t rmi_capabilities_to_tokens(uint32_t caps, char *buf, size_t buf_len);

#endif /* CLOUD_RMI_CAPABILITIES_H */
