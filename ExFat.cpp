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
#include <sys/wait.h>

#include <linux/fs.h>
#include <linux/kdev_t.h>

#define LOG_TAG "Vold"

#include <cutils/log.h>
#include <cutils/properties.h>

#include "ExFat.h"

static char MKEXFAT_PATH[] = "/system/bin/mkexfat";
static char EXFATCK_PATH[] = "/system/bin/exfatck";
static char EXFATINFO_PATH[] = "/sbin/exfatinfo";
extern "C" int logwrap(int argc, const char **argv, int background);
extern "C" int mount(const char *, const char *, const char *, unsigned long, const void *);

#if !defined(min)
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif /* !defined(min) */

static int runExfatck(const char *const fsPath)
{
    bool rw = true;
    if (access(EXFATCK_PATH, X_OK)) {
        SLOGW("Skipping fs checks\n");
        return 0;
    }

    int pass = 1;
    int rc = 0;
    do {
        const char *args[4];
        args[0] = EXFATCK_PATH;
        args[1] = "-r";
        args[2] = fsPath;
        args[3] = NULL;

        rc = logwrap(3, args, 1);

        switch(rc) {
        case 0:
            SLOGI("exFAT check/repair completed OK");
            return 0;

        case 1:
            SLOGI("exFAT check/repair found errors that could not be repaired");
            errno = EIO;
            return -1;

        default:
            SLOGE("exFAT check exited with error: %d", rc);
            errno = EIO;
            return -1;
        }
    } while (0);

    return 0;
}

int ExFat::detect(const char *fsPath, bool *outResult) {
    int retval = -1;

    int fd = open(fsPath, O_RDONLY);
    if (fd != -1) {
        loff_t seek_res = lseek64(fd, 0, SEEK_SET);
        if (seek_res == 0) {
            char boot_sector[512];
            ssize_t read_res = read(fd, boot_sector, 512);
            if (read_res == 512) {
                if (!memcmp(&boot_sector[3], "EXFAT   ", 8)) {
                    SLOGI("exFAT filesystem detected.");
                    *outResult = true;
                }
                else if (!memcmp(&boot_sector[0], "RRaAXFAT   ", 11)) {
                    SLOGI("Corrupted exFAT filesystem detected. Fixing.");
                    *outResult = true;
                }
                else {
                    SLOGV("Filesystem detection failed (not an exFAT "
                          "filesystem).");
                    *outResult = false;
                }

                retval = 0;
            }
            else if (read_res != -1)
                errno = EIO;
        }
        else if (seek_res != -1)
            errno = EIO;

        close(fd);
    }

    return retval;
}

int ExFat::check(const char *fsPath) {
    if(runExfatck(fsPath) != 0) {
        /* Ignore return/errno value. We know it's exFAT, so any file system
         * check/repair results should be considered a bonus. The driver should
         * be able to mount the volume in any case and deal with inconsistencies
         * appropriately. */
        errno = 0;
    }

    return 0;
}

int ExFat::doMount(const char *fsPath, const char *mountPoint,
                 bool ro, bool remount, bool executable, int ownerUid,
                 int ownerGid, int permMask) {
    int rc;
    unsigned long flags;
    char mountData[255];

    flags = MS_NODEV | MS_NOSUID | MS_DIRSYNC;

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

    sprintf(mountData,
            "utf8,uid=%d,gid=%d,fmask=%o,dmask=%o",
            ownerUid, ownerGid, permMask, permMask);

    rc = mount(fsPath, mountPoint, "texfat", flags, mountData);

    if (rc && errno == EROFS) {
        SLOGE("%s appears to be a read only filesystem - retrying mount RO", fsPath);
        flags |= MS_RDONLY;
        rc = mount(fsPath, mountPoint, "texfat", flags, mountData);
    }

    return rc;
}

static int erasePartitionTable(const char *const devicePath)
{
    int err = 0;
    int fd = open(devicePath, O_RDWR);
    if (fd == -1)
        err = errno ? errno : EIO;
    else {
        char sector[512];
        memset(sector, 0, 512);

        if (lseek(fd, 0, SEEK_SET) == -1 ||
            write(fd, sector, 512) == -1 ||
            fsync(fd) == -1)
        {
            err = errno ? errno : EIO;
        }

        close(fd);
    }

    return err;
}

