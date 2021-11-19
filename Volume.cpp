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

#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>

#include <linux/kdev_t.h>

#include <cutils/properties.h>

#include <diskconfig/diskconfig.h>

#include <private/android_filesystem_config.h>

#define LOG_TAG "Vold"

#include <cutils/fs.h>
#include <cutils/log.h>

#include <string>

#include "Volume.h"
#include "VolumeManager.h"
#include "ResponseCode.h"
#include "Fat.h"
//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_TUXERA_PATCH    
#include "Filesystems.h"
#include "ExFat.h"
#endif
//-NATIVE_PLATFORM
//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_SUPPORT_NTFS    
#include "Ntfs.h" // For telechips
#endif
//-NATIVE_PLATFORM
//+NATIVE_PLATFORM Support Cdrom
#ifdef FUNCTION_STORAGE_SUPPORT_CDROM
#include "fusefs.h"
#endif
//-NATIVE_PLATFORM
#include "Process.h"
#include "cryptfs.h"
//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
#include "utils.h"
#endif
//-NATIVE_PLATFORM

extern "C" void dos_partition_dec(void const *pp, struct dos_partition *d);
extern "C" void dos_partition_enc(void *pp, struct dos_partition *d);


/*
 * Media directory - stuff that only media_rw user can see
 */
const char *Volume::MEDIA_DIR           = "/mnt/media_rw";

/*
 * Fuse directory - location where fuse wrapped filesystems go
 */
const char *Volume::FUSE_DIR           = "/storage";

/*
 * Path to external storage where *only* root can access ASEC image files
 */
const char *Volume::SEC_ASECDIR_EXT   = "/mnt/secure/asec";

/*
 * Path to internal storage where *only* root can access ASEC image files
 */
const char *Volume::SEC_ASECDIR_INT   = "/data/app-asec";

/*
 * Path to where secure containers are mounted
 */
const char *Volume::ASECDIR           = "/mnt/asec";

/*
 * Path to where OBBs are mounted
 */
const char *Volume::LOOPDIR           = "/mnt/obb";

const char *Volume::BLKID_PATH = "/system/bin/blkid";

static const char *stateToStr(int state) {
    if (state == Volume::State_Init)
        return "Initializing";
    else if (state == Volume::State_NoMedia)
        return "No-Media";
    else if (state == Volume::State_Idle)
        return "Idle-Unmounted";
    else if (state == Volume::State_Pending)
        return "Pending";
    else if (state == Volume::State_Mounted)
        return "Mounted";
    else if (state == Volume::State_Unmounting)
        return "Unmounting";
    else if (state == Volume::State_Checking)
        return "Checking";
    else if (state == Volume::State_Formatting)
        return "Formatting";
    else if (state == Volume::State_Shared)
        return "Shared-Unmounted";
    else if (state == Volume::State_SharedMnt)
        return "Shared-Mounted";
    else
        return "Unknown-Error";
}

Volume::Volume(VolumeManager *vm, const fstab_rec* rec, int flags) {
    mVm = vm;
    mDebug = false;
    mLabel = strdup(rec->label);
    mUuid = NULL;
    mUserLabel = NULL;
    mState = Volume::State_Init;
    mFlags = flags;
    mCurrentlyMountedKdev = -1;
    
    //===========================
    // For telechips
    //mPartIdx = rec->partnum;
    mPartIdx = -1;
    //===========================
    
    mRetryMount = false;
    
    //===========================
    // For telechips
    mMultiMount = true;
    mRemoving = 0;
    pthread_mutex_init(&mLock, NULL);
    //===========================

    //+FW_STANDARD Removal of VoldResponseCode.VolumeDiskPrepared
    mVolumeId = -1;
    //-FW_STANDARD
}

Volume::~Volume() {
    pthread_mutex_destroy(&mLock); // For telechips

    free(mLabel);
    free(mUuid);
    free(mUserLabel);
}

void Volume::setDebug(bool enable) {
    mDebug = enable;
}

dev_t Volume::getDiskDevice() {
    return MKDEV(0, 0);
};

dev_t Volume::getShareDevice() {
    return getDiskDevice();
}

void Volume::handleVolumeShared() {
}

void Volume::handleVolumeUnshared() {
}

int Volume::handleBlockEvent(NetlinkEvent *evt) {
    errno = ENOSYS;
    return -1;
}

void Volume::setUuid(const char* uuid) {
    char msg[256];

    if (mUuid) {
        free(mUuid);
    }

    if (uuid) {
        mUuid = strdup(uuid);
        snprintf(msg, sizeof(msg), "%s %s \"%s\"", getLabel(),
                getFuseMountpoint(), mUuid);
    } else {
        mUuid = NULL;
        snprintf(msg, sizeof(msg), "%s %s", getLabel(), getFuseMountpoint());
    }

    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeUuidChange, msg,
            false);
}

