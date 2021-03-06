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

#ifndef _NTFS_H
#define _NTFS_H

#include <unistd.h>

class Ntfs {
public:
    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    static int detect(const char *fsPath, bool *outResult);
    #endif
    //-NATIVE_PLATFORM
    static int check(const char *fsPath);
    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_TUXERA_PATCH        
    static int doMount(const char *fsPath, const char *mountPoint, bool ro,
                       bool remount, bool executable, int ownerUid,
                       int ownerGid, int permMask);
    #else
    static int doMount(const char *fsPath, const char *mountPoint,
                       bool ro, bool remount, bool executable,
                       int ownerUid, int ownerGid, int permMask,
                       bool createLost);
    #endif
    //-NATIVE_PLATFORM
    static int format(const char *fsPath, unsigned int numSectors);
};

#endif
