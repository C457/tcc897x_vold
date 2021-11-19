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

#ifndef _VOLUME_H
#define _VOLUME_H

#include <utils/List.h>
#include <fs_mgr.h>

class NetlinkEvent;
class VolumeManager;

//===========================
// For telechips
// The number of partitions including extended partition
#define MAX_MOUNT_PART 16

enum {
    NOT_CHECK,
    FAT,
    NTFS,
    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    CDFS,    
    #endif
    //-NATIVE_PLATFORM
};
//===========================

class Volume {
private:
    int mState;
    int mFlags;
    //===========================
    // For telechips
    int mRemoving;
    pthread_mutex_t mLock;
    //===========================

public:
    static const int State_Init       = -1;
    static const int State_NoMedia    = 0;
    static const int State_Idle       = 1;
    static const int State_Pending    = 2;
    static const int State_Checking   = 3;
    static const int State_Mounted    = 4;
    static const int State_Unmounting = 5;
    static const int State_Formatting = 6;
    static const int State_Shared     = 7;
    static const int State_SharedMnt  = 8;

    static const char *MEDIA_DIR;
    static const char *FUSE_DIR;
    static const char *SEC_ASECDIR_EXT;
    static const char *SEC_ASECDIR_INT;
    static const char *ASECDIR;
    static const char *LOOPDIR;
    static const char *BLKID_PATH;

protected:
    char* mLabel;
    char* mUuid;
    char* mUserLabel;
    VolumeManager *mVm;
    bool mDebug;
    int mPartIdx;
    int mOrigPartIdx;
    bool mRetryMount;
    
    //===========================
    // For telechips
    bool mMultiMount;
    char mDevicePath[4096];
    //===========================

    //+NATIVE_PLATFORM Removal of VoldResponseCode.VolumeDiskPrepared
    #ifdef PATCH_STORAGE_REMOVE_PREPARED_STAGE
    char mDevType[255]; // external, usb, sd
    int32_t mVolumeId;
	#ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    char mVolumeLabel[(3 * 15) + 1];
	#else
    char mVolumeLabel[12];
	#endif
    #endif
    //-NATIVE_PLATFORM

    /*
     * The major/minor tuple of the currently mounted filesystem.
     */
    dev_t mCurrentlyMountedKdev;

public:
    Volume(VolumeManager *vm, const fstab_rec* rec, int flags);
    virtual ~Volume();

    virtual int mountVol(); // For telechips
    virtual int unmountVol(bool force, bool revert); // For telechips
    int formatVol(const char* path, const char* fstype, bool wipe); // For telechips

    const char* getLabel() { return mLabel; }
    const char* getUuid() { return mUuid; }
    const char* getUserLabel() { return mUserLabel; }
    int getState() { return mState; }
    int getFlags() { return mFlags; };

    /* Mountpoint of the raw volume */
    virtual const char *getMountpoint() = 0;
    virtual const char *getFuseMountpoint() = 0;

    virtual int handleBlockEvent(NetlinkEvent *evt);
    virtual dev_t getDiskDevice();
    virtual dev_t getShareDevice();
    virtual void handleVolumeShared();
    virtual void handleVolumeUnshared();

    void setDebug(bool enable);
    virtual int getVolInfo(struct volume_info *v) = 0;

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    virtual char* getDevicePath(void) = 0;
    virtual char* getNodePath(void) = 0;
    #endif
    //-NATIVE_PLATFORM
protected:
    void setUuid(const char* uuid);
    void setUserLabel(const char* userLabel);
    void setState(int state);
    void setRemoveState(int state); // For telechips

    virtual int getDeviceNodes(dev_t *devs, int max) = 0;
    virtual int updateDeviceInfo(char *new_path, int new_major, int new_minor) = 0;
    virtual void revertDeviceInfo(void) = 0;
    virtual int isDecrypted(void) = 0;

    int createDeviceNode(const char *path, int major, int minor);

private:
    int initializeMbr(const char *deviceNode);
    bool isMountpointMounted(const char *path);
    int mountAsecExternal();
    int doUnmount(const char *path, bool force);
    int extractMetadata(const char* devicePath);
    //===========================
    // For telechips
    #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    int mountPartition(const char *devicePath, const char *mountPoint, int uid, int gid, int mask);
    #else
    int mountPartition(char *devicePath, char*mountPoint, int uid, int gid, int mask);
    #endif

    int mountVol_l();
    int unmountVol_l(bool force, bool revert);
    //===========================
};

typedef android::List<Volume *> VolumeCollection;

#endif
