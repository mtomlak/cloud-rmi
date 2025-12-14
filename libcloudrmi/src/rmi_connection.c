#include <string.h>
#include "cloudrmi/rmi_connection.h"

void rmi_connection_init(rmi_connection_t *conn, int sock)
{
    if (conn == NULL)
        return;

    conn->sock = sock;
    conn->is_authenticated = 0;
    memset(conn->username, 0, sizeof(conn->username));
}
