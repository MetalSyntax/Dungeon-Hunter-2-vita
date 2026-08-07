/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  io.h
 * @brief Wrappers and implementations for some of the IO functions.
 */

#ifndef SOLOADER_IO_H
#define SOLOADER_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <sys/dirent.h>
#include <sys/syslimits.h>
#include <sys/fcntl.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef DT_DIR
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14
#endif

typedef struct __attribute__((__packed__)) stat64_bionic {
    unsigned long long st_dev;
    unsigned char __pad0[4];
    unsigned long __st_ino;
    unsigned int st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    unsigned long long st_rdev;
    unsigned char __pad3[4];
    long long st_size;
    unsigned long st_blksize;
    unsigned long long st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    unsigned long long st_ino;
} stat64_bionic;

typedef struct __attribute__((__packed__)) dirent64_bionic {
    int16_t d_ino; // 2 bytes // offset 0x0
    int64_t d_off; // 8 bytes // offset 0x2
    uint64_t d_reclen; // 8 bytes // 0xA
    unsigned char d_type; // 1 byte // offset 0x12
    char d_name[256]; // 256 bytes // offset 0x13
} dirent64_bionic;

int open_soloader(const char * path, int oflag, ...);

FILE * fopen_soloader(const char * filename, const char * mode);

DIR *opendir_soloader(char *name);

int stat_soloader(const char * path, stat64_bionic * buf);

int fstat_soloader(int fd, stat64_bionic * buf);

struct dirent64_bionic * readdir_soloader(DIR *dir);

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result);

int close_soloader(int fd);

int fclose_soloader(FILE *f);

// Cache-handle-safe wrappers for the small read-only file cache in io.c --
// see fopen_soloader's implementation comment for why every one of these
// exists (a cache handle must never reach the real libc function).
size_t fread_soloader(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite_soloader(const void *ptr, size_t size, size_t nmemb, FILE *f);
int fseek_soloader(FILE *f, long offset, int whence);
long ftell_soloader(FILE *f);
void rewind_soloader(FILE *f);
int feof_soloader(FILE *f);
int ferror_soloader(FILE *f);
int fflush_soloader(FILE *f);
int fgetc_soloader(FILE *f);
int getc_soloader(FILE *f);
int fputc_soloader(int c, FILE *f);
int putc_soloader(int c, FILE *f);
char *fgets_soloader(char *str, int n, FILE *f);
int fputs_soloader(const char *str, FILE *f);
int fileno_soloader(FILE *f);
int setvbuf_soloader(FILE *f, char *buf, int mode, size_t size);
int ungetc_soloader(int c, FILE *f);
int fseeko_soloader(FILE *f, off_t offset, int whence);
off_t ftello_soloader(FILE *f);

int closedir_soloader(DIR *dir);

int fcntl_soloader(int fd, int cmd, ...);

int ioctl_soloader(int fd, int request, ... /* arg */);

int fsync_soloader(int fd);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_IO_H
