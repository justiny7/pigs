/*
 * Referenced https://github.com/bztsrc/raspi3-tutorial/tree/master/0D_readfile
 */

#include "fat.h"
#include "emmc.h"
#include "uart.h"
#include "lib.h"

#include <stddef.h>

#define DEBUG
#include "debug.h"

// #define VERBOSE

extern uint8_t __heap_start__[];

/* memcmp for freestanding build (-nostdlib); no libc */
static int memcmp(const void *s1, const void *s2, int n) {
    const uint8_t *a = s1, *b = s2;
    while (n-- > 0) {
        if (*a != *b) return *a - *b;
        a++, b++;
    }
    return 0;
}

static uint32_t partition_lba;
static uint32_t fat_lba;
static uint32_t data_lba;
static uint8_t* mbr;
static bpb_t* bpb;
static uint32_t* fat;

int fat_getpartition() {
    mbr = malloc(SECTOR_SIZE);
    bpb = (bpb_t*) mbr; // can overwrite since we only read mbr once

    // read the partitioning table
    if (sd_readblock(0, mbr, 1)) {
        // check magic
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
            uart_puts("ERROR: Bad magic in MBR\n");
            return 0;
        }

        // check partition type: 0x0B = FAT32 CHS, 0x0C = FAT32 LBA
        if (mbr[0x1C2] != 0xB && mbr[0x1C2] != 0xC) {
            uart_puts("ERROR: Wrong partition type (need FAT32, got 0x");
            uart_hex(mbr[0x1C2]);
            uart_puts(")\n");
            return 0;
        }

        // should be this, but compiler generates bad code...
        partition_lba = mbr[0x1C6] +
            (mbr[0x1C7] << 8)  +
            (mbr[0x1C8] << 16) +
            (mbr[0x1C9] << 24);

        // read the boot record
        if (!sd_readblock(partition_lba, bpb, 1)) {
            uart_puts("ERROR: Unable to read boot record\n");
            return 0;
        }

        // check file system type. We don't use cluster numbers for that, but magic bytes
        if (!(bpb->fst[0] == 'F' && bpb->fst[1] == 'A' && bpb->fst[2] == 'T') &&
                !(bpb->fst2[0] == 'F' && bpb->fst2[1] == 'A' && bpb->fst2[2] == 'T')) {
            uart_puts("ERROR: Unknown file system type\n");
            return 0;
        }

        fat_lba = partition_lba + bpb->reserved_nsec;
        data_lba = fat_lba + bpb->nfats * bpb->nsec_per_fat;

        // read in FAT (assuming only 1)
        fat = malloc(bpb->nsec_per_fat * bpb->nbytes_per_sec);
        sd_readblock(fat_lba, fat, bpb->nsec_per_fat);

#ifdef VERBOSE
        uart_puts("FAT Bytes per Sector: ");
        uart_hex(bpb->nbytes_per_sec);
        uart_puts("\nFAT Sectors per Cluster: ");
        uart_hex(bpb->nsec_per_cluster);
        uart_puts("\nFAT Number of FAT: ");
        uart_hex(bpb->nfats);
        uart_puts("\nFAT Sectors per FAT: ");
        uart_hex(bpb->nsec_per_fat);
        uart_puts("\nFAT Reserved Sectors Count: ");
        uart_hex(bpb->reserved_nsec);
        uart_puts("\nFAT First data sector: ");
        uart_hex(data_lba);
        uart_puts("\n");
#endif

        return 1;
    }
    return 0;
}

uint32_t cluster_to_lba(uint32_t cluster) {
    return data_lba + (cluster - 2) * bpb->nsec_per_cluster;
}
uint32_t cluster_chain_len(uint32_t start_cluster) {
    uint32_t res = 1;
    while (fat[start_cluster] < LAST_CLUSTER) {
        res++;
        start_cluster = fat[start_cluster];
    }
    return res;
}
void cluster_chain_read(uint32_t start_cluster, uint8_t* data) {
    uint32_t bytes_per_cluster = bpb->nsec_per_cluster * bpb->nbytes_per_sec;
    uint32_t i = 0;
    while (fat[start_cluster] < LAST_CLUSTER) {
        sd_readblock(cluster_to_lba(start_cluster), &data[i], bpb->nsec_per_cluster);

        start_cluster = fat[start_cluster];
        i += bytes_per_cluster;
    }
    sd_readblock(cluster_to_lba(start_cluster), &data[i], bpb->nsec_per_cluster);
}

fatdir_t* fat_statroot() {
    uint32_t num_clusters = cluster_chain_len(bpb->root_cluster);
    fatdir_t* dir = malloc(num_clusters * bpb->nsec_per_cluster * bpb->nbytes_per_sec);
    cluster_chain_read(bpb->root_cluster, (uint8_t*) dir);
    return dir;
}