static int reloadPartitions(const char *const devicePath)
{
    int err = 0;
    int fd = open(devicePath, O_RDWR);
    if (fd == -1)
        err = errno ? errno : EIO;
    else {
        if (ioctl(fd, BLKRRPART, NULL) == -1)
            err = errno ? errno : EIO;

        close(fd);
    }

    return err;
}

int ExFat::format(const char *fsPath, unsigned int numSectors,
                  bool wholeDevice) {
    const char *args[6];
    int rc;

    SLOGI("Formatting SDXC exFAT file system at \"%s\" as %s...", fsPath,
          wholeDevice ? "whole device" : "single partition");

    if (wholeDevice) {
        int err;
        if ((err = erasePartitionTable(fsPath)) != 0) {
            SLOGE("Failed to erase existing partition table: %d (%s)", errno,
                  strerror(errno));
            errno = EIO;
            return -1;
        }
        else if ((err = reloadPartitions(fsPath)) != 0) {
            SLOGE("Failed to refresh partitions in kernel: %d (%s)", errno,
                  strerror(errno));
            errno = EIO;
            return -1;
        }
    }

    args[0] = MKEXFAT_PATH;
    if (wholeDevice)
      args[1] = "--sda-whole";
    else
      args[1] = "--sda-strict";
    if (numSectors) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%u", numSectors);
        args[2] = "--sector-count";
        args[3] = (const char*) tmp;
        args[4] = fsPath;
        args[5] = NULL;
        rc = logwrap(6, args, 1);
    }
    else {
        args[2] = fsPath;
        args[3] = NULL;
        rc = logwrap(4, args, 1);
    }

    if (rc == 0) {
        SLOGI("Filesystem formatted OK");
        if (wholeDevice) {
            int err = reloadPartitions(fsPath);
            if (err) {
                SLOGE("Failed to reload partition table: %d (%s)", err,
                      strerror(err));
                errno = EIO;
                return -1;
            }
            else
                return 0;
        }
        else
            return 0;
    } else {
        SLOGE("Format failed (unknown exit code %d)", rc);
        errno = EIO;
        return -1;
    }
    return 0;
}

static int getDeviceSize(const char *const devicePath, uint64_t *const outSize)
{
    int err = 0;
    int fd = open(devicePath, O_RDONLY);
    if (fd == -1)
        err = errno ? errno : EIO;
    else {
        uint64_t size;
        if (ioctl(fd, BLKGETSIZE64, &size) == -1)
            err = errno ? errno : EIO;
        else
            *outSize = size;

        close(fd);
    }

    return err;
}

int ExFat::checkSize(const char *const wholeDevicePath) {
    int res;
    uint64_t size = 0;
    int err = getDeviceSize(wholeDevicePath, &size);
    if (!err) {
        if (size < 34359738368ULL || size > 2199023255552ULL)
            res = 0;
        else
            res = 1;
    }
    else {
        res = -1;
        errno = err;
    }

    return res;
}

static void
__replace(char *src, char s, char r) {
    int len = strlen(src);
    int i;
    for (i=0; i < len; i++) {
        if (src[i] == s) {
            src[i] = r;
        }
    }
}

