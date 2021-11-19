#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>

#define LOG_TAG "Vold"

#include <cutils/log.h>
#include <cutils/properties.h>

#include "fusefs.h"

#define CDROM_NAMECONV "cdrom_nameconv"
#define CDROM_MNTPNT "/storage/cdrom_actual"
#define PROP_STOPPED "stopped"
#define FUSE_MAX_RETRY_TIMEOUT 30

bool FuseFS::isMounted(const char *path) {
    char device[256];
    char mount_path[256];
    char rest[256];
    FILE *fp;
    char line[1024];
    bool mounted = false;

    if (!(fp = fopen("/proc/mounts", "r"))) {
        SLOGE("Error opening /proc/mounts (%s)", strerror(errno));
        return mounted;
    }

    while(fgets(line, sizeof(line), fp) != NULL) {
        sscanf(line, "%255s %255s %255s\n", device, mount_path, rest);
        if (!strcmp(mount_path, path)) {
            mounted = true;
            break;
        }
    }

    fclose(fp);
    return mounted;
}

int FuseFS::doMount(const char *fsPath, const char *mountPoint,
                 bool ro, bool remount, bool executable,
                 int ownerUid, int ownerGid, int permMask, bool createLost) {
    int retry = 0;
    int rc = -ENODEV;
    char prop[PROPERTY_VALUE_MAX];
    struct timeval start, end;

    SLOGD("trying to mount fuse service for %s", fsPath);
    gettimeofday(&start, NULL);

    /* restart cdrom_nameconv service */
    property_set("ctl.restart", CDROM_NAMECONV);

    do {
        if (isMounted(mountPoint)) {
            rc = 0;
            break;
        }
        usleep(100 * 1000);

        property_get("init.svc." CDROM_NAMECONV, prop, PROP_STOPPED);
        if (strcmp(prop, PROP_STOPPED) == 0) {
            SLOGE("%s service is terminated.", CDROM_NAMECONV);
            break;
        }
        retry++;

        gettimeofday(&end, NULL);
        timersub(&end, &start, &end);
    } while (end.tv_sec < FUSE_MAX_RETRY_TIMEOUT);

    SLOGD("fuse retry count is %d, duration = %ld.%06ld sec", retry, end.tv_sec, end.tv_usec);

    if (rc != 0) {
        SLOGE("fuse mount failed. stopping fuse service");
        property_set("ctl.stop", CDROM_NAMECONV);

        /* clean up mount points */
        umount2(CDROM_MNTPNT, MNT_DETACH);
        umount2(mountPoint, MNT_DETACH);

        /* set errno */
        errno = -rc;
    }

    return rc;
}
