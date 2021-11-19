#ifndef _VOLD_NTFS_UTILS_H
#define _VOLD_NTFS_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
int getNtfsInfo(const char *devPath, char *label, int64_t *id);
uint64_t getNtfsSize(const char *devPath);

#ifdef __cplusplus
}
#endif
#endif
