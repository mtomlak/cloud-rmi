#ifndef CLOUDRMI_PAYLOAD_H
#define CLOUDRMI_PAYLOAD_H

#include "types.h"

void to64frombits(unsigned char *out, const unsigned char *in, int inlen);
int from64tobits(char *out, const char *in);

#endif /* CLOUDRMI_PAYLOAD_H */
