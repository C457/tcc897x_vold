/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2010-2014 Tuxera Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include <linux/kdev_t.h>

#define LOG_TAG "Vold"

#include <cutils/log.h>
#include <cutils/properties.h>

#include <logwrap/logwrap.h>

#include "Ntfs.h"

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_TUXERA_PATCH    
static char MKNTFS_PATH[] = "/sbin/mkntfs";
#else
static char FSCK_NTFS_PATH[] = "/system/bin/chkntfs";
static char MKNTFS_PATH[] = "/system/bin/mkntfs";
#endif
//-NATIVE_PLATFORM

extern "C" int mount(const char *, const char *, const char *, unsigned long, const void *);

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_TUXERA_PATCH    
int Ntfs::detect(const char *fsPath, bool *outResult) {
    int retval = -1;

    int fd = open(fsPath, O_RDONLY);
    if(fd != -1) {
        loff_t seek_res = lseek64(fd, 3, SEEK_SET);
        if(seek_res == 3) {
            char signature[8];
            ssize_t read_res = read(fd, signature, 8);
            if(read_res == 8) {
                if(!memcmp(signature, "NTFS    ", 8)) {
                    SLOGI("Detected NTFS filesystem.");
                    *outResult = true;
                }
                else {
                    SLOGV("Filesystem detection failed (not an NTFS "
                          "filesystem).");
                    *outResult = false;
                }

                retval = 0;
            }
            else if(read_res != -1)
                errno = EIO;
        }
        else if(seek_res != -1)
            errno = EIO;

        close(fd);
    }

    return retval;
}
#endif
//-NATIVE_PLATFORM

