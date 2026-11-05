/* iOS SDK lacks libproc.h; the functions exist in libSystem. */
#pragma once
#include <stdint.h>
#include <sys/types.h>
#define PROC_PIDPATHINFO_MAXSIZE (4 * 1024)
#ifdef __cplusplus
extern "C" {
#endif
int proc_pidpath(int pid, void *buffer, uint32_t buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);
#ifdef __cplusplus
}
#endif
