#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <linux/kdev_t.h>
#include <linux/fs.h>

#if ANDROID
#define LOG_TAG "Vold"
//#define LOG_NDEBUG 0
#include <cutils/log.h>
#else
#define SLOGE(fmt, ...) fprintf(stderr, fmt"\n", ##__VA_ARGS__)
#define SLOGI(fmt, ...) fprintf(stderr, fmt"\n", ##__VA_ARGS__)
#define SLOGW(fmt, ...) fprintf(stderr, fmt"\n", ##__VA_ARGS__)
#define SLOGD(fmt, ...) fprintf(stderr, fmt"\n", ##__VA_ARGS__)
#include <stdint.h>
#define PAGE_SHIFT      12
#endif

#include <linux/iso_fs.h>
#include "utils.h"
#ifdef FUNCTION_STORAGE_TUXERA_PATCH    
#include "ExFat.h"
#endif

#define FAT32_ROOT_ENTRY_COUNT 0
#define FAT16_ROOT_ENTRY_COUNT 512
#define FAT_LABEL_SIZE 11

#define BYTES_PER_SECTOR_POS 0x0B
#define TOTAL_SECTOR16_POS 0x13
#define TOTAL_SECTOR32_POS 0x20

typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned char u8;

#define READ_AT(var, offset) pread64(fd, &var, sizeof(var), offset)
#define CLUSTER_START(cluster) (data_start + ((cluster - 2) * cluster_size))

#define PAGE_SIZE        (1UL << PAGE_SHIFT)
#define PAGE_MASK        (~(PAGE_SIZE-1))
#define PAGE_ALIGN(_x) (((_x)+PAGE_SIZE-1)&PAGE_MASK)
#define PAGE(_x)     (_x & PAGE_MASK)

