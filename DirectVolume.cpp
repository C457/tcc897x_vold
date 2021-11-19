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
#include <string.h>
#include <errno.h>

#include <linux/kdev_t.h>

#define LOG_TAG "DirectVolume"

#include <cutils/log.h>
#include <cutils/properties.h> // For telechips
#include <sysutils/NetlinkEvent.h>

#include "DirectVolume.h"
#include "VolumeManager.h"
#include "ResponseCode.h"
#include "cryptfs.h"

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
#include <sys/types.h>
#include <sys/stat.h>
#include "utils.h"

#define VOLD_NODE_FORMAT "/dev/block/vold/%d:%d"    
#endif
//-NATIVE_PLATFORM

#define PARTITION_DEBUG // For telechips


DirectVolume::DirectVolume(VolumeManager *vm, const fstab_rec* rec, int flags) :
        Volume(vm, rec, flags) {
    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    mOrigPartIdx = mPartIdx;
    #endif
    //-NATIVE_PLATFORM

    mPaths = new PathCollection();
    for (int i = 0; i < MAX_PARTITIONS; i++)
        mPartMinors[i] = -1;
    mPendingPartMap = 0;
    mDiskMajor = -1;
    mDiskMinor = -1;
    mDiskNumParts = 0;

    if (strcmp(rec->mount_point, "auto") != 0) {
        ALOGE("Vold managed volumes must have auto mount point; ignoring %s",
              rec->mount_point);
    }

    char mount[PATH_MAX];

    if (flags & VOL_NOFUSE) { // For telechips
        snprintf(mount, PATH_MAX, "%s", rec->mount_point);
    } else {
        snprintf(mount, PATH_MAX, "%s/%s", Volume::MEDIA_DIR, rec->label);
    }
    mMountpoint = strdup(mount);
    snprintf(mount, PATH_MAX, "%s/%s", Volume::FUSE_DIR, rec->label);
    mFuseMountpoint = strdup(mount);

    setState(Volume::State_NoMedia);
}

DirectVolume::~DirectVolume() {
    PathCollection::iterator it;

    for (it = mPaths->begin(); it != mPaths->end(); ++it)
        free(*it);
    delete mPaths;
}

int DirectVolume::addPath(const char *path) {
    mPaths->push_back(strdup(path));
    return 0;
}

dev_t DirectVolume::getDiskDevice() {
    return MKDEV(mDiskMajor, mDiskMinor);
}

dev_t DirectVolume::getShareDevice() {
    if (mPartIdx != -1) {
        return MKDEV(mDiskMajor, mPartIdx);
    } else {
        return MKDEV(mDiskMajor, mDiskMinor);
    }
}

void DirectVolume::handleVolumeShared() {
    setState(Volume::State_Shared);
}

void DirectVolume::handleVolumeUnshared() {
    setState(Volume::State_Idle);
}

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
static bool isAuto(int partIdx) {
    return partIdx == -1;
}