void Volume::setUserLabel(const char* userLabel) {
    char msg[256];

    if (mUserLabel) {
        free(mUserLabel);
    }

    if (userLabel) {
        mUserLabel = strdup(userLabel);
        snprintf(msg, sizeof(msg), "%s %s \"%s\"", getLabel(),
                getFuseMountpoint(), mUserLabel);
    } else {
        mUserLabel = NULL;
        snprintf(msg, sizeof(msg), "%s %s", getLabel(), getFuseMountpoint());
    }

    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeUserLabelChange,
            msg, false);
}

void Volume::setState(int state) {
    char msg[255];
    int oldState = mState;

    if (oldState == state) {
        SLOGW("Duplicate state (%d)\n", state);
        return;
    }

    if ((oldState == Volume::State_Pending) && (state != Volume::State_Idle)) {
        mRetryMount = false;
    }

    mState = state;

    SLOGD("Volume %s state changing %d (%s) -> %d (%s)", mLabel,
         oldState, stateToStr(oldState), mState, stateToStr(mState));
    snprintf(msg, sizeof(msg),
             "Volume %s %s state changed from %d (%s) to %d (%s)", getLabel(),
             getFuseMountpoint(), oldState, stateToStr(oldState), mState,
             stateToStr(mState));

    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeStateChange,
                                         msg, false);
}

//===========================
// For telechips
void Volume::setRemoveState(int state) {
    pthread_mutex_lock(&mLock);
    mRemoving = state;
    pthread_mutex_unlock(&mLock);
}
// For telechips

int Volume::createDeviceNode(const char *path, int major, int minor) {
    mode_t mode = 0660 | S_IFBLK;
    dev_t dev = (major << 8) | minor;
    if (mknod(path, mode, dev) < 0) {
        if (errno != EEXIST) {
            return -1;
        }
    }
    return 0;
}

//===========================
// For telechips
void setDevicePath(const char* path, char* devicePath)
{
  char boot_mode[PROPERTY_VALUE_MAX]="";
  char internal_fat_number[PROPERTY_VALUE_MAX]="";
  char quickboot_mode[PROPERTY_VALUE_MAX]="";

  property_get("ro.bootmode", boot_mode, "");
  property_get("tcc.internal.storage.fat.number", internal_fat_number, "");
  property_get("ro.QB.enable", quickboot_mode, "");

  if (!strcmp(quickboot_mode, "1") ||
      !strcmp(internal_fat_number, "12")) {
    if (!strcmp(path, "/mnt/sdcard")) {
      if (!strcmp(boot_mode, "emmc"))
	sprintf(devicePath, "/dev/block/mmcblk0p12");
      else
	sprintf(devicePath, "/dev/block/ndda12");
    } else {
      if (!strcmp(boot_mode, "emmc"))
	sprintf(devicePath, "/dev/block/mmcblk1");
      else
	sprintf(devicePath, "/dev/block/mmcblk0");
    }
  } else {
    if (!strcmp(path, "/mnt/sdcard")) {
      if (!strcmp(boot_mode, "emmc"))
	sprintf(devicePath, "/dev/block/mmcblk0p11");
      else
	sprintf(devicePath, "/dev/block/ndda11");
    } else {
      if (!strcmp(boot_mode, "emmc"))
	sprintf(devicePath, "/dev/block/mmcblk1");
      else
	sprintf(devicePath, "/dev/block/mmcblk0");
    }
  }
}
//===========================

