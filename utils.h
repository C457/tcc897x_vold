#ifndef _VOLD_UTILS_H
#define _VOLD_UTILS_H

#include "ntfsutils.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stdbool.h>

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_TUXERA_PATCH    
int getVolumeInfo(const char *mntPnt, const char *devPath, char *label,
                  size_t labelSize, int32_t *id);
#else
int getVolumeInfo(const char *mntPnt, const char *devPath, char *label, int32_t *id);
#endif                  
//-NATIVE_PLATFORM
uint64_t getVolumeSize(const char *mntPnt, const char *devPath);

void getDevType(const char *mntPnt, char *devType);
//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_SUPPORT_CDROM
int isCdromPoint(const char *mntPnt);
int isCdromAvailable();
#endif
//-NATIVE_PLATFORM
int isUsbMountPoint(const char *mntPnt);
int isReadOnlyMedia(const char *mntPnt, const int isWritableUsb);
bool isReadable(const char *path);
void markMediaInserted(const char *mntPnt);
void unmarkMediaInserted(const char *mntPnt);

#ifdef __cplusplus
}
#endif

#endif
