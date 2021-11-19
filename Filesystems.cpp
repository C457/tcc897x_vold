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

#include "Filesystems.h"

#include "HfsPlus.h"
#include "Ntfs.h"
#include "ExFat.h"
#include "Fat.h"

#include <errno.h>

int Filesystems::detect(const char *fsPath, FSType *outFsType)
{
    int err;
    bool result = false;

    if ((err = HfsPlus::detect(fsPath, &result)) == 0 && result) {
        *outFsType = FSTYPE_HFSPLUS;
    }
    else if ((err = Ntfs::detect(fsPath, &result)) == 0 && result) {
        *outFsType = FSTYPE_NTFS;
    }
    else if ((err = ExFat::detect(fsPath, &result)) == 0 && result) {
        *outFsType = FSTYPE_EXFAT;
    }
    else {
        /* Until we implement reliable FAT detection code. */
        *outFsType = FSTYPE_FAT;
    }

    return 0; /* Always succeed since FAT is the default fallback. */
}

bool Filesystems::isSupported(FSType fsType)
{
    switch (fsType) {
    case FSTYPE_HFSPLUS:
        return false;
    case FSTYPE_NTFS:
        return true;
    case FSTYPE_EXFAT:
        return true;
    case FSTYPE_FAT:
        return true;
    default:
        return false;
    }
}

int Filesystems::check(FSType fsType, const char *fsPath)
{
    switch (fsType) {
    case FSTYPE_HFSPLUS:
        errno = ENODATA;
        return -1;
    case FSTYPE_NTFS:
        return Ntfs::check(fsPath);
    case FSTYPE_EXFAT:
        return ExFat::check(fsPath);
    case FSTYPE_FAT:
        return Fat::check(fsPath);
    default:
        errno = ENODATA;
        return -1;
    }
}

int Filesystems::doMount(FSType fsType, const char *fsPath,
                         const char *mountPoint, bool ro, bool remount,
                         bool executable, int ownerUid, int ownerGid,
                         int permMask, bool createLost)
{
    switch (fsType) {
    case FSTYPE_HFSPLUS:
        errno = ENOTSUP;
        return -1;
    case FSTYPE_NTFS:
        return Ntfs::doMount(fsPath, mountPoint, ro, remount, executable,
                             ownerUid, ownerGid, permMask);
    case FSTYPE_EXFAT:
        return ExFat::doMount(fsPath, mountPoint, ro, remount, executable,
                              ownerUid, ownerGid, permMask);
    case FSTYPE_FAT:
        return Fat::doMount(fsPath, mountPoint, ro, remount, executable,
                            ownerUid, ownerGid, permMask, createLost);
    default:
        errno = ENOTSUP;
        return -1;
    }
}

const char* Filesystems::fsName(FSType fsType)
{
    switch (fsType) {
    case FSTYPE_FAT:
        return "VFAT";
    case FSTYPE_EXFAT:
        return "EXFAT";
    case FSTYPE_NTFS:
        return "NTFS";
    case FSTYPE_HFSPLUS:
        return "HFS+";
    default:
        return "<unknown filesystem>";
    }
}