static char *
readRootEntryLabel(const char *dev) {
    u16 fat16_length;
    u32 fat32_length, root_cluster;
    u16 dir_entries;
    u16 sector_size;
    u8 fats, sector_per_cluster;
    u32 cluster_size;
    u16 reserved;
    int fd;
    long long root_start, data_start;
    u32 i;
    u8 *first_fat, *second_fat;
    u32 clusters;
    u16 sectors16;
    u32 sectors;

    fd = open(dev, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    READ_AT(sector_size, 11);
    READ_AT(sector_per_cluster, 13);
    READ_AT(reserved, 14);
    READ_AT(fats, 16);
    READ_AT(dir_entries, 17);
    READ_AT(sectors16, 19);
    if (sectors16 == 0) {
        READ_AT(sectors, 32);
    } else {
        sectors = sectors16;
    }

    READ_AT(fat16_length, 22);
    READ_AT(fat32_length, 36);

    SLOGV("fat16_len = %d fat32_len = %d", fat16_length, fat32_length);
    SLOGV("sector_size = %d sectors = %d", sector_size, sectors);
    SLOGV("reserved = %d", reserved);

#define check_size(size) ((size) == 0 || (((size) & ((size) -1)) != 0))
    if (check_size(sector_size)) {
        SLOGW("no valid sector size: %d", sector_size);
        close(fd);
        return NULL;
    }

    int fat_length = fat16_length ? fat16_length : fat32_length;
    cluster_size = sector_size * sector_per_cluster;
    root_start = (reserved + fats * fat_length) * sector_size;
    data_start = root_start + ((dir_entries << 5) + sector_size - 1)/sector_size * sector_size;
    long long data_size = sectors * (long long)sector_size - data_start;
    clusters = data_size / cluster_size;
    u32 num_cluster = clusters + 2;
    int fat_start = reserved * sector_size;
    int fat_size = num_cluster * 4;

    SLOGV("cluster_size = %d", cluster_size);
    SLOGV("data_start = %lld data_size = %lld", data_start, data_size);
    SLOGV("root_start = %lld", root_start);
    SLOGV("first_fat_start = %d fat_size = %d", fat_start, fat_size);
    SLOGI("aligned first_fat_start = %d fat_size = %d", PAGE(fat_start), PAGE_ALIGN(fat_size));

    if (fat16_length == 0 && fat32_length != 0) {
        // fat32
        if (READ_AT(root_cluster, 36 + 8) < 0) goto read_fail;
        if (root_cluster) {
            u32 cluster;
            SLOGD("root_cluster = %d, num_cluster=%d", root_cluster, num_cluster);
            for (cluster=root_cluster; cluster != 0 && cluster < num_cluster; ) {
                long long offset = CLUSTER_START(cluster);
                SLOGV("cluster = %d %lld", cluster, offset);
                for (i=0; i*32<cluster_size; i++, offset += 32) {
                    u8 attr;
                    if (READ_AT(attr, offset + 11) < 0) goto read_fail;
                    SLOGV("attr = %02X", attr);
                    if (attr == 0x08) {
                        // volume label
                        u8 name[8];
                        u8 ext[3], label[16];
                        if (READ_AT(ext, offset + 8) < 0) goto read_fail;
                        if (READ_AT(name, offset) < 0) goto read_fail;
                        if (name[0] == 0xE5 || name[0] == 0) continue;
                        sprintf((char *)label, "%.8s%.3s", name, ext);
                        close(fd);
                        return strdup((char *)label);
                    }
                }
                offset = fat_start + cluster * sizeof(u32);
                if (READ_AT(cluster, offset) < 0) goto read_fail;
                cluster = cluster & 0xFFFFFF;
                SLOGV("next cluster=%u", cluster);
            }
        }
    }
    close(fd);
    return NULL;
read_fail:
    SLOGW("fd read fail. path=%s", dev);
    close(fd);
    return NULL;
}

static int32_t getFatId(const char *fsPath) {
    FILE *fp = fopen(fsPath, "r");
    if(fp == NULL) {
        SLOGE("getFatId: cannot open device: %s %s", fsPath, strerror(errno));
        return -1;
    }
    uint16_t fat_type;
    if (fseek(fp, 0x11, SEEK_SET) < 0) goto seek_fail;
    if (fread(&fat_type, 1, 2, fp) != 2) goto read_fail;
    SLOGD("fat_type=%u", fat_type);
    int32_t id;
    if (fat_type == FAT16_ROOT_ENTRY_COUNT) {
        if (fseek(fp, 0x27, SEEK_SET) < 0) goto seek_fail;
        if (fread(&id, 1, 4, fp) != 4) goto read_fail;
    } else if (fat_type == FAT32_ROOT_ENTRY_COUNT) {
        if (fseek(fp, 0x43, SEEK_SET) < 0) goto seek_fail;
        if (fread(&id, 1, 4, fp) != 4) goto read_fail;
    } else {
        SLOGW("%s::Not surpported filesystem=%s", __func__, fsPath);
        id = -1;
    }
    SLOGD("getFatId() id=%d", id);
    fclose(fp);
    return id;
seek_fail:
    SLOGE("fseek fail. path=%s", fsPath);
    fclose(fp);
    return -1;
read_fail:
    SLOGE("fread fail. path=%s", fsPath);
    fclose(fp);
    return -1;
}

static void
__rtrim(char *src) {
    char *out = src;
    int len = strlen(out);
    int i;
    for (i=len-1; i>=0; i--) {
        if (out[i] != ' ') break;
    }
    out[i+1] = '\0';
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

static void getFatLabel(const char *fsPath, char *label) {
    FILE *fp = fopen(fsPath, "r");
    if (fp == NULL) {
        SLOGE("getFatLabel: cannot open device: %s %s", fsPath, strerror(errno));
        label[0] = '\0';
        return;
    }
    char *rootEntryLabel = readRootEntryLabel(fsPath);
    SLOGI("rootEntryLabel[%s]", rootEntryLabel);
    if (rootEntryLabel) {
        strncpy(label, rootEntryLabel, FAT_LABEL_SIZE);
        free(rootEntryLabel);
    } else {
        uint16_t fat_type;
        if (fseek(fp, 0x11, SEEK_SET) < 0) goto seek_fail;
        if (fread(&fat_type, 1, 2, fp) != 2) goto read_fail;
        SLOGI("fat_type=%d", fat_type);
        if (fat_type == FAT16_ROOT_ENTRY_COUNT) {
            if (fseek(fp, 0x2b, SEEK_SET) < 0) goto seek_fail;
            if (fread(label, 1, FAT_LABEL_SIZE, fp) != FAT_LABEL_SIZE) goto read_fail;
        } else if (fat_type == FAT32_ROOT_ENTRY_COUNT) {
            if (fseek(fp, 0x47, SEEK_SET) < 0) goto seek_fail;
            if (fread(label, 1, FAT_LABEL_SIZE, fp) != FAT_LABEL_SIZE) goto read_fail;
        } else {
            SLOGW("%s::Not surpported filesystem=%s", __func__, fsPath);
        }
    }
    label[FAT_LABEL_SIZE] = 0;
    __rtrim(label);
    __replace(label, ' ', '_');
    fclose(fp);
    // FIXME
    SLOGI("label=[%s]", label);
    return;
seek_fail:
    SLOGE("fseek fail. path=%s", fsPath);
    fclose(fp);
    label[0] = '\0';
    return;
read_fail:
    SLOGE("fread fail. path=%s", fsPath);
    fclose(fp);
    label[0] = '\0';
    return;
}

static uint64_t getFatSize(const char *fsPath) {
    FILE *fp = fopen(fsPath, "r");
    if (fp == NULL) {
        SLOGE("getFatSize: cannot open device: %s %s", fsPath, strerror(errno));
        return 0;
    }
    uint16_t fat_type;
    uint64_t bytes;
    if (fseek(fp, 0x11, SEEK_SET) < 0) goto seek_fail;
    if (fread(&fat_type, 1, 2, fp) != 2) goto read_fail;

    if (fat_type == FAT16_ROOT_ENTRY_COUNT ||
            fat_type == FAT32_ROOT_ENTRY_COUNT) {
        uint16_t sectors16;
        if (fseek(fp, TOTAL_SECTOR16_POS, SEEK_SET) < 0) goto seek_fail;
        if (fread(&sectors16, 1, 2, fp) != 2) goto read_fail;
        SLOGW("%s::total_sector16=%u", fsPath, sectors16);

        uint32_t sectors;
        if (sectors16 != 0) {
            sectors = (uint32_t)sectors16;
        } else {
            uint32_t sectors32;
            if (fseek(fp, TOTAL_SECTOR32_POS, SEEK_SET) < 0) goto seek_fail;
            if (fread(&sectors32, 1, 4, fp) != 4) goto read_fail;
            SLOGW("%s::total_sector32=%u", fsPath, sectors32);
            sectors = sectors32;
        }

        uint16_t bytes_per_sector;
        if (fseek(fp, BYTES_PER_SECTOR_POS, SEEK_SET) < 0) goto seek_fail;
        if (fread(&bytes_per_sector, 1, 2, fp) != 2) goto read_fail;
        SLOGW("%s::bytes_per_sector=%d", fsPath, bytes_per_sector);

        bytes = (uint64_t)sectors * bytes_per_sector;
        SLOGW("%s::volume size bytes=%lld", fsPath, bytes);
    } else {
        bytes = 0;
        SLOGW("fat_type=%d", fat_type);
        SLOGW("%s::Not surpported filesystem=%s", __func__, fsPath);
    }
    fclose(fp);
    return bytes;
seek_fail:
    SLOGE("fseek fail. path=%s", fsPath);
    fclose(fp);
    return 0;
read_fail:
    SLOGE("fread fail. path=%s", fsPath);
    fclose(fp);
    return 0;
}

static int getFatInfo(const char *fsPath, char *label, int32_t *id) {
    uint64_t size = getFatSize(fsPath);
    if (size == 0) {
        *id = -1;
        label[0] = '\0';
        return -1;
    }

    *id = getFatId(fsPath);
    if (*id == -1) {
        label[0] = '\0';
        return -1;
    }

    getFatLabel(fsPath, label);
    return 0;
}

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_SUPPORT_CDROM
static int getIsoDescriptor(const char *devPath, struct iso_primary_descriptor *ipd) {
    FILE *fp = fopen(devPath, "r");
    if(fp == NULL) {
        SLOGE("cannot open device: %s %s", devPath, strerror(errno));
        return -1;
    }

    off_t offset = ((off_t)(16)) <<11;

    if (fseek(fp, offset, SEEK_SET) == 0) {
        if (fread(ipd, sizeof(struct iso_primary_descriptor), 1, fp) == 1) {
            if ((ipd->type[0] != ISO_VD_PRIMARY) ||
                    (strncmp(ipd->id, ISO_STANDARD_ID, sizeof(ipd->id)) != 0) ||
                    (ipd->version[0] != 1)) {
                SLOGE("this device(%s) has no iso9660 filesystem", devPath);
                goto fail;
            }
            // success
        } else {
            SLOGW("fread failed: %s", strerror(errno));
            goto fail;
        }
    } else {
        SLOGW("fseek failed: %s", strerror(errno));
        goto fail;
    }
    fclose(fp);
    return 0;
fail:
    fclose(fp);
    return -1;
}

int getIsoInfo(const char *fsPath, char *label, int32_t *id) {
    if (!isCdromAvailable()) {
        SLOGI("cdrom drive is not prepared from micom");
        *id = -1;
        label[0] = '\0';
        return -1;
    }
    struct iso_primary_descriptor ipd;
    if (getIsoDescriptor(fsPath, &ipd) != 0) {
        *id = -1;
        label[0] = '\0';
        return -1;
    }

    uint8_t *data = (uint8_t *)(&ipd);
    ssize_t count = sizeof(ipd);

    union {
        uint8_t bytes[4];
        uint32_t id;
    } Checksum;

    Checksum.id = 0;
    while (count --) {
        Checksum.bytes[count & 0x03] += (*data++);
    }

    *id = Checksum.id;

    // FIXME
    label[0] = '\0';

    return 0;
}
#endif
//-NATIVE_PLATFORM

#define STORAGE_PREFIX "/storage/"
#define UDISK_PREFIX STORAGE_PREFIX "usb"
#define SDCARD_PREFIX STORAGE_PREFIX "sdcard"
//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_SUPPORT_CDROM
#define CDROM_PATH STORAGE_PREFIX "cdrom"
#endif
//-NATIVE_PLATFORM
#define MYMUSIC_PATH STORAGE_PREFIX "mymusic"
#define VR_PATH STORAGE_PREFIX "vr"

// this directory is used by mediascanner whether media is ejected
#define MOUNTED_DIR "/dev/block/mounted"

void getDevType(const char *mntPnt, char *devType) {
    if (!strncmp(mntPnt, UDISK_PREFIX, strlen(UDISK_PREFIX))) {
        strcpy(devType, "udisk");
    } else if (!strcmp(mntPnt, MYMUSIC_PATH)) {
        strcpy(devType, "mymusic");
    } else 
	//+NATIVE_PLATFORM
	#ifdef FUNCTION_STORAGE_SUPPORT_CDROM
    if (!strcmp(mntPnt, CDROM_PATH)) {
        strcpy(devType, "cdrom");
    } else 
	#endif
	//-NATIVE_PLATFORM
    if (!strncmp(mntPnt, SDCARD_PREFIX, strlen(SDCARD_PREFIX))) {
        strcpy(devType, "sdcard");
    } else if (!strcmp(mntPnt, VR_PATH)) {
        strcpy(devType, "vr");
    } else {
        strcpy(devType, "external");
    }
}

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_SUPPORT_CDROM
int isCdromPoint(const char *mntPnt) {
    return strcmp(mntPnt, CDROM_PATH) == 0;
}

int isCdromAvailable() {
    return access("/dev/block/vold/cdrom_inserted", F_OK) == 0;
}

int isUsbMountPoint(const char *mntPnt) {
    return strncmp(mntPnt, UDISK_PREFIX, strlen(UDISK_PREFIX)) == 0;
}
#endif
//-NATIVE_PLATFORM

int isReadOnlyMedia(const char *mntPnt, const int isWritableUsb) {
    if (!strncmp(mntPnt, UDISK_PREFIX, strlen(UDISK_PREFIX))) {
        if(isWritableUsb > 0) {
            return 0; //read write
        } else{
            return 1; //read only
        }
    } else 
	//+NATIVE_PLATFORM
	#ifdef FUNCTION_STORAGE_SUPPORT_CDROM
    if (!strcmp(mntPnt, CDROM_PATH)) {
        return 1;
    }
	#endif
    if (!strncmp(mntPnt, SDCARD_PREFIX, strlen(SDCARD_PREFIX))) {
        return 1;
    } else if (!strncmp(mntPnt, STORAGE_PREFIX, strlen(STORAGE_PREFIX))) {
        return 1;
    }
     
    
    
    //-NATIVE_PLATFORM
    

    return 0;
}



bool isReadable(const char *path) {
    int fd;
    int bytes;
    uint8_t buf[512];

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        SLOGE("open failed: %s", strerror(errno));
        return false;
    }
    bytes = read(fd, buf, 512);
    if (bytes < 0) {
        SLOGE("read failed: %s", strerror(errno));
    } else if (bytes != 512) {
        SLOGE("read insufficient data: %d bytes", bytes);
    }
    close(fd);

    return (bytes == 512);
}

//+NATIVE_PLATFORM
#ifdef FUNCTION_STORAGE_TUXERA_PATCH    
int getVolumeInfo(const char *mntPnt, const char *devPath, char *label, size_t labelSize, int32_t *id) {
#else
int getVolumeInfo(const char *mntPnt, const char *devPath, char *label, int32_t *id) {
#endif                  
//-NATIVE_PLATFORM
	//+NATIVE_PLATFORM
	#ifdef FUNCTION_STORAGE_SUPPORT_CDROM
    if (isCdromPoint(mntPnt)) {
        return getIsoInfo(devPath, label, id);
    }
	#endif                  
	//-NATIVE_PLATFORM
    int rc = getFatInfo(devPath, label, id);

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    if (rc == -1) {
        int err = getExFatInfo(devPath, label, labelSize, id, NULL);
        if (err) {
            errno = err;
            rc = -1;
        }
        else {
            rc = 0;
        }
    }
    #endif                  
    //-NATIVE_PLATFORM

    if (rc == -1) {
        int64_t ntfs_id;
        rc = getNtfsInfo(devPath, label, &ntfs_id);
        if (rc != -1) {
            *id = ((ntfs_id >> 32) & 0xffffffff);
        }
    }
    return rc;
}

uint64_t getVolumeSize(const char *mntPnt, const char *devPath) {
	//+NATIVE_PLATFORM
	#ifdef FUNCTION_STORAGE_SUPPORT_CDROM
    if (isCdromPoint(mntPnt)) {
        // FIXME
        return 1;
    }
	#endif					
    //-NATIVE_PLATFORM

    uint64_t size = getFatSize(devPath);

    //+NATIVE_PLATFORM
    #ifdef FUNCTION_STORAGE_TUXERA_PATCH    
    if (size == 0) {
        int err = getExFatInfo(devPath, NULL, 0, NULL, &size);
        if (err) {
            size = 0;
        }
    }
    #endif                  
    //-NATIVE_PLATFORM

    if (size == 0) {
        return getNtfsSize(devPath);
    }

    return size;
}

static int _dirfd = -1;

static const char *getMountDir(const char *mntPnt) {
    if (strncmp(mntPnt, STORAGE_PREFIX, strlen(STORAGE_PREFIX))) {
        SLOGI("%s is not valid mount point", mntPnt);
        return NULL;
    }

    return mntPnt + strlen(STORAGE_PREFIX);
}

void markMediaInserted(const char *mntPnt) {
    const char *dirname = getMountDir(mntPnt);

    if (dirname == NULL)
        return;

    if (_dirfd == -1) {
        mkdir(MOUNTED_DIR, 0755);
        chmod(MOUNTED_DIR, 0755);
        _dirfd = open(MOUNTED_DIR, O_DIRECTORY);
    }

    SLOGI("create mark file %s on %s", MOUNTED_DIR, dirname);
    int fd = openat(_dirfd, dirname, O_WRONLY | O_CREAT, 0644);
    fchmod(fd, 0644);
    close(fd);
}

void unmarkMediaInserted(const char *mntPnt) {
    const char *dirname = getMountDir(mntPnt);
    SLOGI("remove mark file %s on %s", MOUNTED_DIR, dirname);
    if (dirname)
        unlinkat(_dirfd, dirname, 0);
}

#if !ANDROID
int main(int argc, char **argv) {
    char *dev = argv[1];
    char label[13];
    uint32_t id;
    getFatInfo(dev, label, &id);
    printf("size = %llu\n", getFatSize(dev));
}
#endif
