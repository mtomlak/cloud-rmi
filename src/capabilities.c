#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "capabilities.h"
#include "cloud-rmi.h"

struct capability_def {
   uint32_t bit;
   const char *token;
};

static const struct capability_def capability_definitions[] = {
   { RMI_CAP_PROXY,   "PROXY"   },
   { RMI_CAP_STATS,   "STATS"   },
};

static const uint32_t RMI_COMPILED_CAPS =
#ifdef PROXY_SUPPORT
   RMI_CAP_PROXY |
#endif
   RMI_CAP_STATS;

uint32_t rmi_get_compiled_capabilities(void)
{
   return RMI_COMPILED_CAPS;
}

uint32_t rmi_capability_from_token(const char *token)
{
   if (token == NULL)
      return 0;

   for (size_t i = 0; i < (sizeof(capability_definitions) / sizeof(capability_definitions[0])); i++) {
      const char *expected = capability_definitions[i].token;

      if (strlen(token) == strlen(expected)) {
         size_t idx = 0;

         while (token[idx] && expected[idx]) {
            if (toupper((unsigned char)token[idx]) != expected[idx])
               break;
            idx++;
         }

         if (idx == strlen(expected))
            return capability_definitions[i].bit;
      }
   }

   return 0;
}

uint32_t rmi_parse_capabilities(const char *token_list)
{
   if (token_list == NULL)
      return 0;

   uint32_t caps = 0;
   const char *cursor = token_list;

   while (*cursor) {
      while (*cursor == ' ')
         cursor++;

      if (*cursor == '\0')
         break;

      const char *end = cursor;
      while (*end != '\0' && *end != ' ')
         end++;

      char token[RMI_BUF_SZ + 1];
      size_t len = (size_t)(end - cursor);
      if (len > RMI_BUF_SZ)
         len = RMI_BUF_SZ;

      memcpy(token, cursor, len);
      token[len] = '\0';

      caps |= rmi_capability_from_token(token);

      cursor = end;
   }

   return caps;
}

size_t rmi_capabilities_to_tokens(uint32_t caps, char *buf, size_t buf_len)
{
   if (buf == NULL || buf_len == 0)
      return 0;

   size_t used = 0;
   buf[0] = '\0';

   for (size_t i = 0; i < (sizeof(capability_definitions) / sizeof(capability_definitions[0])); i++) {
      if (!(caps & capability_definitions[i].bit))
         continue;

      const char *token = capability_definitions[i].token;
      size_t token_len = strlen(token);
      size_t required = token_len + (used ? 1 : 0);

      if (used + required >= buf_len)
         break;

      if (used)
         buf[used++] = ' ';

      memcpy(buf + used, token, token_len);
      used += token_len;
      buf[used] = '\0';
   }

   return used;
}