//+NATIVE_PLATFORM Removal of VoldResponseCode.VolumeDiskPrepared
#ifdef PATCH_STORAGE_REMOVE_PREPARED_STAGE
bool DirectVolume::initMountpoint(const char *devpath) {
#else
void DirectVolume::initMountpoint(const char *devpath) {
#endif
//-NATIVE_PLATFORM
    if (isAuto(mPartIdx) && mDiskNumParts != 0) {
#ifdef FUNCTION_STORAGE_MOUNT_LARGEST_PARTITION
        mPartIdx = getLargestPartitionIndex() + 1;
#else        
        mPartIdx = getPrimaryPartitionIndex() + 1;
#endif
        SLOGD("initMountpoint mPartIdx=%d", mPartIdx);
    }
    char *nodepath = getNodePath();
    if(nodepath == NULL) {
        SLOGW("initMountpoint() nodepath is NULL");
        return false;
    }
    if (::access(nodepath, R_OK) != 0) {
        SLOGW("initMountPoint ignored. nodepath is not initialized. %s (%s)", nodepath, strerror(errno));
        free(nodepath);
        //+NATIVE_PLATFORM Removal of VoldResponseCode.VolumeDiskPrepared
        #ifdef PATCH_STORAGE_REMOVE_PREPARED_STAGE
        return false;
        #endif
        //-NATIVE_PLATFORM
    }
    //==========================
    // [WR_PATCH] If kernel logical block could not be recognized at handleDiskAdded(), handle it at handleChanged() (2015.6.25)
    // moved from handleDiskAdded() and rearranged. (2015.10.30)
    else {
        if (!isReadable(nodepath)) {
            SLOGW("can not read the node file %s", nodepath);
            setState(Volume::State_NoMedia);
            free(nodepath);
            return false;
        }    
    }
    //==========================
    
    char devType[255]; // external, usb, sd
    getDevType(mFuseMountpoint, devType); // <- changed from mMountpoint for Kitkat

    int32_t volumeId = 0;
#ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    /* Note: Volume label buffer extended to cope with exFAT volume labels,
     * which (once converted to UTF-8) can occupy up to 3 times the maximum
     * number of UTF-16 code units (15, though 11 is the official limit). */
    char volumeLabel[(3 * 15) + 1];
    if (getVolumeInfo(mMountpoint, nodepath, volumeLabel, sizeof(volumeLabel), &volumeId) != 0) {
        SLOGE("cannot retreive filesystem from %s", nodepath);
    }
#else
    char volumeLabel[12];
    if (getVolumeInfo(mFuseMountpoint, nodepath, volumeLabel, &volumeId) != 0) { // <- changed from mMountpoint for Kitkat
        SLOGE("cannot retreive filesystem from %s", nodepath);
    }
#endif

    free(nodepath);
    if (volumeId == -1) {
        SLOGW("No available partition for %d:%d volumeId=%d", mDiskMajor, mDiskMinor, volumeId);
        char msg[255];
        snprintf(msg, sizeof(msg), "Volume %s %s disk noavailable %s",
                getLabel(), getFuseMountpoint(), devType); // <- changed from getMountpoint for Kitkat
        mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskNoAvailable, msg, false);
        //+NATIVE_PLATFORM Removal of VoldResponseCode.VolumeDiskPrepared
        #ifdef PATCH_STORAGE_REMOVE_PREPARED_STAGE
        return false;
        #endif
        //-NATIVE_PLATFORM
    }

    if (!strcmp(mMountpoint, "UNKNOWN")) {
        char mountPath[255];
        snprintf(mountPath, 255, "/mnt/vold/%X", volumeId);
        SLOGD("mMountPoint changed from %s to %s", mMountpoint, mountPath);
        free(mMountpoint);
        mMountpoint = strdup(mountPath);
    }
    mkdir(mMountpoint, 0755);

    //+NATIVE_PLATFORM Removal of VoldResponseCode.VolumeDiskPrepared
    #ifdef PATCH_STORAGE_REMOVE_PREPARED_STAGE
    strcpy(mDevType, devType);
    mVolumeId = volumeId;
    strcpy(mVolumeLabel, volumeLabel);

    return true;
    #else
    char msg[255];
    snprintf(msg, sizeof(msg), "Volume %s %s disk prepared (%d:%d) %s %s %d",
            getLabel(), getFuseMountpoint(), mDiskMajor, mDiskMinor, devType, volumeLabel, volumeId); // <- changed from getMountpoint for Kitkat
    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskPrepared, msg, false);
    #endif
    //-NATIVE_PLATFORM
}
#endif
//-NATIVE_PLATFORM

