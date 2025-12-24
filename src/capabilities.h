#ifndef CLOUD_RMI_CAPABILITIES_H
#define CLOUD_RMI_CAPABILITIES_H

#include <stddef.h>
#include <stdint.h>

/* Capabilities advertise the node's role and optional protocol extensions.
 * SERVER and PROXY are mutually exclusive - a node is one or the other.
 * Core commands (CALL, PING, AUTH, etc.) are NOT capabilities.
 * Unknown capabilities from other implementations are ignored gracefully. */
#define RMI_CAP_SERVER  (1u << 1)
#define RMI_CAP_PROXY   (1u << 2)

/* Returns all capabilities that are compiled in (for help output). */
uint32_t rmi_get_compiled_capabilities(void);
/* Returns capabilities based on current runtime mode (server vs proxy). */
uint32_t rmi_get_runtime_capabilities(void);
uint32_t rmi_capability_from_token(const char *token);
uint32_t rmi_parse_capabilities(const char *token_list);
size_t rmi_capabilities_to_tokens(uint32_t caps, char *buf, size_t buf_len);

#endif /* CLOUD_RMI_CAPABILITIES_H */
