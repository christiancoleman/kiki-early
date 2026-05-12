#ifndef SHIM_DISCOVERY_H
#define SHIM_DISCOVERY_H

#include <stdint.h>

/* Initialize discovery patches. */
void shim_discovery_init(void);

/* Discovery SIGTRAP handler. Returns 1 if handled, 0 otherwise. */
int shim_discovery_handler(uintptr_t pc, void *ucontext);

/* Specialized logger for discovery events. */
void discovery_log(const char *fmt, ...);

#endif /* SHIM_DISCOVERY_H */