int DirectVolume::handleBlockEvent(NetlinkEvent *evt) {
    const char *dp = evt->findParam("DEVPATH");

    int connectedType = 0; // For telechips
    PathCollection::iterator  it;
    for (it = mPaths->begin(); it != mPaths->end(); ++it) {
        connectedType++; // For telechips
        if (!strncmp(dp, *it, strlen(*it))) {
            /* We can handle this disk */
            int action = evt->getAction();
            const char *devtype = evt->findParam("DEVTYPE");
            int major = atoi(evt->findParam("MAJOR")); // For telechips 
            int minor = atoi(evt->findParam("MINOR")); // For telechips 

            if (action == NetlinkEvent::NlActionAdd) {
                // For telechips int major = atoi(evt->findParam("MAJOR"));
                // For telechips int minor = atoi(evt->findParam("MINOR"));
                char nodepath[255];

                snprintf(nodepath,
                         sizeof(nodepath), "/dev/block/vold/%d:%d",
                         major, minor);
                if (createDeviceNode(nodepath, major, minor)) {
                    SLOGE("Error making device node '%s' (%s)", nodepath,
                                                               strerror(errno));
                }
                if (!strcmp(devtype, "disk")) {
                    //===========================
                    // For telechips
                    const char *tmp = evt->findParam("NPARTS");
                    if ((mDiskMajor != -1) ||
                           (major == 240 && (tmp==NULL || atoi(tmp)==0))) {
                        break;
                    }
                    setStorageType(connectedType);
                    //===========================
                    handleDiskAdded(dp, evt);
                } else {
                    //===========================
                    // For telechips
                    if ((mDiskMajor != major) || (mDiskMinor > minor) || (mDiskMinor+15 < minor)) {
                        break;
                    }
                    //===========================
                    handlePartitionAdded(dp, evt);
                }
                /* Send notification iff disk is ready (ie all partitions found) */
                if (getState() == Volume::State_Idle) {
                    char msg[255];

                    //+NATIVE_PLATFORM Removal of VoldResponseCode.VolumeDiskPrepared
                    #ifdef PATCH_STORAGE_REMOVE_PREPARED_STAGE
                    snprintf(msg, sizeof(msg), "Volume %s %s disk prepared (%d:%d) %s %s %d",
                            getLabel(), getFuseMountpoint(), mDiskMajor, mDiskMinor, mDevType, mVolumeLabel, mVolumeId); // <- changed from mMountpoint for Kitkat              
                    #else
                    snprintf(msg, sizeof(msg), "Volume %s %s disk inserted (%d:%d)", 
                            getLabel(), getFuseMountpoint(), mDiskMajor, mDiskMinor); // <- changed from mMountpoint for Kitkat
                    #endif
                    //-NATIVE_PLATFORM
                    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskInserted,
                                                         msg, false);
                }
            } else if (action == NetlinkEvent::NlActionRemove) {
                if (!strcmp(devtype, "disk")) {
                    //===========================
                    // For telechips
                    if ((mDiskMajor != major) || (mDiskMinor != minor)) {
                        break;
                    }
                    setStorageType(NULL);
                    //===========================
                    handleDiskRemoved(dp, evt);
                } else {
                    //===========================
                    // For telechips
                    if ((mDiskMajor != major) || (mDiskMinor > minor) || (mDiskMinor+15 < minor)) {
                        break;
                    }
                    //===========================
                    handlePartitionRemoved(dp, evt);
                }
            } else if (action == NetlinkEvent::NlActionChange) {
                if (!strcmp(devtype, "disk")) {
                    //===========================
                    // For telechips
                    if ((mDiskMajor != major) || (mDiskMinor != minor)) {
                        break;
                    }
                    //===========================
                    handleDiskChanged(dp, evt);
                } else {
                    //===========================
                    // For telechips
                    if ((mDiskMajor != major) || (mDiskMinor > minor) || (mDiskMinor+15 < minor)) {
                        break;
                    }
                    //===========================
                    handlePartitionChanged(dp, evt);
                }
            } else {
                    SLOGW("Ignoring non add/remove/change event");
            }

            return 0;
        }
    }
    errno = ENODEV;
    return -1;
}

void DirectVolume::handleDiskAdded(const char *devpath, NetlinkEvent *evt) {
    //+NATIVE_PLATFORM
    /* duplicated (already checked "mDiskMajor!=-1" before calling this method, so meaningless code)
    if ((mDiskMajor != -1) && (mDiskMinor != -1)) {
        SLOGW("handleDiskAdded:: disk insert ignored. mDiskMajor=%d, mDiskMinor=%d\n", mDiskMajor, mDiskMinor);
        return;
    }
    */
    //-NATIVE_PLATFORM

    mDiskMajor = atoi(evt->findParam("MAJOR"));
    mDiskMinor = atoi(evt->findParam("MINOR"));

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_SUPPORT_CDROM
    if (isCdromPoint(mFuseMountpoint) && !isCdromAvailable()) { // <- changed from mMountpoint for Kitkat
        SLOGW("handleDiskAdded:: cdrom insert ignored.");
        return;
    }
    #endif
    //-NATIVE_PLATFORM

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    int state = getState();
    if (state == Volume::State_Mounted ||
            state == Volume::State_Checking) {
        SLOGW("handleDiskAdded:: disk insert ignored. current state=%d\n", state);
        return;
    }
    #endif
    //-NATIVE_PLATFORM

    const char *tmp = evt->findParam("NPARTS");
    if (tmp) {
        mDiskNumParts = atoi(tmp);
    } else {
        SLOGW("Kernel block uevent missing 'NPARTS'");
        mDiskNumParts = 1;
    }

    int partmask = 0;
    int i;
    for (i = 1; i <= mDiskNumParts; i++) {
        partmask |= (1 << i);
    }
    mPendingPartMap = partmask;

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    markMediaInserted(getFuseMountpoint()); // <- changed from getMountpoint for Kitkat
    mPendingPartMap = (mDiskNumParts <= MAX_PARTITIONS) ? mDiskNumParts : MAX_PARTITIONS;

    //+NATIVE_PLATFORM Removal of VoldResponseCode.VolumeDiskPrepared
    #if ! defined(PATCH_STORAGE_REMOVE_PREPARED_STAGE)
    char devType[255]; // external, usb, sd
    getDevType(mFuseMountpoint, devType); // <- changed from mMountpoint for Kitkat
    SLOGW("handleDiskAdded:: devpath=%s %s", devpath, devType);
    char nodepath[255];
    snprintf(nodepath, sizeof(nodepath), VOLD_NODE_FORMAT, mDiskMajor, mDiskMinor);
    if (access(nodepath, R_OK) == 0) {
        char msg[255];
        //==========================
        // [WR_PATCH] If kernel logical block could not be recognized at handleDiskAdded(), handle it at handleChanged() (2015.6.25)
        // moved into initMountPoint() (2015.10.30)
        if (!isReadable(nodepath)){
            SLOGW("can not read the node file %s", nodepath);
            setState(Volume::State_NoMedia);
            return;
        }
        //==========================
        snprintf(msg, sizeof(msg), "Volume %s %s disk inserted (%d:%d) %s",
                 getLabel(), getFuseMountpoint(), mDiskMajor, mDiskMinor, devType); // <- changed from getMountpoint for Kitkat
        mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskInserted, msg, false);
    } else {
        SLOGW("handleDiskAdded: VolumeDiskInserted ignored. nodepath is not initialized. %s (%s)",
                nodepath, strerror(errno));
    }
    #endif
    //-NATIVE_PLATFORM
