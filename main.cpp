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
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
//===========================
// For telechips
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
//===========================
#include <fcntl.h>
#include <dirent.h>
#include <fs_mgr.h>

#define LOG_TAG "Vold"

#include "cutils/klog.h"
#include "cutils/log.h"
#include "cutils/properties.h"

#include "VolumeManager.h"
#include "CommandListener.h"
#include "NetlinkManager.h"
#include "DirectVolume.h"
#include "NetworkVolume.h" // For telechips
#include "cryptfs.h"

#include <cutils/properties.h> // For telechips

static int process_config(VolumeManager *vm);
static void coldboot(const char *path);

#define FSTAB_PREFIX "/fstab."
struct fstab *fstab;

//===========================
// For telechips
/* USB DRD default mode */
#define USB_DRD_CMD_VALUE_MAX	50

// host mode
#define USB_DRD_DEF_HOST	"host"

// device mode
#define USB_DRD_DEF_MTP		"mtp"
#define USB_DRD_DEF_MTP_ADB "mtp,adb"

#define USB_DEF_MODE USB_DRD_DEF_HOST
//===========================

int main() {

    VolumeManager *vm;
    CommandListener *cl;
    NetlinkManager *nm;
    //===========================
    // For telechips    
    int ret;
	int host_mode = 0;
	char mode[PROPERTY_VALUE_MAX];
    //char chip[PROPERTY_VALUE_MAX];
	char usb_config_cmd[USB_DRD_CMD_VALUE_MAX];
    //===========================

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    SLOGI("Vold 2.1-tuxera (%s %s) (the revenge) firing up", __DATE__, __TIME__);
    #else
    SLOGI("Vold 2.1 (the revenge) firing up");
    #endif

    mkdir("/dev/block/vold", 0755);

    /* For when cryptfs checks and mounts an encrypted filesystem */
    klog_set_level(6);

    /* Create our singleton managers */
    if (!(vm = VolumeManager::Instance())) {
        SLOGE("Unable to create VolumeManager");
        exit(1);
    };

    if (!(nm = NetlinkManager::Instance())) {
        SLOGE("Unable to create NetlinkManager");
        exit(1);
    };


    cl = new CommandListener();
    vm->setBroadcaster((SocketListener *) cl);
    nm->setBroadcaster((SocketListener *) cl);

    if (vm->start()) {
        SLOGE("Unable to start VolumeManager (%s)", strerror(errno));
        exit(1);
    }

    if (process_config(vm)) {
        SLOGE("Error reading configuration (%s)... continuing anyways", strerror(errno));
    }

    if (nm->start()) {
        SLOGE("Unable to start NetlinkManager (%s)", strerror(errno));
        exit(1);
    }

    coldboot("sys/block/ndda"); // For telechips
    coldboot("/sys/block");
//    coldboot("/sys/class/switch");

    /*
     * Now that we're up, we can respond to commands
     */
    if (cl->startListener()) {
        SLOGE("Unable to start CommandListener (%s)", strerror(errno));
        exit(1);
    }

    //===========================
    // For telechips    
	// Set default usb mode
	// If you want to change the usb drd default mode, please change the value of "USB_DEF_MODE".
	//property_get("ro.hardware", chip, "");
	property_get("persist.sys.usb.defset", mode, "");

	if (strcmp(mode, "done") != 0)
	{
		SLOGI("## Set USB DRD default mode : %s ##",USB_DEF_MODE);

		ret = system("setprop persist.sys.usb.defset done");
	    if (WIFSIGNALED(ret) && (WTERMSIG(ret) == SIGINT || WTERMSIG(ret) == SIGQUIT)) {
	         SLOGE("USB default mode set fail - 0(%d).", ret);
	    }

		sprintf(usb_config_cmd,"setprop persist.sys.usb.config %s",USB_DEF_MODE);
		ret = system(usb_config_cmd);
	    if (WIFSIGNALED(ret) && (WTERMSIG(ret) == SIGINT || WTERMSIG(ret) == SIGQUIT)) {
	         SLOGE("USB default mode set fail - 1(%d).", ret);
	    }
	}
	else
	{
		property_get("persist.sys.usb.config", mode, "");
	    SLOGI("USB Mode is %s", mode);
	}
    //===========================

    // Eventually we'll become the monitoring thread
    while(1) {
        sleep(1000);
    }

    SLOGI("Vold exiting");
    exit(0);
}