int Ntfs::check(const char *fsPath) {
    /* Insert NTFS checking code here. */
    
    //+NATIVE_PLATFORM
    #if ! defined (FUNCTION_STORAGE_TUXERA_PATCH)
    bool rw = true;
    if (access(FSCK_NTFS_PATH, X_OK)) {
        SLOGW("NTFS: Skipping fs checks\n");
        return 0;
    }

    int pass = 1;
    int rc = 0;
    do {
        const char *args[5];
        int status;

        args[0] = FSCK_NTFS_PATH;
        args[1] = fsPath;
        args[2] = "-a";
        args[3] = "-f";
        args[4] = NULL;

        rc = android_fork_execvp(5, (char **)args, &status, false, true);

        if (rc != 0) {
            SLOGE("Filesystem check failed due to logwrap error");
            errno = EIO;
            return -1;
        }

        if (!WIFEXITED(status)) {
            SLOGE("Filesystem check did not exit properly");
            errno = EIO;
            return -1;
        }

        status = WEXITSTATUS(status);

        switch(status) {
        case 0:
            SLOGI("NTFS: Filesystem check completed OK");
            return 0;

        case 1:
            SLOGI("NTFS: no errors were found on the %s volume" , fsPath);
            return 0;

        case 2:
            SLOGW("NTFS: error were found and fixed");
            return 0;

        case 3:
            SLOGW("NTFS: only minor errors were found on the %d Volume", fsPath);
            return 0;

        case 4:
            SLOGW("NTFS: errors were found onthe %s but they could not be fixed" , fsPath);
            return 0;

        case 6:
            SLOGW("NTFS: %s volume is not NTFS volume" , fsPath);
            errno = ENODATA;
            return 0;

        default:
            SLOGE("NTFS: Filesystem check failed (unknown exit code %d)", rc);
            errno = EIO;
            return -1;
        }
    } while (0);
    #endif
    //-NATIVE_PLATFORM
    
    return 0;
}

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_TUXERA_PATCH    
int Ntfs::doMount(const char *fsPath, const char *mountPoint,
                 bool ro, bool remount, bool executable, int ownerUid,
                 int ownerGid, int permMask) {
#else
int Ntfs::doMount(const char *fsPath, const char *mountPoint,
                 bool ro, bool remount, bool executable,
                 int ownerUid, int ownerGid, int permMask, bool createLost) {
#endif                 
//-NATIVE_PLATFORM
    int rc;
    unsigned long flags;
    char mountData[255];

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    flags = MS_NODEV | MS_NOSUID | MS_DIRSYNC;
    #else
    flags = MS_NODEV | MS_NOSUID | MS_DIRSYNC | MS_NOATIME | MS_NODIRATIME;
    #endif                 
    //-NATIVE_PLATFORM

    flags |= (executable ? 0 : MS_NOEXEC);
    flags |= (ro ? MS_RDONLY : 0);
    flags |= (remount ? MS_REMOUNT : 0);

    /*
     * Note: This is a temporary hack. If the sampling profiler is enabled,
     * we make the SD card world-writable so any process can write snapshots.
     *
     * TODO: Remove this code once we have a drop box in system_server.
     */
    char value[PROPERTY_VALUE_MAX];
    property_get("persist.sampling_profiler", value, "");
    if (value[0] == '1') {
        SLOGW("The SD card is world-writable because the"
            " 'persist.sampling_profiler' system property is set to '1'.");
        permMask = 0;
    }

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    sprintf(mountData,
            "utf8,uid=%d,gid=%d,fmask=%o,dmask=%o",
            ownerUid, ownerGid, permMask, permMask);

    rc = mount(fsPath, mountPoint, "tntfs", flags, mountData);

    if (rc && errno == EROFS) {
        SLOGE("%s appears to be a read only filesystem - retrying mount RO", fsPath);
        flags |= MS_RDONLY;
        rc = mount(fsPath, mountPoint, "tntfs", flags, mountData);
    }
    #else
    sprintf(mountData,
            "iocharset=utf8,uid=%d,gid=%d,fmask=%o,dmask=%o,force",
            ownerUid, ownerGid, permMask, permMask);

    rc = mount(fsPath, mountPoint, "ufsd", flags, mountData);

    if (rc && errno == EROFS) {
        SLOGE("%s appears to be a read only filesystem - retrying mount RO", fsPath);
        flags |= MS_RDONLY;
        rc = mount(fsPath, mountPoint, "ufsd", flags, mountData);
    }

    if (rc == 0 && createLost) {
        char *lost_path;
        asprintf(&lost_path, "%s/LOST.DIR", mountPoint);
        if (access(lost_path, F_OK)) {
            /*
             * Create a LOST.DIR in the root so we have somewhere to put
             * lost cluster chains (fsck_msdos doesn't currently do this)
             */
            if (mkdir(lost_path, 0755)) {
                SLOGE("Unable to create LOST.DIR (%s)", strerror(errno));
            }
        }
        free(lost_path);
    }
    #endif
    return rc;
}

int Ntfs::format(const char *fsPath, unsigned int numSectors) {
    //+NATIVE_PLATFORM
    #if ! defined (FUNCTION_STORAGE_TUXERA_PATCH)
    int fd;
    #endif
    //-NATIVE_PLATFORM
    const char *args[5];
    int rc;
    int status;

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    args[0] = MKNTFS_PATH;
    args[1] = "-Q";
    args[2] = fsPath;
    if (numSectors) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%u", numSectors);
        args[3] = (const char*) tmp;
        rc = android_fork_execvp(4, (char **)args, &status, false, true);
    }
    else {
        rc = android_fork_execvp(3, (char **)args, &status, false, true);
    }
    #else
    args[0] = MKNTFS_PATH;
    args[1] = "-a:32k";
    args[2] = "-f";
    args[3] = fsPath;
    
    rc = android_fork_execvp(4, (char **)args, &status, false, true);
    #endif
    //-NATIVE_PLATFORM

    if (rc != 0) {
        SLOGE("Filesystem format failed due to logwrap error");
        errno = EIO;
        return -1;
    }

    if (!WIFEXITED(status)) {
        SLOGE("Filesystem format did not exit properly");
        errno = EIO;
        return -1;
    }

    status = WEXITSTATUS(status);

    if (status == 0) {
        SLOGI("Filesystem formatted OK");
        return 0;
    } else {
        SLOGE("Format failed (unknown exit code %d)", status);
        errno = EIO;
        return -1;
    }
    return 0;
}