#endif
//-NATIVE_PLATFORM

    if (mDiskNumParts == 0) {
#ifdef PARTITION_DEBUG
        SLOGD("Dv::diskIns - No partitions - good to go son!");
#endif
        //===========================
        // For telechips
        const char *devname = evt->findParam("DEVNAME");
        if(devname && !strcmp(devname, "ndda")) {
            return;
	    }
        //===========================
        
        //+NATIVE_PLATFORM Removal of VoldResponseCode.VolumeDiskPrepared
        #ifdef PATCH_STORAGE_REMOVE_PREPARED_STAGE 
        if(initMountpoint(devpath))
        #else
        initMountpoint(devpath);
        #endif
        //-NATIVE_PLATFORM            
            setState(Volume::State_Idle);
    } else {
#ifdef PARTITION_DEBUG
        SLOGD("Dv::diskIns - waiting for %d partitions (mask 0x%x)",
             mDiskNumParts, mPendingPartMap);
#endif
        setState(Volume::State_Pending);
    }
}

void DirectVolume::handlePartitionAdded(const char *devpath, NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));

    int part_num;

    const char *tmp = evt->findParam("PARTN");

    if (tmp) {
        part_num = atoi(tmp);
    } else {
        SLOGW("Kernel block uevent missing 'PARTN'");
        part_num = 1;
    }

    if (part_num > MAX_PARTITIONS || part_num < 1) {
        SLOGE("Invalid 'PARTN' value");
        return;
    }

    if (part_num > mDiskNumParts) {
        mDiskNumParts = part_num;
    }

    if (major != mDiskMajor) {
        SLOGE("Partition '%s' has a different major than its disk!", devpath);
        return;
    }
#ifdef PARTITION_DEBUG
    SLOGD("Dv:partAdd: part_num = %d, minor = %d\n", part_num, minor);
#endif
    if (part_num >= MAX_PARTITIONS) {
        SLOGE("Dv:partAdd: ignoring part_num = %d (max: %d)\n", part_num, MAX_PARTITIONS-1);
//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    } else {  
        if (mPartMinors[part_num - 1] < 0) {
            mPartMinors[part_num - 1] = minor;
            if (mPendingPartMap == 0) {
                SLOGW("partAdd: part_num=%d, minor=%d. There was no pending partition", part_num, minor);
            } else {
                mPendingPartMap--;
            }
        } else {
            SLOGW("Dv:partAdd: ignoring part_num=%d which is already received part.\
                    new_minor=%d, old_minor=%d", part_num, minor, mPartMinors[part_num - 1]);
        }
    }
#else
    } else {
        mPartMinors[part_num -1] = minor;
    }
    mPendingPartMap &= ~(1 << part_num);
