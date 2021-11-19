/*
 * Copyright (C) 2014 Tuxera Inc.
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

#ifndef _FILESYSTEMS_H
#define _FILESYSTEMS_H

typedef enum {
    FSTYPE_UNRECOGNIZED,
    FSTYPE_FAT,
    FSTYPE_EXFAT,
    FSTYPE_NTFS,
    FSTYPE_HFSPLUS,
} FSType;

class Filesystems {
public:
    static int detect(const char *fsPath, FSType *outFsType);

    static bool isSupported(FSType fsType);

    static int check(FSType fsType, const char *fsPath);

    static int doMount(FSType fsType, const char *fsPath,
                       const char *mountPoint, bool ro, bool remount,
                       bool executable, int ownerUid, int ownerGid,
                       int permMask, bool createLost);

    static const char* fsName(FSType fsType);
};

#endif
