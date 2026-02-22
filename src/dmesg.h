#ifndef DMESG_H
#define DMESG_H

#include <stddef.h>

void dmesg_init(void);
void dmesg_log(const char *msg);
void dmesg_print(void);

#endif