int ExFat::getInfo(const char *const fsPath, char *const label,
                   const size_t labelSize, int32_t *const id,
                   uint64_t *const fsSize)
{
    int err = 0;
    bool needLabel = label != NULL;
    bool needId = id != NULL;
    bool needFsSize = fsSize != NULL;

    size_t cmd_size = 0;
    char *cmd = NULL;
    FILE *fp = NULL;

    cmd_size = 1 + strlen(EXFATINFO_PATH) + 3 + strlen(fsPath) + 2;
    cmd = (char*) malloc(cmd_size);
    if(!cmd) {
        err = errno ? errno : ENOMEM;
        SLOGE("Error while allocating %zu bytes for 'cmd' buffer: %d (%s)",
              cmd_size, errno, strerror(errno));
	goto out;
    }

    snprintf(cmd, cmd_size, "\"%s\" \"%s\"", EXFATINFO_PATH, fsPath);

    fp = popen(cmd, "r");
    if (!fp) {
        err = errno ? errno : EIO;
        SLOGE("Error while invoking command '%s': %s (%d)",
              cmd, strerror(errno), errno);
        goto out;
    }

    char line[2048];
    while (fgets(line, sizeof(line), fp) != NULL &&
           (needLabel || needId || needFsSize))
    {
        static const char labelPrefix[] = "\tVolume name: \"";
        static const char serialPrefix[] = "\tVolume serial number: ";
        static const char volumeLengthPrefix[] = "\tVolume length: ";

        const size_t lineLength = strlen(line) - 1;

        if (needLabel && !strncmp(line, labelPrefix, sizeof(labelPrefix) - 1))
        {
            if (line[lineLength - 1] != '\"') {
                SLOGE("Unexpected! Non-terminated volume name string: %.*s",
                      (int) lineLength, line);
                err = EINVAL;
                goto out;
            }

            if (labelSize < (lineLength - 1) - (sizeof(labelPrefix) - 1) + 1)
            {
                SLOGE("Supplied label buffer is too small (%zu < %zu).",
                      labelSize,
                      (lineLength - 1) - (sizeof(labelPrefix) - 1) + 1);
                err = ERANGE;
                goto out;
            }

            /* Copy all except terminating '"' to label. */
            strncpy(label, &line[sizeof(labelPrefix) - 1],
                    min((lineLength - 1) - (sizeof(labelPrefix) - 1),
                    labelSize - 1));
            
            if((lineLength - 1) - (sizeof(labelPrefix) - 1) < labelSize -1) // trim garbage string with chaging position of null
                label[(lineLength - 1) - (sizeof(labelPrefix) - 1)] = '\0';
            else
                label[labelSize - 1] = '\0';

            __replace(label, ' ', '_'); // replace space to '_' like vfat
            needLabel = false;
        }
        else if (needId && !strncmp(line, serialPrefix,
            sizeof(serialPrefix) - 1))
        {
            int serialHead = 0;
            int serialTail = 0;

            if (lineLength - (sizeof(serialPrefix) - 1) != 9) {
                SLOGE("Unexpected serial number length: %zu (line: \"%.*s\")",
                      lineLength - (sizeof(serialPrefix) - 1), (int) lineLength,
                      line);
                err = EINVAL;
                goto out;
            }

            if (sscanf(&line[sizeof(serialPrefix) - 1], "%04X-%04X",
                &serialHead, &serialTail) != 2)
            {
                SLOGE("Malformed serial number: %.*s",
                      (int) (lineLength - (sizeof(serialPrefix) - 1)),
                      &line[sizeof(serialPrefix) - 1]);
                err = EINVAL;
                goto out;
            }

            *id = (serialHead << 16) | serialTail;
            needId = false;
        }
        else if (needFsSize && !strncmp(line, volumeLengthPrefix,
            sizeof(volumeLengthPrefix) - 1))
        {
            unsigned long long value = 0;
            if (sscanf(&line[sizeof(volumeLengthPrefix) - 1], "%*u sectors "
                "(%llu bytes)", &value) != 1)
            {
                SLOGE("Malformed volume length string: %.*s",
                      (int) (lineLength - (sizeof(volumeLengthPrefix) - 1)),
                      &line[sizeof(volumeLengthPrefix) - 1]);
                err = EINVAL;
                goto out;
            }

            *fsSize = value;
            needFsSize = false;
        }
    }

    if (needLabel || needId || needFsSize) {
        SLOGE("Could not parse the following information from exfatinfo "
              "output:%s%s%s",
              needLabel ? " 'volume name'" : "",
              needId ? " 'volume serial number'" : "",
              needFsSize ? " 'volume length'" : "");
        err = EIO;
        goto out;
    }
out:
    int retval;
    if (fp != NULL && (retval = pclose(fp)) != 0) {
        if (retval == -1) {
            err = err ? err : (errno ? errno : EIO);
            SLOGE("Error while closing popen stream: %d (%s)", errno,
                  strerror(errno));
        }
        else if (WIFEXITED(retval)) {
            SLOGE("exfatinfo utility returned error code %d.",
                  WEXITSTATUS(retval));
            err = err ? err : EIO;
        }
        else if (WIFSIGNALED(retval)) {
            SLOGE("exfatinfo utility exited abnormally due to signal %d.",
                  WTERMSIG(retval));
            err = err ? err : EIO;
        }
        else {
            SLOGE("exfatinfo utility exited abnormally for an unknown reason "
                  "(status: 0x%08X).", retval);
            err = err ? err : EIO;
        }
    }

    if (cmd) {
        free(cmd);
    }

    return err;
}

int getExFatInfo(const char *fsPath, char *label, size_t labelSize,
                 int32_t *id, uint64_t *fsSize)
{
    return ExFat::getInfo(fsPath, label, labelSize, id, fsSize);
}
