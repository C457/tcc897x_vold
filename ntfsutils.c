#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <linux/kdev_t.h>
#include <linux/fs.h>

#if ANDROID
#define LOG_TAG "Vold"
#include <cutils/log.h>
#else
#define SLOGE(fmt, ...) fprintf(stderr, fmt"\n", ##__VA_ARGS__)
#define SLOGI(fmt, ...) fprintf(stderr, fmt"\n", ##__VA_ARGS__)
#define SLOGW(fmt, ...) fprintf(stderr, fmt"\n", ##__VA_ARGS__)
#define SLOGD(fmt, ...) fprintf(stderr, fmt"\n", ##__VA_ARGS__)
#include <stdint.h>
#define PAGE_SHIFT      12
#endif

#include "ntfsutils.h"

struct NTFSBoot{
    unsigned char   bJumpCode[3];      // 0x00: Jump to boot code
    char            cSystemID[8];      // 0x03: System ID, equals "NTFS    "

    unsigned char   wBytesPerSector[2];// 0x0cB: Bytes per sector

    unsigned char   bSectorsPerCluster;// 0x0D: Sectors per cluster
    unsigned char   Unused1[7];        //
    unsigned char   bMediaType;        // 0x15: Media type (0xF8 - harddisk)
    unsigned char   Unused2[2];        //
    unsigned short  wSectorsPerTrack;  // 0x18: number of sectors per track
    unsigned short  wHeadNumber;       // 0x1A: number of heads per cylinder
    unsigned int    HiddenSectors;     // 0x1C: number of 'hidden' sectors
    unsigned char   Unused3[4];        //
    unsigned char   bBIOSDriveNumber;  // 0x24: BIOS drive number =0x80
    unsigned char   bReserved1;
    unsigned char   bExtendedSignature;// 0x26: Extended BOOT signature =0x80
    unsigned char   bReserved2;
    uint64_t        SectorsPerVolume;  // 0x28: size of volume in sectors
    uint64_t        MFTCluster;        // 0x30: first cluster of $MFT
    uint64_t        MFTCopyCluster;    // 0x38: first cluster of $MFTMirr
    signed char     bMFTRecordSize;    // 0x40: size of MFT record in clusters(sectors)
    unsigned char   bReserved3[3];
    signed char     bIndexRecordSize;  // 0x44: size of INDX record in clusters(sectors)
    char            bReserved4[3];
    uint64_t        SerialNumber;      // 0x48: Volume serial number
    unsigned int    CheckSum;          // 0x50: Simple additive checksum of all of the ULONGs which precede the Checksum

    unsigned char   BootCode[ 0x200 - 0x50 - 2 - 4 ]; // 0x54: 0x1AA = 426
    unsigned char   Magic[2];          // 0x1FE: Boot signature =0x55 + 0xAA
};

/**
 * ntfs 볼륨의 정보를 얻어옵니다.
 * @param devPath ntfs 볼륨이 포함된 블럭장치의 경로
 * @param label [OUT] ntfs 볼륨의 label, 최대크기는 12byte
 *    현재 사용되지 않으므로 구현하지 않아도 됩니다.
 * @param id [OUT] ntfs 볼륨 MFT의 guid
 * @return ntfs 볼륨에서 데이터를 얻어오면 0
 *    ntfs볼륨이 아니거나, devPath의 경로가 잘못되었거나,
 *    정보를 얻는데 실패하면 -1
 */
int getNtfsInfo(const char *devPath, char *label, int64_t *id) {
    struct NTFSBoot boot;
    FILE *fp = fopen(devPath, "r");
    if(fp == NULL) {
        SLOGE("getNtfsId: cannot open device: %s %s", devPath, strerror(errno));
        return -1;
    }

    if ( fread( &boot, 1, sizeof(boot), fp) != sizeof(boot) ) {
        SLOGE("fread fail. path=%s", devPath);
        boot.SerialNumber = -1;
    } else {
        SLOGD("getNtfsId() id=%llx", boot.SerialNumber);
    }

    fclose(fp);

    *id = boot.SerialNumber;
    *label = boot.cSystemID;

    return 0;
}

/**
 * ntfs 볼륨의 크기를 얻어옵니다.
 * @param devPath ntfs 볼륨이 포함된 블럭장치의 경로
 * @return ntfs 볼륨의 크기
 *    ntfs볼륨이 아니거나, devPath의 경로가 잘못되었거나,
 *    정보를 얻는데 실패하면 0
 */
uint64_t getNtfsSize(const char *devPath) {
    struct NTFSBoot boot;
    uint64_t NtfsSize;

    FILE *fp = fopen(devPath, "r");
    if (fp == NULL) {
        SLOGE("getNtfsSize: cannot open device: %s %s", devPath, strerror(errno));
        return 0;
    }

    if ( fread( &boot, 1, sizeof(boot), fp) != sizeof(boot) ) {
        SLOGE("fread fail. path=%s", devPath);
        NtfsSize = 0;
    } else {
        NtfsSize = boot.SectorsPerVolume * (boot.wBytesPerSector[1] << 8);
        SLOGD("getNtfsSize() size=%lld", NtfsSize);
    }

    fclose(fp);
    return NtfsSize;
}
