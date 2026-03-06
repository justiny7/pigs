#include "fat.h"
#include "sd.h"
#include "uart.h"
#include "lib.h"

/* 8.3 filename: "NAME    EXT" (8 chars name, space-padded, then 3 chars extension) */
#define FILE_NAME "TWEETS  CSV"

int main(void)
{
    if (sd_init() != SD_OK) {
        uart_puts("ERROR: SD init failed\n");
        rpi_reset();
    }
    uart_puts("SD init OK\n");

    if (!fat_getpartition()) {
        uart_puts("ERROR: FAT partition not found\n");
        rpi_reset();
    }
    uart_puts("FAT partition OK\n");

    unsigned int file_size = 0;
    unsigned int cluster = fat_getcluster(FILE_NAME, &file_size);
    if (cluster == 0) {
        uart_puts("ERROR: File not found: " FILE_NAME "\n");
        rpi_reset();
    }

    unsigned int bytes_read = 0;
    char *data = fat_readfile(cluster, &bytes_read);
    if (data == 0) {
        uart_puts("ERROR: Failed to read file\n");
        rpi_reset();
    }

    uart_puts("File size: ");
    uart_putd(file_size);
    uart_puts(" bytes, read: ");
    uart_putd(bytes_read);
    uart_puts(" bytes\n");

    uart_puts("File read OK. First 64 bytes:\n");
    for (unsigned int i = 0; i < 64 && i < file_size && data[i] != '\0'; i++) {
        uart_putc((unsigned char) data[i]);
    }

    uart_puts("\nLast 64 bytes of file:\n");
    // Use actual file_size for last bytes
    unsigned int end = file_size;
    unsigned int start = (end > 64) ? (end - 64) : 0;
    for (unsigned int i = start; i < end; i++) {
        uart_putc((unsigned char) data[i]);
    }
    uart_putc('\n');
    uart_puts("\nDone.\n");

    rpi_reset();
    return 0;
}