#endif    
//-NATIVE_PLATFORM

    if (!mPendingPartMap) {
#ifdef PARTITION_DEBUG
        SLOGD("Dv:partAdd: Got all partitions - ready to rock!");
#endif
        // For telechips if (getState() != Volume::State_Formatting) {
        if (getState() != Volume::State_Formatting && getState() != Volume::State_Mounted) { // For telechips 
            //+NATIVE_PLATFORM Removal of VoldResponseCode.VolumeDiskPrepared
            #ifdef PATCH_STORAGE_REMOVE_PREPARED_STAGE
            if(initMountpoint(devpath))           
            #else
            initMountpoint(devpath);
            #endif
            //-NATIVE_PLATFORM
                setState(Volume::State_Idle);
                
            if (mRetryMount == true) {
                mRetryMount = false;
                mountVol();
            }
        }
    } else {
#ifdef PARTITION_DEBUG
        SLOGD("Dv:partAdd: pending mask now = 0x%x", mPendingPartMap);
#endif
    }
}

void DirectVolume::handleDiskChanged(const char *devpath, NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));

    if ((major != mDiskMajor) || (minor != mDiskMinor)) {
        return;
    }

    SLOGI("Volume %s disk has changed", getLabel());
    const char *tmp = evt->findParam("NPARTS");
    if (tmp) {
        mDiskNumParts = atoi(tmp);
    } else {
        SLOGW("Kernel block uevent missing 'NPARTS'");
        mDiskNumParts = 1;
    }

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    mPendingPartMap = (mDiskNumParts <= MAX_PARTITIONS) ? mDiskNumParts : MAX_PARTITIONS;
    for (int i=0; i<MAX_PARTITIONS; i++) {
        mPartMinors[i] = -1;
    }
#else
    int partmask = 0;
    int i;
    for (i = 1; i <= mDiskNumParts; i++) {
        partmask |= (1 << i);
    }
    mPendingPartMap = partmask;
#endif
//-NATIVE_PLATFORM

    if (getState() != Volume::State_Formatting) {
        if (mDiskNumParts == 0) {
            //+NATIVE_PLATFORM
            // [WR_PATCH] If kernel logical block could not be recognized at handleDiskAdded(), handle it at handleChanged() (2015.6.25)
            #ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
            if (getState() == Volume::State_NoMedia) {
                SLOGI("try to initialize mount point (dev path : %s)", devpath);
                char *nodepath = getNodePath();
                if(nodepath == NULL) {
                    SLOGW("handleDiskChanged() nodepath is NULL");
                    return;
                }
                if (!isReadable(nodepath)) {
                    SLOGW("can not read the node file %s", nodepath);
                    free(nodepath);
                    return;
                }
                free(nodepath);
                //+NATIVE_PLATFORM Removal of VoldResponseCode.VolumeDiskPrepared
                #ifdef PATCH_STORAGE_REMOVE_PREPARED_STAGE
                if(initMountpoint(devpath))
                    setState(Volume::State_Idle);
                #else
                initMountpoint(devpath);
                #endif
            }
            #endif
            //-NATIVE_PLATFORM
            //+NATIVE_PLATFORM Removal of VoldResponseCode.VolumeDiskPrepared
            #ifdef PATCH_STORAGE_REMOVE_PREPARED_STAGE
            else // 'else' might be needed 
            #endif
            //==========================
                setState(Volume::State_Idle);
        } else {
            setState(Volume::State_Pending);
        }
    }
}

void DirectVolume::handlePartitionChanged(const char *devpath, NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    SLOGD("Volume %s %s partition %d:%d changed\n", getLabel(), getMountpoint(), major, minor);
}

