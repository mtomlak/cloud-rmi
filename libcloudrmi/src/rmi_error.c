#include <string.h>
#include "cloudrmi/rmi_error.h"

void rmi_error_clear(rmi_error_t *error)
{
    if (error == NULL)
        return;

    error->code = 0;
    memset(error->message, 0, sizeof(error->message));
}
