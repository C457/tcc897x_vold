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

#define LOG_TAG "Vold"

#include <cutils/log.h>
#include <cutils/properties.h>

#include "HfsPlus.h"

extern "C" int logwrap(int argc, const char **argv, int background);
extern "C" int mount(const char *, const char *, const char *, unsigned long, const void *);

int HfsPlus::detect(const char *fsPath, bool *outResult) {
    int retval = -1;

    int fd = open(fsPath, O_RDONLY);
    if(fd != -1) {
        loff_t seek_res = lseek64(fd, 1024, SEEK_SET);
        if(seek_res == 1024) {
            char signature[4];
            ssize_t read_res = read(fd, signature, 4);
            if(read_res == 4) {
                if(!memcmp(signature, "H+\x00\x04", 4)) {
                    SLOGI("HFS+ filesystem detected.");
                    *outResult = true;
                }
                else if(!memcmp(signature, "HX\x00\x05", 4)) {
                    SLOGI("HFSX filesystem detected.");
                    *outResult = true;
                }
                else {
                    SLOGV("Filesystem detection failed (not an HFS+/HFSX "
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