void DirectVolume::handleDiskRemoved(const char *devpath, NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
    char msg[255];
    bool enabled;

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    unmarkMediaInserted(getFuseMountpoint()); // <- changed from getMountpoint for Kitkat
    #endif
    //-NATIVE_PLATFORM

	//+NATIVE_PLATFORM initialize format of usb
    #ifdef FUNCTION_DEFINE_USB_FORMAT
    if (isUsbMountPoint(getFuseMountpoint())) { // <- changed from getMountpoint for Kitkat
	    property_set("sys.usb.format", "0");
    }
    #endif  
	//-NATIVE_PLATFORM

    setRemoveState(1); // For telechips

    if (mVm->shareEnabled(getLabel(), "ums", &enabled) == 0 && enabled) {
        mVm->unshareVolume(getLabel(), "ums");
    }

    //===========================
    // For telechips
    if (mDiskNumParts == 0) {
        UnmountUnshare(major, minor);
    }
    //===========================

    SLOGD("Volume %s %s disk %d:%d removed\n", getLabel(), getMountpoint(), major, minor);

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    if (getState() == Volume::State_NoMedia) {
       SLOGD("Volume %s has no media, skip to emit disk removed event", getLabel());
    } else {
        snprintf(msg, sizeof(msg), "Volume %s %s disk removed (%d:%d)",
                 getLabel(), getFuseMountpoint(), major, minor); // <- changed from getMountpoint for Kitkat
        mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskRemoved,
                                                 msg, false);
    }
    #else
    snprintf(msg, sizeof(msg), "Volume %s %s disk removed (%d:%d)",
             getLabel(), getFuseMountpoint(), major, minor); // <- changed from getMountpoint for Kitkat
    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeDiskRemoved,
                                             msg, false);
    #endif
    //-NATIVE_PLATFORM
    
    setState(Volume::State_NoMedia);

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    /* [windriver] fix partition counting [START] */
    int i;
    char nodepath[255];
    SLOGD("handledisk removed mDiskMajor=%d, mDiskMinor=%d", mDiskMajor, mDiskMinor);
    for (i=0; i<MAX_PARTITIONS; i++) {
        int minor = mPartMinors[i];
        SLOGD("handledisk removed part minors=%d", minor);
        if (minor < 0) {
            continue;
        }
        snprintf(nodepath, sizeof(nodepath), VOLD_NODE_FORMAT, mDiskMajor, minor);
        if (!access(nodepath, F_OK)) {
            unlink(nodepath);
        }
        mPartMinors[i] = -1;
    }
    snprintf(nodepath, sizeof(nodepath), VOLD_NODE_FORMAT, mDiskMajor, mDiskMinor);
    if (!access(nodepath, F_OK)) {
        unlink(nodepath);
    }
    mPartIdx = mOrigPartIdx;
    /* [windriver] fix partition counting [END] */
    #endif
    //-NATIVE_PLATFORM
    
    //===========================
    // For telechips
    mDiskMajor = -1;
    mDiskMinor = -1;
    mDiskNumParts = 0;

    setRemoveState(0);
    //===========================
}

void DirectVolume::handlePartitionRemoved(const char *devpath, NetlinkEvent *evt) {
    int major = atoi(evt->findParam("MAJOR"));
    int minor = atoi(evt->findParam("MINOR"));
//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    SLOGD("Volume %s %s partition %d:%d removed\n", getLabel(), getMountpoint(), major, minor);

    //140707 Windriver Patch : USB Memory mound error fix - Start
    unmarkMediaInserted(getFuseMountpoint()); //for Automotive Service // <- changed from getMountpoint for Kitkat
    //140707 Windriver Patch : USB MEmory mount error fix - End
	//+NATIVE_PLATFORM initialize format of usb
    #ifdef FUNCTION_DEFINE_USB_FORMAT
    if (isUsbMountPoint(getFuseMountpoint())) { // <- changed from getMountpoint for Kitkat
	    property_set("sys.usb.format", "0");    
    }    
    #endif
	//-NATIVE_PLATFORM

    //===========================
    // For telechips
    setRemoveState(1);
    UnmountUnshare(major, minor);
    setRemoveState(0);
    //===========================
}

/*
 * The framework doesn't need to get notified of partition removal unless it's mounted. Otherwise
 * the removal notification will be sent on the Disk itself
 */
