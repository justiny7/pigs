/*
 * Referenced https://github.com/bztsrc/raspi3-tutorial/tree/master/0D_readfile
 */

#ifndef FAT_H
#define FAT_H

#include <stdint.h>

#define SECTOR_SIZE 512
#define MAX_LFN_LEN 256
#define LAST_CLUSTER 0xFFFFFF8

// the BIOS Parameter Block (in Volume Boot Record)
typedef struct {
    uint8_t   jmp[3];
    uint8_t   oem[8];
    uint16_t  nbytes_per_sec;
    uint8_t   nsec_per_cluster;
    uint16_t  reserved_nsec;
    uint8_t   nfats;
    uint16_t  max_files; // legacy (0 for FAT32)
    uint16_t  fs_nsec; // num sectors in filesystem
    uint8_t   media; // 0xF0 = removable disk, 0xF8 = fixed disk
    uint16_t  spf16; // size of each FAT (0 for FAT32)
    uint16_t  sectors_per_track;
    uint16_t  nheads; // number of heads
    uint32_t  hidden_nsec; // number of hidden secs
    uint32_t  total_nsec; // total nsec in filesystem
    uint32_t  nsec_per_fat;
    uint32_t  flg;
    uint32_t  root_cluster;
    uint8_t   vol[6];
    uint8_t   fst[8];
    uint8_t   dmy[20];
    uint8_t   fst2[8];
} __attribute__((packed)) bpb_t;


typedef struct {
    uint8_t   name[8];
    uint8_t   ext[3];
    uint8_t   attr[9];
    uint16_t  ch;
    uint32_t  attr2;
    uint16_t  cl;
    uint32_t  size;
} __attribute__((packed)) fatdir_t;

typedef struct {
    uint8_t   seqno;    // bits 0-3: which # in seq, bit 6: last seg
    uint16_t  name0[5]; // first 5 chars (UTF-16) of name
    uint8_t   lfn;      // always 0x0F
    uint8_t   zero0;    // always 0
    uint8_t   cksum;    // checksum of 8.3 filename
    uint16_t  name1[6]; // next 6 chars
    uint8_t   zero1[2];  // always 0
    uint16_t  name2[2]; // last 2 chars
} __attribute__((packed)) fatdir_lfn_t;

enum {
    FAT32_RO         = 0x1, // read-only
    FAT32_HIDDEN     = 0x2,
    FAT32_SYS_FILE   = 0x4,
    FAT32_VOLUME_LBL = 0x8,
    FAT32_LFN        = 0xF,  // long file name (if bits 0-3 are set)
    FAT32_DIR        = 0x10
};

void fat_init();
void fat_get_plys(fatdir_t** dirs, uint8_t** lfns, uint32_t* num_files);
void fat_readfile(const char* fn, uint8_t** data, uint32_t* filesize);
void fat_readfile_cluster(uint32_t cluster, uint8_t** data);
uint32_t fatdir_get_cluster(fatdir_t* dir);

#endif