int fat_skip_dirent(fatdir_t* dir) {
    uint8_t attr = dir->attr[0];
    return (attr & FAT32_HIDDEN) ||
        (attr & FAT32_SYS_FILE) ||
        (attr & FAT32_VOLUME_LBL) ||
        (dir->name[0] == 0xE5);
}
void fat_get_plys(fatdir_t** dirs, uint8_t** lfns, uint32_t* num_files) {
    fatdir_t* dir = fat_statroot();
    const char* ext = "PLY";

    uint32_t dir_cnt = 0;
    for (fatdir_t* d = dir; d->name[0]; d++) {
        if (fat_skip_dirent(d)) {
            continue;
        }

        if (!memcmp(d->ext, ext, 3)) {
            dir_cnt++;
        }
    }

    fatdir_t* res = malloc(dir_cnt * sizeof(fatdir_t));
    uint8_t* lfn = malloc(dir_cnt * MAX_LFN_LEN);
    memset(lfn, 0, dir_cnt * MAX_LFN_LEN);

    uint8_t cksum;
    for (int i = 0, l = 0; dir->name[0]; dir++) {
        if ((dir->attr[0] & 0xF) == FAT32_LFN) {
            fatdir_lfn_t* lfn_dir = (fatdir_lfn_t*) dir;
            assert(lfn_dir->lfn == 0x0F, "LFN type mismatch");

            cksum = lfn_dir->cksum;

            uint32_t offset = ((lfn_dir->seqno & 0x1F) - 1) * 13;
            for (int j = 0; j < 5; j++) {
                lfn[i * MAX_LFN_LEN + offset + j] = lfn_dir->name0[j] & 0xFF;
            }
            for (int j = 0; j < 6; j++) {
                lfn[i * MAX_LFN_LEN + offset + 5 + j] = lfn_dir->name1[j] & 0xFF;
            }
            for (int j = 0; j < 2; j++) {
                lfn[i * MAX_LFN_LEN + offset + 11 + j] = lfn_dir->name2[j] & 0xFF;
            }

            l += 13;

            continue;
        }
        if (fat_skip_dirent(dir)) {
            while (l > 0) {
                lfn[i * MAX_LFN_LEN + (--l)] = 0;
            }
            continue;
        }

        if (!memcmp(dir->ext, ext, 3)) {
            // check cksum
            if (l) {
                uint8_t ck = 0;
                for (int j = 0; j < 11; j++) {
                    ck = ((ck & 1) ? 0x80 : 0) + (ck >> 1) + *(dir->name + j);
                }
                assert(ck == cksum, "LFN checksum mismatch");
            }

            memcpy(&res[i++], dir, sizeof(fatdir_t));
            l = 0;
        }
    }

    *num_files = dir_cnt;
    *dirs = res;
    *lfns = lfn;
}
uint32_t fat_getcluster(char* fn, uint32_t* file_size) {
    fatdir_t* dir = fat_statroot();

    for (; dir->name[0]; dir++) {
        if (fat_skip_dirent(dir)) {
            continue;
        }

        if (!memcmp(dir->name, fn, 8) && !memcmp(dir->ext, fn+8, 3)) {
#ifdef VERBOSE
            uart_puts("FAT File ");
            uart_puts(fn);
            uart_puts(" starts at cluster: ");
            uart_putx(fatdir_get_cluster(dir));
            uart_puts(" size: ");
            uart_putx(dir->size);
            uart_puts("\n");
#endif

            if (file_size) {
                *file_size = dir->size;
            }

            return fatdir_get_cluster(dir);
        }
    }

    uart_puts("ERROR: file not found\n");
    return 0;
}

void fat_readfile_cluster(uint32_t cluster, uint8_t** data) {
    uint32_t num_clusters = cluster_chain_len(cluster);
    *data = malloc(num_clusters * bpb->nsec_per_cluster * bpb->nbytes_per_sec);
    cluster_chain_read(cluster, *data);
}


void fat_init() {
    assert(sd_init() == SD_OK, "SD init failed");

#ifdef VERBOSE
    uart_puts("SD init OK\n");
#endif

    assert(fat_getpartition(), "FAT partition not found");

#ifdef VERBOSE
    uart_puts("FAT partition OK\n");
#endif
}
void fat_readfile(const char* fn, uint8_t** data, uint32_t* filesize) {
    uint32_t cluster = fat_getcluster(fn, filesize);
    assert(cluster, "File not found");
    fat_readfile_cluster(cluster, data);
}

uint32_t fatdir_get_cluster(fatdir_t* dir) {
    return ((uint32_t) dir->ch << 16) | dir->cl;
}
