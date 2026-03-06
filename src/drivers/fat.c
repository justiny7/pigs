/*
 * Copyright (C) 2018 bzt (bztsrc@github)
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "sd.h"
#include "uart.h"

/* memcmp for freestanding build (-nostdlib); no libc */
static int memcmp(const void *s1, const void *s2, int n)
{
    const unsigned char *a = s1, *b = s2;
    while (n-- > 0) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return 0;
}

// get the end of bss segment from linker
extern unsigned char __bss_end__[];

static unsigned int partitionlba = 0;

// the BIOS Parameter Block (in Volume Boot Record)
typedef struct {
    char            jmp[3];
    char            oem[8];
    unsigned char   bps0;
    unsigned char   bps1;
    unsigned char   spc;
    unsigned short  rsc;
    unsigned char   nf;
    unsigned char   nr0;
    unsigned char   nr1;
    unsigned short  ts16;
    unsigned char   media;
    unsigned short  spf16;
    unsigned short  spt;
    unsigned short  nh;
    unsigned int    hs;
    unsigned int    ts32;
    unsigned int    spf32;
    unsigned int    flg;
    unsigned int    rc;
    char            vol[6];
    char            fst[8];
    char            dmy[20];
    char            fst2[8];
} __attribute__((packed)) bpb_t;

// directory entry structure
typedef struct {
    char            name[8];
    char            ext[3];
    char            attr[9];
    unsigned short  ch;
    unsigned int    attr2;
    unsigned short  cl;
    unsigned int    size;
} __attribute__((packed)) fatdir_t;

/**
 * Get the starting LBA address of the first partition
 * so that we know where our FAT file system starts, and
 * read that volume's BIOS Parameter Block
 */
int fat_getpartition(void)
{
    unsigned char *mbr=__bss_end__;
    bpb_t *bpb=(bpb_t*)__bss_end__;
    // read the partitioning table
    if(sd_readblock(0,__bss_end__,1)) {
        // check magic
        if(mbr[510]!=0x55 || mbr[511]!=0xAA) {
            uart_puts("ERROR: Bad magic in MBR\n");
            return 0;
        }
        // check partition type (FAT32 LBA only)
        if(mbr[0x1C2]!=0xC/*FAT32 LBA*/) {
            uart_puts("ERROR: Wrong partition type (need FAT32)\n");
            return 0;
        }
        // should be this, but compiler generates bad code...
        //partitionlba=*((unsigned int*)((unsigned long)&_end+0x1C6));
        partitionlba=mbr[0x1C6] + (mbr[0x1C7]<<8) + (mbr[0x1C8]<<16) + (mbr[0x1C9]<<24);
        // read the boot record
        if(!sd_readblock(partitionlba,__bss_end__,1)) {
            uart_puts("ERROR: Unable to read boot record\n");
            return 0;
        }
        // check file system type. We don't use cluster numbers for that, but magic bytes
        if( !(bpb->fst[0]=='F' && bpb->fst[1]=='A' && bpb->fst[2]=='T') &&
            !(bpb->fst2[0]=='F' && bpb->fst2[1]=='A' && bpb->fst2[2]=='T')) {
            uart_puts("ERROR: Unknown file system type\n");
            return 0;
        }
        return 1;
    }
    return 0;
}

/**
 * Find a file in root directory entries
 * @param fn: 8.3 filename (11 chars, space-padded)
 * @param file_size: output parameter for file size (can be NULL)
 */