void DirectVolume::UnmountUnshare(int major, int minor) {     // For telechips
#endif
//-NATIVE_PLATFORM
    char msg[255];
    int state;

    state = getState();

    SLOGD("UnmountUnshare, state : %d", state); // For telechips
    if (state != Volume::State_Mounted && state != Volume::State_Shared) {
      if (state != Volume::State_Checking) // For telechips
        return;
    }

    if ((dev_t) MKDEV(major, minor) == mCurrentlyMountedKdev) {
        SLOGD("UnmountUnshare, state is mounted..."); // For telechips
        /*
         * Yikes, our mounted partition is going away!
         */

        bool providesAsec = (getFlags() & VOL_PROVIDES_ASEC) != 0;
        if (providesAsec && mVm->cleanupAsec(this, true)) {
            SLOGE("Failed to cleanup ASEC - unmount will probably fail!");
        }

        snprintf(msg, sizeof(msg), "Volume %s %s bad removal (%d:%d)",
                 getLabel(), getFuseMountpoint(), major, minor);
        mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeBadRemoval,
                                             msg, false);

        if (Volume::unmountVol(true, false)) {
            SLOGE("Failed to unmount volume on bad removal (%s)", 
                 strerror(errno));
            // XXX: At this point we're screwed for now
        } else {
            SLOGD("Crisis averted");
        }
    } else if (state == Volume::State_Shared) {
        SLOGD("UnmountUnshare, state is shared..."); // For telechips
        /* removed during mass storage */
        snprintf(msg, sizeof(msg), "Volume %s %s bad removal (%d:%d)",
                 getLabel(), getMountpoint(), major, minor); // For telechips
        mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeBadRemoval,
                                             msg, false);

        if (mVm->unshareVolume(getLabel(), "ums")) {
            SLOGE("Failed to unshare volume on bad removal (%s)",
                strerror(errno));
        } else {
            SLOGD("Crisis averted");
        }
    //===========================
    // For telechips
    } else if (state == Volume::State_Checking) {
        SLOGD("UnmountUnshare, state is checking...");
        snprintf(msg, sizeof(msg), "Volume %s %s bad removal (%d:%d)",
                 getLabel(), getFuseMountpoint(), major, minor);
        mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeBadRemoval,
                                             msg, false);

    	if (mVm->cleanupAsec(this, true)) {
            SLOGE("Failed to cleanup ASEC - unmount will probably fail!");
        }
    //===========================
    }
}

/*
 * Called from base to get a list of devicenodes for mounting
 */
int DirectVolume::getDeviceNodes(dev_t *devs, int max) {

    if (mPartIdx == -1) {
        // If the disk has no partitions, try the disk itself
        if (!mDiskNumParts) {
            devs[0] = MKDEV(mDiskMajor, mDiskMinor);
            return 1;
        }

        int i;
        for (i = 0; i < mDiskNumParts; i++) {
            if (i == max)
                break;
            devs[i] = MKDEV(mDiskMajor, mPartMinors[i]);
        }
        return mDiskNumParts;
    }
    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    if (mPartMinors[mPartIdx -1] == -1) {
        SLOGI("getDeviceNodes - mDiskMajor : %d, mPartIdx : %d\n", mDiskMajor, mPartIdx);
        mPartMinors[mPartIdx -1] = mPartIdx;
    }
    #endif
    //-NATIVE_PLATFORM
    devs[0] = MKDEV(mDiskMajor, mPartMinors[mPartIdx -1]);
    return 1;
}

/*
 * Called from base to update device info,
 * e.g. When setting up an dm-crypt mapping for the sd card.
 */
int DirectVolume::updateDeviceInfo(char *new_path, int new_major, int new_minor)
{
    PathCollection::iterator it;

    if (mPartIdx == -1) {
        SLOGE("Can only change device info on a partition\n");
        return -1;
    }

    /*
     * This is to change the sysfs path associated with a partition, in particular,
     * for an internal SD card partition that is encrypted.  Thus, the list is
     * expected to be only 1 entry long.  Check that and bail if not.
     */
    if (mPaths->size() != 1) {
        SLOGE("Cannot change path if there are more than one for a volume\n");
        return -1;
    }

    it = mPaths->begin();
    free(*it); /* Free the string storage */
    mPaths->erase(it); /* Remove it from the list */
    addPath(new_path); /* Put the new path on the list */

    /* Save away original info so we can restore it when doing factory reset.
     * Then, when doing the format, it will format the original device in the
     * clear, otherwise it just formats the encrypted device which is not
     * readable when the device boots unencrypted after the reset.
     */
    mOrigDiskMajor = mDiskMajor;
    mOrigDiskMinor = mDiskMinor;
    mOrigPartIdx = mPartIdx;
    memcpy(mOrigPartMinors, mPartMinors, sizeof(mPartMinors));

    mDiskMajor = new_major;
    mDiskMinor = new_minor;
    /* Ugh, virual block devices don't use minor 0 for whole disk and minor > 0 for
     * partition number.  They don't have partitions, they are just virtual block
     * devices, and minor number 0 is the first dm-crypt device.  Luckily the first
     * dm-crypt device is for the userdata partition, which gets minor number 0, and
     * it is not managed by vold.  So the next device is minor number one, which we
     * will call partition one.
     */
    mPartIdx = new_minor;
    mPartMinors[new_minor-1] = new_minor;

    mIsDecrypted = 1;

    return 0;
}

/*
 * Called from base to revert device info to the way it was before a
 * crypto mapping was created for it.
 */