int Volume::formatVol(const char* path, const char* fstype, bool wipe) { // For telechips add path, fstype
    SLOGI("Start Volume::formatVol");
    if (getState() == Volume::State_NoMedia) {
        errno = ENODEV;
        return -1;
    } else if (getState() != Volume::State_Idle) {
        errno = EBUSY;
        return -1;
    }

    if (isMountpointMounted(getMountpoint())) {
        SLOGW("Volume is idle but appears to be mounted - fixing");
        setState(Volume::State_Mounted);
        // mCurrentlyMountedKdev = XXX
        errno = EBUSY;
        return -1;
    }

    bool formatEntireDevice = (mPartIdx == -1);
    char devicePath[255];
    dev_t diskNode = getDiskDevice();
    dev_t partNode =
        MKDEV(MAJOR(diskNode),
              MINOR(diskNode) + (formatEntireDevice ? 1 : mPartIdx));

    setState(Volume::State_Formatting);

    int ret = -1;

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    sprintf(devicePath, "/dev/block/vold/%d:%d",
            MAJOR(diskNode), MINOR(diskNode));
    const int formatExfat = ExFat::checkSize(devicePath);
    if (formatExfat == -1) {
        SLOGE("Failed to determine size of device (%s).", strerror(errno));
        goto err;
    }
    else if (formatExfat != 0 && formatExfat != 1) {
        SLOGE("Unknown return value from ExFat::checkSize: %d", formatExfat);
        goto err;
    }
    #endif
    //-NATIVE_PLATFORM

    // Only initialize the MBR if we are formatting the entire device
    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    if (formatEntireDevice && !formatExfat) {
    #else
    if (formatEntireDevice) {
    #endif
    //-NATIVE_PLATFORM
        sprintf(devicePath, "/dev/block/vold/%d:%d",
                MAJOR(diskNode), MINOR(diskNode));

#if 0  // For telechips
        // Dose not Make MBR For NAND and SDcard when Formatting Volume - B100044
        if (initializeMbr(devicePath)) {
            SLOGE("Failed to initialize MBR (%s)", strerror(errno));
            goto err;
        }
#endif
    }

    sprintf(devicePath, "/dev/block/vold/%d:%d",
            MAJOR(partNode), MINOR(partNode));
		
    //===========================
    // For telechips
    #if 0 
	char boot_mode[512];
	property_get("ro.bootmode", boot_mode, "");
	if (!strcmp(boot_mode, "emmc"))
		sprintf(devicePath, "/dev/block/mmcblk1");
	else
		sprintf(devicePath, "/dev/block/mmcblk0");
    #endif
    //===========================

    if (mDebug) {
        SLOGI("Formatting volume %s (%s)", getLabel(), devicePath);
    }
    
    //===========================
    // For telechips    
    //setDevicePath(path, devicePath);
    strncpy(devicePath, mDevicePath, strlen(mDevicePath));
    SLOGI("mDevicePath %s, devicePath %s", mDevicePath, devicePath);
    SLOGI("Formatting volume %s (%s)", getLabel(), devicePath);

    if (!strcmp(fstype, "ntfs")) {
        if (Ntfs::format(devicePath, 0)) {
	        SLOGE("Failed to format (%s)", strerror(errno));
	        goto err;
        }
    } else 
    //+NATIVE_PLATFORM Support Cdrom
    #ifdef FUNCTION_STORAGE_SUPPORT_CDROM    
    if (!strcmp(fstype , "iso9660")) {
        SLOGE("Failed to format cdfs");
        goto err;
    } else
    #endif
    //-NATIVE_PLATFORM
    {
        //+NATIVE_PLATFORM
        #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
        if (formatExfat == 1) {
            if (formatEntireDevice) {
                sprintf(devicePath, "/dev/block/vold/%d:%d",
                        MAJOR(diskNode), MINOR(diskNode));
                if (ExFat::format(devicePath, 0, true)) {
                    SLOGE("Failed to format with exFAT (%s) (%s)",
                        strerror(errno), devicePath);
                    goto err;
                }
            }
            else if (ExFat::format(devicePath, 0, false)) {
                SLOGE("Failed to format with exFAT (%s) (%s)",
                    strerror(errno), devicePath);
                goto err;
            }
        }
        else
        #endif
        //-NATIVE_PLATFORM
        if (Fat::format(devicePath, 0, wipe)) {
	        SLOGE("Failed to format (%s)", strerror(errno));
            goto err;
        }
    }

    ret = 0;

err:
    setState(Volume::State_Idle);
    return ret;
}

bool Volume::isMountpointMounted(const char *path) {
    char device[256];
    char mount_path[256];
    char rest[256];
    FILE *fp;
    char line[1024];

    if (!(fp = fopen("/proc/mounts", "r"))) {
        SLOGE("Error opening /proc/mounts (%s)", strerror(errno));
        return false;
    }

    while(fgets(line, sizeof(line), fp)) {
        line[strlen(line)-1] = '\0';
        sscanf(line, "%255s %255s %255s\n", device, mount_path, rest);
        if (!strcmp(mount_path, path)) {
            fclose(fp);
            return true;
        }
    }

    fclose(fp);
    return false;
}

int Volume::mountVol() {
//===========================
// For telechips
    int iRet = -1;

    pthread_mutex_lock(&mLock);
    if (mRemoving == 0) {
        iRet = mountVol_l();
    }
    pthread_mutex_unlock(&mLock);

    return iRet;
}

int Volume::mountVol_l() {
//===========================
    dev_t deviceNodes[MAX_MOUNT_PART]; // For telechips
    int mask, n, i, rc = 0; // For telechips add variable 'mask'
    char errmsg[255];

    int flags = getFlags();
    bool providesAsec = (flags & VOL_PROVIDES_ASEC) != 0;

    // TODO: handle "bind" style mounts, for emulated storage

    char decrypt_state[PROPERTY_VALUE_MAX];
    char crypto_state[PROPERTY_VALUE_MAX];
    char encrypt_progress[PROPERTY_VALUE_MAX];

    property_get("vold.decrypt", decrypt_state, "");
    property_get("vold.encrypt_progress", encrypt_progress, "");

    /* Don't try to mount the volumes if we have not yet entered the disk password
     * or are in the process of encrypting.
     */
    if ((getState() == Volume::State_NoMedia) ||
        ((!strcmp(decrypt_state, "1") || encrypt_progress[0]) && providesAsec)) {
        snprintf(errmsg, sizeof(errmsg),
                 "Volume %s %s mount failed - no media",
                 getLabel(), getFuseMountpoint());
        mVm->getBroadcaster()->sendBroadcast(
                                         ResponseCode::VolumeMountFailedNoMedia,
                                         errmsg, false);
        errno = ENODEV;
        return -1;
    } else if (getState() != Volume::State_Idle) {
        errno = EBUSY;
        if (getState() == Volume::State_Pending) {
            mRetryMount = true;
        }
        return -1;
    }

    if (isMountpointMounted(getMountpoint())) {
        // ==================================
        // for Automotive Service workaround
        // BSP request workaround until fix the root problem
        /*
        SLOGW("Volume is idle but appears to be mounted - fixing");
        setState(Volume::State_Mounted);
        // mCurrentlyMountedKdev = XXX
        return 0;
        */
        doUnmount(getMountpoint(), true);
        // ==================================
    }

    n = getDeviceNodes((dev_t *) &deviceNodes, MAX_MOUNT_PART); // For telechips
    if (!n) {
        SLOGE("Failed to get device nodes (%s)\n", strerror(errno));
        return -1;
    }

    /* If we're running encrypted, and the volume is marked as encryptable and nonremovable,
     * and also marked as providing Asec storage, then we need to decrypt
     * that partition, and update the volume object to point to it's new decrypted
     * block device
     */
    property_get("ro.crypto.state", crypto_state, "");
    if (providesAsec &&
        ((flags & (VOL_NONREMOVABLE | VOL_ENCRYPTABLE))==(VOL_NONREMOVABLE | VOL_ENCRYPTABLE)) &&
        !strcmp(crypto_state, "encrypted") && !isDecrypted()) {
       char new_sys_path[MAXPATHLEN];
       char nodepath[256];
       int new_major, new_minor;

       if (n != 1) {
           /* We only expect one device node returned when mounting encryptable volumes */
           SLOGE("Too many device nodes returned when mounting %d\n", getMountpoint());
           return -1;
       }

       if (cryptfs_setup_volume(getLabel(), MAJOR(deviceNodes[0]), MINOR(deviceNodes[0]),
                                new_sys_path, sizeof(new_sys_path),
                                &new_major, &new_minor)) {
           SLOGE("Cannot setup encryption mapping for %d\n", getMountpoint());
           return -1;
       }
       /* We now have the new sysfs path for the decrypted block device, and the
        * majore and minor numbers for it.  So, create the device, update the
        * path to the new sysfs path, and continue.
        */
        snprintf(nodepath,
                 sizeof(nodepath), "/dev/block/vold/%d:%d",
                 new_major, new_minor);
        if (createDeviceNode(nodepath, new_major, new_minor)) {
            SLOGE("Error making device node '%s' (%s)", nodepath,
                                                       strerror(errno));
        }

        // Todo: Either create sys filename from nodepath, or pass in bogus path so
        //       vold ignores state changes on this internal device.
        updateDeviceInfo(nodepath, new_major, new_minor);

        /* Get the device nodes again, because they just changed */
        n = getDeviceNodes((dev_t *) &deviceNodes, MAX_MOUNT_PART); // For telechips
        if (!n) {
            SLOGE("Failed to get device nodes (%s)\n", strerror(errno));
            return -1;
        }
    }

    //===========================
    // For telechips
    if (n > MAX_MOUNT_PART) {
        n = MAX_MOUNT_PART;
    }
    //===========================

    for (i = 0; i < n; i++) {
        char devicePath[255];

        sprintf(devicePath, "/dev/block/vold/%d:%d", MAJOR(deviceNodes[i]),
                MINOR(deviceNodes[i]));

        //===========================
        // For telechips
        strncpy(mDevicePath, devicePath, strlen(devicePath));
        SLOGI("mDevicePath %s", mDevicePath);
        //===========================

        SLOGI("%s being considered for volume %s\n", devicePath, getLabel());

        errno = 0;
        setState(Volume::State_Checking);

        #if 0 // not used
        if (Fat::check(devicePath)) {
    	    if (Ntfs::check(devicePath)) { // For telechips add ntfs
    	        SLOGW("%s does not contain a FAT or NTFS filesystem\n", devicePath);
              
                #if 0 // For telechips
                if (errno == ENODATA) {
                    SLOGW("%s does not contain a FAT or NTFS filesystem\n", devicePath);
                    continue;
                }
                errno = EIO;
                /* Badness - abort the mount */
                SLOGE("%s failed FS checks (%s)", devicePath, strerror(errno));
                setState(Volume::State_Idle);
                return -1;
                #endif
            }
        }
        #endif

        errno = 0;

        //===========================
        // For telechips
        if (flags & VOL_NOFUSE) {
            mask = 0002;
        } else {
            mask = 0007;
        }
        //===========================

        #if 1 // merged telechips code from daudio to mount cdfs, exfat, and etc.
        if (mountPartition(devicePath, getMountpoint(), AID_MEDIA_RW, AID_MEDIA_RW, mask) != 0) {
            continue;
        }        
        #else
        if (Fat::doMount(devicePath, getMountpoint(), false, false, false,
                AID_MEDIA_RW, AID_MEDIA_RW, mask, true)) {
        if (Ntfs::doMount(devicePath, getMountpoint(), false, false, false,
                AID_MEDIA_RW, AID_MEDIA_RW, mask, true)) {  // For telechips add ntfs
            SLOGE("%s failed to mount via VFAT or NTFS (%s)\n", devicePath, strerror(errno));
            continue;
        }
        }
        #endif
        
        extractMetadata(devicePath);

        if (providesAsec && mountAsecExternal() != 0) {
            SLOGE("Failed to mount secure area (%s)", strerror(errno));
            umount(getMountpoint());
            setState(Volume::State_Idle);
            return -1;
        }

        char service[64];
        snprintf(service, 64, "fuse_%s", getLabel());
        property_set("ctl.start", service);

        // For telechips setState(Volume::State_Mounted);
        mCurrentlyMountedKdev = deviceNodes[i];

        //===========================
        // For telechips
        if (mMultiMount) {
            int mounted = 1;
            char mountPoint[255];

            while (++i < n) {
                sprintf(devicePath, "/dev/block/vold/%d:%d", MAJOR(deviceNodes[i]),
                        MINOR(deviceNodes[i]));

                SLOGI("%s being considered for volume %s:%d\n", devicePath, getLabel(), i+1);
                sprintf(mountPoint, "%s/%s%d", getMountpoint(), getLabel(), mounted+1);
                if (mountPartition(devicePath, mountPoint, AID_MEDIA_RW, AID_MEDIA_RW, mask) == 0) {
                    mounted++;
                }
            }
        }
        //===========================

        //+NATIVE_PLATFORM
        #ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
        int state = getState();
        if (state != Volume::State_Checking) {
            SLOGE("media status for %s is not expected, abort mount: %s", getMountpoint(), stateToStr(state));
            if (umount(getMountpoint()) != 0) {
                SLOGE("unmount %s failed: %s", getMountpoint(), strerror(errno));
            }
            return -1;
        }

        setState(Volume::State_Mounted); // For telechips 
        #endif
        //-NATIVE_PLATFORM
        
        return 0;
    }

    SLOGE("Volume %s found no suitable devices for mounting :(\n", getLabel());
    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_FOR_AUTOMOTIVE
    if (getState() != Volume::State_NoMedia) {
        setState(Volume::State_Idle);
    } 
    #else
    setState(Volume::State_Idle);
    #endif
    //-NATIVE_PLATFORM
    
    return -1;
}

//===========================
// For telechips
#ifdef FUNCTION_STORAGE_TUXERA_PATCH    
int Volume::mountPartition(const char *devicePath, const char *mountPoint, int uid, int gid, int mask)
{
    char fscheck_sdcard[PROPERTY_VALUE_MAX];
    char fscheck_usb[PROPERTY_VALUE_MAX];
    bool disableFsChecks;
    FSType recognizedFS = FSTYPE_UNRECOGNIZED;

    property_get("tcc.checkdisk.disable.sdcard", fscheck_sdcard, "1");
    property_get("tcc.checkdisk.disable.usb", fscheck_usb, "1");

    if(strncmp(&devicePath[16], "8:", 2)) {
        disableFsChecks = (fscheck_sdcard[0] != '0' || fscheck_sdcard[1] != '\0');    // sdcard
    }
    else {
        disableFsChecks = (fscheck_usb[0] != '0' || fscheck_usb[1] != '\0');    // usb
    }

    //+NATIVE_PLATFORM Support Cdrom
    #ifdef FUNCTION_STORAGE_SUPPORT_CDROM
    if (isCdromPoint(getFuseMountpoint())) { // <- changed from getMountpoint for Kitkat
        #if 0 // not used
        // FIXME
        if (0 && !strcmp(fscheck, "0")) {
            SLOGW("%s does not contain a CDFS filesystem\n", devicePath);
            return -1;
        }
        #endif
    }
    else 
    #endif
    //-NATIVE_PLATFORM
	{
        if (Filesystems::detect(devicePath, &recognizedFS) ||
            recognizedFS == FSTYPE_UNRECOGNIZED) 
		{
            SLOGW("%s does not contain a recognized filesystem\n", devicePath);
			//+NATIVE_PLATFORM set format of usb
            #ifdef FUNCTION_DEFINE_USB_FORMAT
            if(isUsbMountPoint(getFuseMountpoint())) { // <- changed from getMountpoint for Kitkat
			    property_set("sys.usb.format", "4");
            }
            #endif
			//-NATIVE_PLATFORM
            return -2;
        }

		//+NATIVE_PLATFORM set format of usb
        #ifdef FUNCTION_DEFINE_USB_FORMAT
        if(isUsbMountPoint(getFuseMountpoint())) { // <- changed from getMountpoint for Kitkat
            switch(recognizedFS) {
                case FSTYPE_FAT:
                    property_set("sys.usb.format", "1");
                    break;
                case FSTYPE_EXFAT:
                    property_set("sys.usb.format", "2");
                    break;
                case FSTYPE_NTFS:
                    property_set("sys.usb.format", "3");
                    break;
                case FSTYPE_HFSPLUS:
                    property_set("sys.usb.format", "4");
                    break;
                default:
                    SLOGW("Illegal case of recognized filesystem\n");
                    break;
            }
        }
        #endif
		//-NATIVE_PLATFORM

        if (!Filesystems::isSupported(recognizedFS)) {
            SLOGW("%s contains an unsupported filesystem (%s)\n", devicePath,
                  Filesystems::fsName(recognizedFS));
            return -2;
        }

        if (!disableFsChecks && Filesystems::check(recognizedFS, devicePath)) {
            if (recognizedFS == FSTYPE_FAT && errno == ENODATA) {
                /* Remove when reliable FAT detection code has been added. */
                SLOGW("%s does not contain a FAT filesystem\n", devicePath);
                return -2;
            }
            errno = EIO;
            /* Badness - abort the mount */
            SLOGE("%s failed FS checks (%s)", devicePath, strerror(errno));
            setState(Volume::State_Idle);
            return -1;
		}
    }

    // added exfat, ntfs to writable (2017.09.04)
    #ifdef FEATURE_ENABLE_NTFS_EXFAT_READWRITE
    bool isWritableUsb = ((recognizedFS == FSTYPE_FAT) 
                        || (recognizedFS == FSTYPE_EXFAT) 
                        || (recognizedFS == FSTYPE_NTFS));
    #else
    bool isWritableUsb = (recognizedFS == FSTYPE_FAT);
    #endif
    bool readonly = isReadOnlyMedia(getFuseMountpoint(),(int)isWritableUsb); // <- changed from getMountpoint for Kitkat
    
    mkdir(mountPoint, mask);

    if (recognizedFS != FSTYPE_UNRECOGNIZED && Filesystems::doMount(recognizedFS, devicePath, mountPoint, readonly, false, false,
            uid, gid, mask, true)) {
        SLOGE("%s failed to mount via %s (%s)\n", Filesystems::fsName(recognizedFS), devicePath, strerror(errno));
        return -3;
    }

    //+NATIVE_PLATFORM Support Cdrom
    #ifdef FUNCTION_STORAGE_SUPPORT_CDROM
    // === For Lollipop ===
    // changed mountPoint -> getFuseMountpoint() for cdrom 
    // because mounting cdrom use not aosp 'fuse_cdrom' service(/system/bin/sdcard at init.tcc893x.rc) 
    // but daudio own 'cdrom_nameconv' service(/system/bin/fs_nameconv at init.daudio.rc)
    if (recognizedFS == FSTYPE_UNRECOGNIZED && FuseFS::doMount(devicePath, getFuseMountpoint(), readonly, false, false,
            uid, gid, mask, false)) {
        SLOGE("%s failed to mount via FuseFS (%s)\n", devicePath, strerror(errno));
        return -3;

    }
    #endif
    //-NATIVE_PLATFORM

    return 0;
}
#else
int Volume::mountPartition(char *devicePath, char *mountPoint, int uid, int gid, int mask) {
    int fstype = NOT_CHECK;
    char fscheck[PROPERTY_VALUE_MAX];

    property_get("tcc.checkdisk.disable", fscheck, "0");
    //+NATIVE_PLATFORM Support Cdrom
    #ifdef FUNCTION_STORAGE_SUPPORT_CDROM
    if (isCdromPoint(getFuseMountpoint())) { // <- changed from getMountpoint for Kitkat
        fstype = CDFS;
        // FIXME
        if (0 && !strcmp(fscheck, "0")) {
            SLOGW("%s does not contain a CDFS filesystem\n", devicePath);
            return -1;
        }
	} else
    #endif
    //-NATIVE_PLATFORM
    if (!strcmp(fscheck, "0")&& strncmp(&devicePath[16], "8:", 2)) {
        fstype = FAT;
        if (Fat::check(devicePath)) {
            //+NATIVE_PLATFORM
            #ifdef FUNCTION_STORAGE_SUPPORT_NTFS
            fstype = NTFS;
            if (Ntfs::check(devicePath)) 
            #endif
            //-NATIVE_PLATFORM
            {
                if (errno == ENODATA) {
                    SLOGW("%s does not contain a FAT(NTFS) filesystem\n", devicePath);
                    return -1;
                }
                errno = EIO;
                /* Badness - abort the mount */
                SLOGE("%s failed FS checks (%s)", devicePath, strerror(errno));
                return -2;
            }
        }
    } else {
        SLOGW("Skip check disk : %s\n", devicePath);
    }
    
    // added ntfs to writable (2017.09.04), (exfat is only supported with TUXERA_PATCH)
    #ifdef FEATURE_ENABLE_NTFS_EXFAT_READWRITE
    bool isWritableUsb = ((fstype == FAT) 
                        || (fstype == NTFS));
    #else
    bool isWritableUsb = (recognizedFS == FSTYPE_FAT);
    #endif
    bool readonly = isReadOnlyMedia(getFuseMountpoint(), (int)isWritableUsb); // <- changed from getMountpoint for Kitkat
    
    mkdir(mountPoint, 0007);
    if (fstype == FAT) {
        if (Fat::doMount(devicePath, mountPoint, readonly, false, false,
                uid, gid, mask, true)) {
            SLOGE("%s failed to mount via VFAT (%s)\n", devicePath, strerror(errno));
            return -3;
        }
    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_SUPPORT_NTFS
    } else if (fstype == NTFS) {
        if (Ntfs::doMount(devicePath, mountPoint, readonly, false, false,
                uid, gid, mask, true)) {
            SLOGE("%s failed to mount via NTFS (%s)\n", devicePath, strerror(errno));
            return -3;
        }
    #endif
    //-NATIVE_PLATFORM
    //+NATIVE_PLATFORM Support Cdrom
    #ifdef FUNCTION_STORAGE_SUPPORT_CDROM
    } else if (fstype == CDFS) {
        // === For Lollipop ===
        // changed mountPoint -> getFuseMountPoint() for cdrom 
        // because mounting cdrom use not aosp 'fuse_cdrom' service(/system/bin/sdcard at init.tcc893x.rc) 
        // but daudio own 'cdrom_nameconv' service(/system/bin/fs_nameconv at init.daudio.rc)
        if (FuseFS::doMount(devicePath, getFuseMountpoint(), readonly, false, false, // <- changed from getMountpoint for Kitkat
                uid, gid, uid, false)) {
            SLOGE("%s failed to mount via FuseFS (%s)\n", devicePath, strerror(errno));
            return -3;
        }
    #endif
    //-NATIVE_PLATFORM
    } else {
        if (Fat::doMount(devicePath, mountPoint, readonly, false, false,
                uid, gid, mask, true)) {
            //if (Ntfs::doMount(devicePath, mountPoint, false, false, false,
            //        uid, gid, mask, true)) {
                SLOGE("%s failed to mount(No check disk)", devicePath);
                return -3;
            //}
        }
    }
    return 0;
}
#endif // FUNCTION_STORAGE_TUXERA_PATCH
//===========================

int Volume::mountAsecExternal() {
    char legacy_path[PATH_MAX];
    char secure_path[PATH_MAX];

    snprintf(legacy_path, PATH_MAX, "%s/android_secure", getMountpoint());
    snprintf(secure_path, PATH_MAX, "%s/.android_secure", getMountpoint());

    // Recover legacy secure path
    if (!access(legacy_path, R_OK | X_OK) && access(secure_path, R_OK | X_OK)) {
        if (rename(legacy_path, secure_path)) {
            SLOGE("Failed to rename legacy asec dir (%s)", strerror(errno));
        }
    }

    if (fs_prepare_dir(secure_path, 0770, AID_MEDIA_RW, AID_MEDIA_RW) != 0) {
        SLOGW("fs_prepare_dir failed: %s", strerror(errno));
        return -1;
    }

    if (mount(secure_path, SEC_ASECDIR_EXT, "", MS_BIND, NULL)) {
        SLOGE("Failed to bind mount points %s -> %s (%s)", secure_path,
                SEC_ASECDIR_EXT, strerror(errno));
        return -1;
    }

    return 0;
}

int Volume::doUnmount(const char *path, bool force) {
    int retries = 10;

    if (mDebug) {
        SLOGD("Unmounting {%s}, force = %d", path, force);
    }

    while (retries--) {
        #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
        if (!umount2(path, MNT_DETACH) || errno == EINVAL || errno == ENOENT || errno == EIO) {
        #else
        if (!umount2(path, MNT_DETACH) || errno == EINVAL || errno == ENOENT) { // For telechips
        #endif
            SLOGI("%s sucessfully unmounted", path);
            return 0;
        }

        int action = 0;

        if (force) {
            if (retries == 1) {
                action = 2; // SIGKILL
            } else if (retries == 2) {
                action = 1; // SIGHUP
            }
        }

        SLOGW("Failed to unmount %s (%s, retries %d, action %d)",
                path, strerror(errno), retries, action);

        Process::killProcessesWithOpenFiles(path, action);
        usleep(1000*1000);
    }
    errno = EBUSY;
    SLOGE("Giving up on unmount %s (%s)", path, strerror(errno));
    return -1;
}

int Volume::unmountVol(bool force, bool revert) {
//===========================
// For telechips
    int iRet;

    pthread_mutex_lock(&mLock);
    iRet = unmountVol_l(force, revert);
    pthread_mutex_unlock(&mLock);
    return iRet;
}

int Volume::unmountVol_l(bool force, bool revert) {
//===========================
    int i, rc;

    int flags = getFlags();
    bool providesAsec = (flags & VOL_PROVIDES_ASEC) != 0;

    if (getState() != Volume::State_Mounted) {
        SLOGE("Volume %s unmount request when not mounted", getLabel());
        errno = EINVAL;
        return UNMOUNT_NOT_MOUNTED_ERR;
    }

    setState(Volume::State_Unmounting);
    usleep(1000 * 1000); // Give the framework some time to react

    char service[64];
    snprintf(service, 64, "fuse_%s", getLabel());
    property_set("ctl.stop", service);
    /* Give it a chance to stop.  I wish we had a synchronous way to determine this... */
    sleep(1);

    // TODO: determine failure mode if FUSE times out

    //===========================
    // For telechips    
    /*
     * Mount sub-parts before unmounting main-part.
     */
    if (mMultiMount) {
        char mountPoint[255];
        dev_t deviceNodes[MAX_MOUNT_PART];
        int n = getDeviceNodes((dev_t *) &deviceNodes, MAX_MOUNT_PART);
        if (n > MAX_MOUNT_PART) {
            n = MAX_MOUNT_PART;
        }
        for (i = 1; i < n; i++) {
            sprintf(mountPoint, "%s/%s%d", getMountpoint(), getLabel(), i+1);
            if (isMountpointMounted(mountPoint)) {
                if (doUnmount(mountPoint, force) < 0) {
                    SLOGE("Failed to unmount sub-part: %s", mountPoint);
                    setState(Volume::State_Mounted);
                    return -1;
                }
                rmdir(mountPoint);
            }
        }
    }
    //===========================

    if (providesAsec && doUnmount(Volume::SEC_ASECDIR_EXT, force) != 0) {
        SLOGE("Failed to unmount secure area on %s (%s)", getMountpoint(), strerror(errno));
        goto out_mounted;
    }

    /* Now that the fuse daemon is dead, unmount it */
    if (doUnmount(getFuseMountpoint(), force) != 0) {
        SLOGE("Failed to unmount %s (%s)", getFuseMountpoint(), strerror(errno));
        goto fail_remount_secure;
    }

    /* Unmount the real sd card */
    if (doUnmount(getMountpoint(), force) != 0) {
        SLOGE("Failed to unmount %s (%s)", getMountpoint(), strerror(errno));
        goto fail_remount_secure;
    }

    SLOGI("%s unmounted successfully", getMountpoint());

    /* If this is an encrypted volume, and we've been asked to undo
     * the crypto mapping, then revert the dm-crypt mapping, and revert
     * the device info to the original values.
     */
    if (revert && isDecrypted()) {
        cryptfs_revert_volume(getLabel());
        revertDeviceInfo();
        SLOGI("Encrypted volume %s reverted successfully", getMountpoint());
    }

    setUuid(NULL);
    setUserLabel(NULL);
    setState(Volume::State_Idle);
    mCurrentlyMountedKdev = -1;
    return 0;

fail_remount_secure:
    if (providesAsec && mountAsecExternal() != 0) {
        SLOGE("Failed to remount secure area (%s)", strerror(errno));
        goto out_nomedia;
    }

out_mounted:
    setState(Volume::State_Mounted);
    return -1;

out_nomedia:
    setState(Volume::State_NoMedia);
    return -1;
}

int Volume::initializeMbr(const char *deviceNode) {
    struct disk_info dinfo;

    memset(&dinfo, 0, sizeof(dinfo));

    if (!(dinfo.part_lst = (struct part_info *) malloc(MAX_NUM_PARTS * sizeof(struct part_info)))) {
        SLOGE("Failed to malloc prt_lst");
        return -1;
    }

    memset(dinfo.part_lst, 0, MAX_NUM_PARTS * sizeof(struct part_info));
    dinfo.device = strdup(deviceNode);
    dinfo.scheme = PART_SCHEME_MBR;
    dinfo.sect_size = 512;
    dinfo.skip_lba = 2048;
    dinfo.num_lba = 0;
    dinfo.num_parts = 1;

    struct part_info *pinfo = &dinfo.part_lst[0];

    pinfo->name = strdup("android_sdcard");
    pinfo->flags |= PART_ACTIVE_FLAG;
    pinfo->type = PC_PART_TYPE_FAT32;
    pinfo->len_kb = -1;

    int rc = apply_disk_config(&dinfo, 0);

    if (rc) {
        SLOGE("Failed to apply disk configuration (%d)", rc);
        goto out;
    }

 out:
    free(pinfo->name);
    free(dinfo.device);
    free(dinfo.part_lst);

    return rc;
}

/*
 * Use blkid to extract UUID and label from device, since it handles many
 * obscure edge cases around partition types and formats. Always broadcasts
 * updated metadata values.
 */
int Volume::extractMetadata(const char* devicePath) {
    int res = 0;

    std::string cmd;
    cmd = BLKID_PATH;
    cmd += " -c /dev/null ";
    cmd += devicePath;

    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        ALOGE("Failed to run %s: %s", cmd.c_str(), strerror(errno));
        res = -1;
        goto done;
    }

    char line[1024];
    char value[128];
    if (fgets(line, sizeof(line), fp) != NULL) {
        ALOGD("blkid identified as %s", line);

        char* start = strstr(line, "UUID=");
        if (start != NULL && sscanf(start + 5, "\"%127[^\"]\"", value) == 1) {
            setUuid(value);
        } else {
            setUuid(NULL);
        }

        start = strstr(line, "LABEL=");
        if (start != NULL && sscanf(start + 6, "\"%127[^\"]\"", value) == 1) {
            setUserLabel(value);
        } else {
            setUserLabel(NULL);
        }
    } else {
        ALOGW("blkid failed to identify %s", devicePath);
        res = -1;
    }

    pclose(fp);

done:
    if (res == -1) {
        setUuid(NULL);
        setUserLabel(NULL);
    }
    return res;
}