unsigned int fat_getcluster(char *fn, unsigned int *file_size)
{
    bpb_t *bpb=(bpb_t*)__bss_end__;
    fatdir_t *dir=(fatdir_t*)(__bss_end__+512);
    unsigned int root_sec, s;
    // find the root directory's LBA (FAT32)
    root_sec=(bpb->spf32*bpb->nf)+bpb->rsc;
    // FAT32: root dir starts at root cluster
    root_sec+=(bpb->rc-2)*bpb->spc;
    // FAT32: root dir is a cluster chain, read at least 1 cluster (spc sectors)
    s = bpb->spc * 512;
    // add partition LBA
    root_sec+=partitionlba;
    // load the root directory (read at least 16 sectors = 256 entries for FAT32)
    unsigned int sectors_to_read = s/512;
    if(sectors_to_read < 16) sectors_to_read = 16;
    if(sd_readblock(root_sec,(unsigned char*)dir,sectors_to_read)) {
        // iterate on each entry and check if it's the one we're looking for
        for(;dir->name[0]!=0;dir++) {
            // is it a valid entry?
            if(dir->name[0]==0xE5 || dir->attr[0]==0xF) continue;
            // // debug: print raw filename bytes
            // uart_puts("DIR entry: ");
            // for (int i = 0; i < 8; i++) uart_putc(dir->name[i]);
            // uart_puts(" ");
            // for (int i = 0; i < 3; i++) uart_putc(dir->ext[i]);
            // uart_puts("\n");
            // filename match?
            if(!memcmp(dir->name,fn,8) && !memcmp(dir->ext,fn+8,3)) {
                uart_puts("FAT File ");
                uart_puts(fn);
                uart_puts(" starts at cluster: ");
                uart_hex(((unsigned int)dir->ch)<<16|dir->cl);
                uart_puts(" size: ");
                uart_hex(dir->size);
                uart_puts("\n");
                // return file size if requested
                if (file_size) *file_size = dir->size;
                // return starting cluster
                return ((unsigned int)dir->ch)<<16|dir->cl;
            }
        }
        uart_puts("ERROR: file not found\n");
    } else {
        uart_puts("ERROR: Unable to load root directory\n");
    }
    return 0;
}

/**
 * Read a file into memory
 * @param cluster: starting cluster of the file
 * @param bytes_read: output parameter for total bytes read (can be NULL)
 */
char *fat_readfile(unsigned int cluster, unsigned int *bytes_read)
{
    // BIOS Parameter Block
    bpb_t *bpb=(bpb_t*)__bss_end__;
    // FAT32 table
    unsigned int *fat32=(unsigned int*)(__bss_end__+bpb->rsc*512);
    // Data pointers
    unsigned int data_sec;
    unsigned char *data, *ptr;
    unsigned int total_bytes = 0;
    // find the LBA of the first data sector (FAT32)
    data_sec=(bpb->spf32*bpb->nf)+bpb->rsc;
    // add partition LBA
    data_sec+=partitionlba;
    // dump important properties
    uart_puts("FAT Bytes per Sector: ");
    uart_hex(bpb->bps0 + (bpb->bps1 << 8));
    uart_puts("\nFAT Sectors per Cluster: ");
    uart_hex(bpb->spc);
    uart_puts("\nFAT Number of FAT: ");
    uart_hex(bpb->nf);
    uart_puts("\nFAT Sectors per FAT: ");
    uart_hex(bpb->spf32);
    uart_puts("\nFAT Reserved Sectors Count: ");
    uart_hex(bpb->rsc);
    uart_puts("\nFAT First data sector: ");
    uart_hex(data_sec);
    uart_puts("\n");
    // load FAT table
    unsigned int fat_sectors = bpb->spf32 + bpb->rsc;
    unsigned int s = sd_readblock(partitionlba+1,(unsigned char*)__bss_end__+512,fat_sectors);
    // end of FAT in memory
    data=ptr=__bss_end__+512+s;
    // iterate on cluster chain
    // (FAT32 is actually FAT28 - upper 4 bits must be masked)
    while(cluster>1 && cluster<0x0FFFFFF8) {
        // load all sectors in a cluster
        sd_readblock((cluster-2)*bpb->spc+data_sec,ptr,bpb->spc);
        // move pointer, sector per cluster * bytes per sector
        unsigned int cluster_bytes = bpb->spc*(bpb->bps0 + (bpb->bps1 << 8));
        ptr += cluster_bytes;
        total_bytes += cluster_bytes;
        // get the next cluster in chain (mask upper 4 bits)
        cluster = fat32[cluster] & 0x0FFFFFFF;
    }
    if (bytes_read) *bytes_read = total_bytes;
    return (char*)data;
}

