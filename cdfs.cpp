/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <linux/kdev_t.h>
#include <linux/fs.h>

#define LOG_TAG "Vold"

#include <cutils/log.h>
#include <cutils/properties.h>

#include "cdfs.h"

extern "C" int logwrap(int argc, const char **argv, int background);
extern "C" int mount(const char *, const char *, const char *, unsigned long, const void *);


int CDFS::doMount(const char *fsPath, const char *mountPoint,
                 bool ro, bool remount, bool executable,
                 int ownerUid, int ownerGid, int permMask, bool createLost) {
    int rc;
    unsigned long flags;
    char mountData[255];

    flags = MS_NODEV | MS_NOSUID | MS_DIRSYNC;

    flags |= MS_NOEXEC;
    flags |= MS_RDONLY;
    flags |= (remount ? MS_REMOUNT : 0);

    createLost = 0; // CDROM is not Writable 

    sprintf(mountData,
            "utf8,uid=%d,gid=%d,overriderockperm,mode=0044,dmode=0055",
            ownerUid, ownerGid);

    rc = mount(fsPath, mountPoint, "iso9660", flags, mountData);

    if(rc != 0){
	    SLOGE("%s appears to be a unother file system [%s] - retrying to mount UDF file system" , fsPath, strerror(errno));
    	    rc = mount(fsPath, mountPoint, "UDF", flags, mountData);
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

    return rc;
}