static void do_coldboot(DIR *d, int lvl)
{
    struct dirent *de;
    int dfd, fd;

    dfd = dirfd(d);

    fd = openat(dfd, "uevent", O_WRONLY);
    if(fd >= 0) {
        write(fd, "add\n", 4);
        close(fd);
    }

    while((de = readdir(d))) {
        DIR *d2;

        if (de->d_name[0] == '.' || !strcmp(de->d_name, "ndda")) // For telechips
            continue;

        if (de->d_type != DT_DIR && lvl > 0)
            continue;

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY);
        if(fd < 0)
            continue;

        d2 = fdopendir(fd);
        if(d2 == 0)
            close(fd);
        else {
            do_coldboot(d2, lvl + 1);
            closedir(d2);
        }
    }
}

static void coldboot(const char *path)
{
    DIR *d = opendir(path);
    if(d) {
        do_coldboot(d, 0);
        closedir(d);
    }
}

static int process_config(VolumeManager *vm)
{
    char fstab_filename[PROPERTY_VALUE_MAX + sizeof(FSTAB_PREFIX)];
    char propbuf[PROPERTY_VALUE_MAX];
    int i;
    int j; // For telechips
    int ret = -1;
    int flags;

    property_get("ro.hardware", propbuf, "");
    snprintf(fstab_filename, sizeof(fstab_filename), FSTAB_PREFIX"%s", propbuf);

    //===========================
    // For telechips    
    property_get("ro.bootmode", propbuf, "");
    if (!strcmp(propbuf, "emmc"))
        sprintf(fstab_filename, "%s.%s", fstab_filename, propbuf);
    //===========================
    
    fstab = fs_mgr_read_fstab(fstab_filename);
    if (!fstab) {
        SLOGE("failed to open %s\n", fstab_filename);
        return -1;
    }

    /* Loop through entries looking for ones that vold manages */
    for (i = 0; i < fstab->num_entries; i++) {
        if (fs_mgr_is_voldmanaged(&fstab->recs[i])) {
            DirectVolume *dv = NULL;
            flags = 0;

            //===========================
            // For telechips    
            if (!strcmp(fstab->recs[i].fs_type, "nfs")) {
               NetworkVolume *dv = NULL;

               dv = new NetworkVolume(vm, &(fstab->recs[i]), flags);

               vm->addVolume(dv);

               continue;
            }
            /* Only use this flag in the box of telechips */
            if (fs_mgr_is_nofuse(&fstab->recs[i])) {
                flags |= VOL_NOFUSE;
            }
            //===========================
            
            /* Set any flags that might be set for this volume */
            if (fs_mgr_is_nonremovable(&fstab->recs[i])) {
                flags |= VOL_NONREMOVABLE;
            }
            if (fs_mgr_is_encryptable(&fstab->recs[i])) {
                flags |= VOL_ENCRYPTABLE;
            }
            /* Only set this flag if there is not an emulated sd card */
            if (fs_mgr_is_noemulatedsd(&fstab->recs[i]) &&
                !strcmp(fstab->recs[i].fs_type, "vfat")) {
                flags |= VOL_PROVIDES_ASEC;
            }
            dv = new DirectVolume(vm, &(fstab->recs[i]), flags);

            if (dv->addPath(fstab->recs[i].blk_device)) {
                SLOGE("Failed to add devpath %s to volume %s",
                      fstab->recs[i].blk_device, fstab->recs[i].label);
                goto out_fail;
            }

            //===========================
            // For telechips    
            for (j=0; j<BLK_DEVICE2_NUM; j++) {
                if (strcmp(fstab->recs[i].blk_device2[j], NO_BLK_DEVICE2)) {
                    if (dv->addPath(fstab->recs[i].blk_device2[j])) {
                        SLOGE("Failed to add devpath %s", fstab->recs[i].blk_device2[j]);
                        goto out_fail;
                    }
                }
            }
            //===========================

            vm->addVolume(dv);
        }
    }

    ret = 0;

out_fail:
    return ret;
}