void DirectVolume::revertDeviceInfo(void)
{
    if (mIsDecrypted) {
        mDiskMajor = mOrigDiskMajor;
        mDiskMinor = mOrigDiskMinor;
        mPartIdx = mOrigPartIdx;
        memcpy(mPartMinors, mOrigPartMinors, sizeof(mPartMinors));

        mIsDecrypted = 0;
    }

    return;
}

/*
 * Called from base to give cryptfs all the info it needs to encrypt eligible volumes
 */
int DirectVolume::getVolInfo(struct volume_info *v)
{
    strcpy(v->label, mLabel);
    strcpy(v->mnt_point, mMountpoint);
    v->flags = getFlags();
    /* Other fields of struct volume_info are filled in by the caller or cryptfs.c */

    return 0;
}

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
void DirectVolume::setStorageType(int type)
{
    switch (type) {
    case 1:
        property_set("tcc.primary_storage.type", "Internal Storage(primary)");
        break;
    case 2:
        property_set("tcc.primary_storage.type", "SD Card(primary)");
        break;
    case 3:
    case 4:
    case 5:
        property_set("tcc.primary_storage.type", "USB storage(primary)");
        break;
    case 6:
        property_set("tcc.primary_storage.type", "SATA storage(primary)");
        break;
    default:
        property_set("tcc.primary_storage.type", "Unknown storage(primary)");
    }
}

char * DirectVolume::getDevicePath(void) {
    PathCollection::iterator it = mPaths->begin();
    return (char *)(*it);
}

char * DirectVolume::getNodePath(void) {
    char nodepath[255];
    if (mDiskNumParts == 0) {
        snprintf(nodepath, sizeof(nodepath), VOLD_NODE_FORMAT, mDiskMajor, mDiskMinor);
    } else {
        if (mPartIdx == -1) {
            SLOGW("No available partition for %d:%d mPartIdx=%d", mDiskMajor, mDiskMinor, mPartIdx);
            return NULL;
        }
        snprintf(nodepath, sizeof(nodepath), VOLD_NODE_FORMAT, mDiskMajor, mPartMinors[mPartIdx-1]);
    }
    return strdup(nodepath);
}

#ifdef FUNCTION_STORAGE_MOUNT_LARGEST_PARTITION
int DirectVolume::getLargestPartitionIndex() {
    int i;
    uint64_t size;
    int maxPart = 0;
    uint64_t maxSize = 0;
    //SLOGD("checking partition sizes...");
    //SLOGD("mDiskNumParts=%d", mDiskNumParts);
    char nodepath[255];
    for (i=0; i<MAX_PARTITIONS; i++) {
        int minor = mPartMinors[i];
        if (minor < 0) {
            continue;
        }
        snprintf(nodepath, sizeof(nodepath), VOLD_NODE_FORMAT, mDiskMajor, minor);
        size = getVolumeSize(mFuseMountpoint, nodepath); // <- changed from mMountpoint for Kitkat
        if (size == 0) {
            continue;
        }
        SLOGD("node=%s size=%llu", nodepath, size);
        if (maxSize < size) {
            maxSize = size;
            maxPart = i;
        }
    }

    if (maxSize == 0) {
        SLOGW("No available partition for %d:%d maxSize=%llu", mDiskMajor, mDiskMinor, maxSize);
        return -1;
    }
    SLOGD("Largest partition is %d:%d size=%llu", mDiskMajor, mPartMinors[maxPart], maxSize);
    return maxPart;
}
#else
int DirectVolume::getPrimaryPartitionIndex() {
    int i;
    uint64_t size = 0;
    int primaryPart = 0;

    char nodepath[255];
    for (i=0; i<MAX_PARTITIONS; i++) {
        int minor = mPartMinors[i];
        if (minor < 0) {
            continue;
        }
        snprintf(nodepath, sizeof(nodepath), VOLD_NODE_FORMAT, mDiskMajor, minor);
        size = getVolumeSize(mFuseMountpoint, nodepath); // <- changed from mMountpoint for Kitkat
        if (size == 0) {
            continue;
        }
        SLOGD("node=%s size=%llu", nodepath, size);
        primaryPart = i;
        break;
    }

    if (size == 0) {
        SLOGW("No available partition for %d:%d maxSize=%llu", mDiskMajor, mDiskMinor, size);
        return -1;
    }
    SLOGD("Primary partition is %d:%d size=%llu", mDiskMajor, mPartMinors[primaryPart], size);
    return primaryPart;
}
#endif // FUNCTION_STORAGE_MOUNT_LARGEST_PARTITION
#endif
//-NATIVE_PLATFORM
