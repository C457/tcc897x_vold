#ifndef _FUSE_FS_H
#define _FUSE_FS_H

#include <unistd.h>

class FuseFS {
public:
    static int doMount(const char *fsPath, const char *mountPoint,
                       bool ro, bool remount, bool executable,
                       int ownerUid, int ownerGid, int permMask,
                       bool createLost);
private:
    static bool isMounted(const char *path);
};

#endif
