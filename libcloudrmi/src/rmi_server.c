#include <string.h>
#include "cloudrmi/rmi.h"
#include "cloudrmi/rmi_server.h"

void rmi_server_init(rmi_server_t *server, const char *context_name)
{
    if (server == NULL)
        return;

    memset(server, 0, sizeof(*server));
    server->context_name = context_name;
    server->protocol_extensions = 0;
}

void rmi_server_set_call_handler(rmi_server_t *server, rmi_call_handler handler, void *userdata)
{
    if (server == NULL)
        return;

    server->call_handler = handler;
    server->call_userdata = userdata;
}
