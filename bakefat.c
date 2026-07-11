/*
 * bakefat.c: bootable external FAT disk image creator for DOS and Windows 3.1--95--98--ME
 * by pts@fazekas.hu at Thu Feb  6 14:32:22 CET 2025
 *
 * Compile with OpenWatcom C compiler, minilibc686 for Linux i386: minicc -march=i386 -Werror -Wno-n201 -o bakefat bakefat.c
 * Compile with GCC for Unix: gcc -ansi -pedantic -W -Wall -Wno-overlength-strings -Werror -s -O2 -o bakefat bakefat.c
 * Compile with OpenWatcom C compiler for Win32: owcc -bwin32 -Wl,runtime -Wl,console=3.10 -Os -fno-stack-check -march=i386 -W -Wall -Wno-n201 -o bakefat.exe bakefat.c
 *
 * !! Fix last-sector-read bug. Test reading the last FAT sector within QEMU and VirtualBox.
 * !! doc: Check wheter MS-DOS 8.0 needs the first 4 sectors of io.sys to be contiguous.
 * !! doc: Can VirtualBox open with .img, without VBOX_E_OBJECT_NOT_FOUND? Or just .vhd extension?
 * !! Add boot sector for booting Windows NT--2000--XP (ntldr) from FAT16 and FAT32.
 * !! Add 32-bit DOS port bakefatdexe. (Make sure it compiles with owcc -bpmodew etc.)
 * !! Write directly to block device on Unix (how to resize it?), clear existing FAT table (cluster chain pointers) and first root directory entry.
 * !! Write directly to block device on DOS (specified as e.g. a:, c:).
 * !! Exclude FAT header and partition table from boot_bin, making it shorter.
 * !! Add implementation using <windows.h>, which compiles with MSVC, Borland C compiler and Digital Mars C compiler in addition to OpenWatcom C compiler.
 * !! Add support for VHD_SPARSE (SPARSEVHD). !! Experiment with block sizes smaller than 2 MiB. (NTFS sparse files have block size 64 KiB.)
 * !! Add command-line flag for fewer reserved sectors (minimum: 2 or 3) for FAT32.
 * !! Add command-line flag RNDUUID, to base the VHD UUID on the result of gettimeofday(2) and getpid(2).
 * !! Move all relevant comments from fat16m.nasm to bakefat.c, and remove fat16m.nasm.
 *
 * !! Add feature to copy files to the root directory (like mcopy(1)).
 * !! Create io.sys patch for MS-DOS 3.30.
 * !! Release the MS-DOS io.sys patches.
 * !! Add multisector boot code to detect and boot everything.
 * !! Add fixup to the multisector boot code to load the entire *io.sys* (or *ibmbio.com*), autodetect the end of MSLOAD, and delete MSLOAD. The benefit is that io.sys can be fragmented. This has to be smart (detect the near jmp? DOS 3.30 doesn't have it), also for small-io-sys.
 * !! Make the boot sectors boot from 0x07c0:0 (some buggy BIOS, according to the GRUB 1 0.97 stage1/stage1.S soure code).
 */

#ifndef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS 64  /* __GLIBC__ and __UCLIBC__ use lseek64(...) instead of lseek(...), and use ftruncate64(...) instead of ftruncate(...). */
#endif
#define _LARGEFILE64_SOURCE  /* __GLIBC__ lseek64(...). */
#define _XOPEN_SOURCE  /* __GLIBC__ ftruncate64(...) with `gcc -ansi -pedantic. */
#define _XOPEN_SOURCE_EXTENDED  /* __GLIBC__ ftruncate64(...). */
#ifdef __MMLIBC386__
#  include <mmlibc386.h>
#else
#  include <fcntl.h>
#  include <stdint.h>
#  include <stdarg.h>
#  include <stdio.h>
#  include <string.h>
#  include <strings.h>  /* strcasecmp(...). */
#  include <stdlib.h>
#  if defined(_WIN32) || defined(__NT__) || defined(MSDOS) || defined(__MSDOS__) || defined(__DOS__)
#    define BAKEFAT_DOS_OR_WIN32 1
#    include <io.h>
#  else
#    include <unistd.h>
#  endif
#endif

#ifndef BAKEFAT_VERSION
#  define BAKEFAT_VERSION 1
#endif

#undef noreturn
#ifdef __GNUC__
#  ifndef inline
#    define inline __inline__  /* For `gcc -ansi -pedantic'. */
#  endif
#  define noreturn __attribute__((__noreturn__))
#else
#  ifdef __WATCOMC__
#    define noreturn __declspec(noreturn)
#  else
#    define noreturn
#  endif
#endif

#undef  ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#undef  ARRAY_END
#define ARRAY_END(a) ((a) + (sizeof(a) / sizeof((a)[0])))

#ifndef O_BINARY
#  define O_BINARY 0
#endif

#ifdef __MMLIBC386__
#  define msg_printf printf_void
#else
#  ifdef __GNUC__
    __attribute__((__format__(__printf__, 1, 2)))
#  endif
  static void msg_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)!vfprintf(stderr, fmt, ap);
  }
#endif

#if defined(__WATCOMC__) && defined(__NT__) && defined(_WCDATA)  /* OpenWatcom C compiler, Win32 target, OpenWatcom libc. */
  /* OpenWatcom libc SetFilePointer: https://github.com/open-watcom/open-watcom-v2/blob/817428310bd22abeaf8a7018ce4c1c2578975543/bld/clib/handleio/c/__lseek.c#L97-L109 */
  /* Overrides lib386/nt/clib3r.lib / mbcupper.o
   * Source: https://github.com/open-watcom/open-watcom-v2/blob/master/bld/clib/mbyte/c/mbcupper.c
   * Overridden implementation calls CharUpperA in USER32.DLL:
   * https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-charuppera
   *
   * This function is a transitive dependency of _cstart() with main() in
   * OpenWatcom. By overridding it, we remove the transitive dependency of all
   * .exe files compiled with `owcc -bwin32' on USER32.DLL.
   *
   * This is a simplified implementation, it keeps non-ASCII characters intact.
   */
  unsigned int _mbctoupper(unsigned int c) {
    return (c - 'a' + 0U <= 'z' - 'a' + 0U)  ? c + 'A' - 'a' : c;
  }
#  define IS_WINNT() ((short)_osbuild >= 0)  /* https://github.com/open-watcom/open-watcom-v2/blob/b59d9d9ea5b9266e66efa305f970fe0a51892bb7/bld/lib_misc/h/osver.h#L43 */
#  define IS_WINNT_WIN32API() ((int)GetVersion() >= 0)  /* https://github.com/open-watcom/open-watcom-v2/blob/b59d9d9ea5b9266e66efa305f970fe0a51892bb7/bld/clib/startup/c/mainwnt.c#L138-L142 and https://github.com/open-watcom/open-watcom-v2/blob/b59d9d9ea5b9266e66efa305f970fe0a51892bb7/bld/lib_misc/h/osver.h#L43 */
  int __stdcall kernel32_SetEndOfFile(int handle);
#  pragma aux kernel32_SetEndOfFile "_SetEndOfFile@4"
  static const char nul_buf[0x1000];
  /* It assumes that the current position is new_ofs, and upon success, it
   * will be the same when it returns. The return value is new_ofs on
   * success, and -1 on error.
   */
  static int64_t fill_fd_with_nul(int64_t new_ofs, int fd, int64_t size) {
    int got;
#  if 0
    msg_printf("info: fill_fd_with_nul: new_ofs=0x%llx size=0x%llx\n", new_ofs, size);
#  endif
    if (!IS_WINNT() && size < new_ofs) {
      if (_lseeki64(fd, size, SEEK_SET) != size) return -1;
      got = -(unsigned)size & (sizeof(nul_buf) - 1);  /* The initiali write would align the file size to a multiple of sizeof(nul_buf). This is for speed. */
      size = new_ofs - size;
      if (!got || (unsigned)got > (uint64_t)size) goto new_got;
      do {
        if ((got = (int)write(fd, nul_buf, got)) <= 0) return -1;
#  if 0
        msg_printf("info: fill_fd_with_nul: got=0x%x\n", got);
#  endif
        size -= (unsigned)got;
       new_got:
        got = (uint64_t)size > sizeof(nul_buf) ? (unsigned)sizeof(nul_buf) : (unsigned)size;
      } while (size > 0);
    }
    return new_ofs;
  }
  /* We fix it because OpenWatcom libc _lseeki64 adds undefined bytes
   * (rather than NUL) bytes when growing the file. This has been verified
   * on Windows 95 OSR2 and WDOSX. (The default syscall (int 21h with
   * AH==40h) on MS-DOS (tested with 6.22 and 7.1) also adds undefined
   * bytes.) SetFilePointer(...) and SetEndOfFile(...) on Windows NT 3.1 and
   * above and derivatives add NUL bytes.
   *
   * This function is not POSIX, because sometimes it explicitly grows the
   * file if seeking beyond EOF, and POSIX would grow it later, after the
   * first non-empty write(2). But it's good enough for bakefat.
   */
  static int64_t bakefat_lseek64(int fd, int64_t offset, int whence) {
    int64_t old_ofs, size, new_ofs;
    if ((old_ofs = _lseeki64(fd, 0, SEEK_CUR)) < 0 ||
        (size = _lseeki64(fd, 0, SEEK_END)) < 0 ||
        (new_ofs = _lseeki64(fd, offset, whence)) < 0) return -1;
    return fill_fd_with_nul(new_ofs, fd, size);  /* Force NUL for newly added bytes. https://stackoverflow.com/q/9809512/97248 */
  }
  /* We fix it because OpenWatcom libc _lseeki64 adds undefined bytes
   * (rather than NUL) bytes when growing the file. This has been verified
   * on Windows 95 OSR2 and WDOSX. (The default syscall (int 21h with
   * AH==40h) on MS-DOS (tested with 6.22 and 7.1) also adds undefined
   * bytes.) SetFilePointer(...) and SetEndOfFile(...) on Windows NT 3.1 and
   * above and derivatives add NUL bytes.
   *
   * Also we have to implement this because OpenWatcom libc doesn't provide
   * a 64-bit chsize(...) or ftruncate(...).
   */
  static int bakefat_ftruncate64(int fd, int64_t length) {
    int64_t old_ofs, size;
    if ((old_ofs = _lseeki64(fd, 0, SEEK_CUR)) < 0 ||
        (size = _lseeki64(fd, 0, SEEK_END)) < 0) return -1;
    if (length != size) {
      if (_lseeki64(fd, length, SEEK_SET) != length ||
          !kernel32_SetEndOfFile(_get_osfhandle(fd))) return -1;
      if (!IS_WINNT() && fill_fd_with_nul(length, fd, size) != length) return -1;  /* Force NUL for newly added bytes. https://stackoverflow.com/q/9809512/97248 */
    }
    if (old_ofs != length && _lseeki64(fd, old_ofs, SEEK_SET) != old_ofs) return -1;
    return 0;
  }
  int __stdcall kernel32_GetModuleHandleA(const char *lpModuleName);
#  pragma aux kernel32_GetModuleHandleA "_GetModuleHandleA@4"
  void * __stdcall kernel32_GetProcAddress(int hModule, const char *lpProcName);
#  pragma aux kernel32_GetProcAddress "_GetProcAddress@8"
#  define kernel32_FSCTL_SET_SPARSE 0x900c4  /* #define _WIN32_WINNT 0x0500 ++ #include <windows.h> --> FSCTL_SET_PARSE. Windows 2000 and later (e.g. Windows NT). */
  /* Converts file to sparse.
   *
   * Based on https://web.archive.org/web/20220207223136/http://www.flexhex.com/docs/articles/sparse-files.phtml
   *
   * The call below succeeds on Windows 2000, Windows XP and later
   * versions of Windows on NTFS filesystems (but fail on FAT
   * filesystems).
   *
   * Upon success, the image file will be sparse on NTFS filesystems, so
   * 64 KiB blocks skipped over (with lseek64(2) and ftruncate64(2)) won't
   * take actual disk space. Thus a `720K` floppy image will use 64 KiB,
   * and a `FAT12 256M` HDD will use 3*64 KiB (first 64 KiB: MBR, boot
   * sector and first sector of first FAT; second 64 KiB: first sector of
   * second FAT; third 64 KiB: root directory). This has been tested on
   * Windows XP.
   *
   * After creating a sparse file on Windows >=2000, Windows NT won't be
   * able to access the filesystem (not even directories or other files).
   */
  static void bakefat_set_sparse(int fd) {
    /* We look up the DLL function by name because not all Win32
     * kernel32.dll files have it, for example WDOSX doesn't have it.
     */
    int __stdcall (*DeviceIoControl)(int, unsigned, void *, unsigned, void *, unsigned, unsigned *, void *) =
        (int __stdcall (*)(int, unsigned, void *, unsigned, void *, unsigned, unsigned *, void *))
        kernel32_GetProcAddress(kernel32_GetModuleHandleA("kernel32.dll"), "DeviceIoControl");
    unsigned dwTemp;
#    ifdef DEBUG
    int result;
    if (!DeviceIoControl) {
      msg_printf("info: sparse: no DeviceIoControl API function\n", DeviceIoControl != NULL);
    } else if (DeviceIoControl(_get_osfhandle(fd), kernel32_FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dwTemp, NULL)) {
      msg_printf("info: sparse: set image file to sparse\n");
    } else {
      msg_printf("info: sparse: failed to set image file to sparse (needs Windows >=2000 and NTFS)\n");
    }
#    else
    if (DeviceIoControl) {
      DeviceIoControl(_get_osfhandle(fd), kernel32_FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dwTemp, NULL);
    }
#    endif
  }
#else
  /* FreeBSD and musl have 64-bit off_t, lseek(2) and ftruncate(2) by default. Linux libcs (uClibc, EGLIBC, minilibc686) have it with -D_FILE_OFFSET_BITS=64. */
#  define bakefat_lseek64(fd, offset, whence) lseek(fd, offset, whence)
#  ifdef BAKEFAT_DOS_OR_WIN32
#    define bakefat_ftruncate64(fd, length) chsize(fd, length)  /* !! On DOS, add manual filling with NUL bytes here, and also for lseek(...). */
#  else
#    define bakefat_ftruncate64(fd, length) ftruncate(fd, length)
#  endif
#  ifdef __MMLIBC386__
#    ifdef DEBUG
       static void bakefat_set_sparse(int fd) {
         msg_printf("info: sparse: %sset image file to sparse\n",
                    fsetsparse(fd) != 0 ? "failed to " : "");
       }
#    else
#      define bakefat_set_sparse(fd) fsetsparse(fd)
#    endif
#  else
#    define bakefat_set_sparse(fd) do {} while (0)  /* Fallback: no-op. */
#  endif
#endif

/* Byte offsets in boot_bin. */
#define BOOT_OFS_MBR 0
#define BOOT_OFS_FAT32 0x200
#define BOOT_OFS_FAT16 0x400
#define BOOT_OFS_FAT12 0x600
#define BOOT_OFS_FAT12_OFSS 0x800
#define BOOT_OFS_END (BOOT_OFS_FAT12_OFSS + 4 * 2)

#if CONFIG_INCBIN_BOOT_BIN  /* GCC + GNU as(1) or Clang. */
  extern const char boot_bin[];  /* Defined in boot.obj. */
  /* The default (.text) section is fine. The underscore prefix (of _boot_bin) is needed on _WIN32 and __APPLE__ (macOS etc.). */
  __asm__(".global boot_bin\nboot_bin:\n.global _boot_bin\n_boot_bin:\n.incbin \"boot.bin\"");
#endif

#if CONFIG_INCLUDE_BOOT_BIN
  const char boot_bin[] =
#    include "boot.h"
  ;
  typedef char assert_sizeof_boot_bin[sizeof(boot_bin) == BOOT_OFS_END + 1 ? 1 : -1];
#else
  extern const char boot_bin[];  /* Defined in boot.obj. */
#endif

typedef char assert_sizeof_uint32_t[sizeof(uint32_t) == 4 ? 1 : -1];

typedef uint8_t ub;
typedef uint16_t uw;
typedef uint32_t ud;
typedef int32_t sd;

/* GCC >= 4.6 and Clang >= 3.2 have __BYTE_ORDER__ defined. */
#if defined(__i386) || defined(__i386__) || defined(__amd64__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || defined(__386) || \
    defined(__X86_64__) || defined(_M_I386) || defined(_M_I86) || defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86) || defined(__386__) || \
    defined(__X86__) || defined(__I86__) || defined(_M_I86) || defined(_M_I8086) || defined(_M_I286)
#  define IS_X86 1
#endif
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__) || \
    defined(__ARMEB__) || defined(__THUMBEB__) || defined(__AARCH64EB__) || defined(_MIPSEB) || defined(__MIPSEB) || defined(__MIPSEB__) || \
    defined(__powerpc__) || defined(_M_PPC) || defined(__m68k__) || defined(_ARCH_PPC) || defined(__PPC__) || defined(__PPC) || defined(PPC) || \
    defined(__powerpc) || defined(powerpc) || (defined(__BIG_ENDIAN) && (!defined(__BYTE_ORDER) || __BYTE_ORDER == __BIG_ENDIAN +0)) || \
    defined(_BIG_ENDIAN)
#else
#  if defined(__LITTLE_ENDIAN__) || (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || \
      defined(__ARMEL__) || defined(__THUMBEL__) || defined(__AARCH64EL__) || defined(_MIPSEL) || defined (__MIPSEL) || defined(__MIPSEL__) || \
      defined(__ia64__) || defined(__LITTLE_ENDIAN) || defined(_LITTLE_ENDIAN) || defined(MSDOS) || defined(__MSDOS__) || defined(__DOS__) || IS_X86
#    define IS_LE 1
#  endif
#endif

static char sbuf[0x200];  /* A single sector. */
static char *s;  /* Output pointer within sbuf. */

static int sfd;
static const char *sfn;

static inline void db(ub x) { *s++ = x; }
/* Serializing integers in little endian. */
#ifdef IS_LE
  static inline void dw(uw x) { *(uw*)s = x; s += 2; }
  static inline void dd(ud x) { *(ud*)s = x; s += 4; }
  static inline uw gw(const char *p) { return *(const uw*)p; }
#else
  static uw gw(const char *p) { return ((const unsigned char*)p)[0] | ((const unsigned char*)p)[1] << 8; }
  static void dw(uw x) { *s++ = x & 0xff; *s++ = x >> 8; }
  /* !! Is this shorter: static void dd(ud x) { dw(x); dw(x >> 16); } */
  static void dd(ud x) { *s++ = x & 0xff; *s++ = (x >> 8) & 0xff; *s++ = (x >> 16) & 0xff; *s++ = x >> 24; }
#endif

/* Serializing integers in big endian. */
static void dwb(uw x) { *s++ = x >> 8; *s++ = x & 0xff; }
/* !! Is this shorter: static void ddb(ud x) { dwb(x >> 16); dwb(x); } */
static void ddb(ud x) { *s++ = x >> 24; *s++ = (x >> 16) & 0xff; *s++ = (x >> 8) & 0xff; *s++ = x & 0xff; }
/* Emits (uint64_t)x << 9 in big endian, in 8 bytes. Avoids overflow. */
static void dsb(ud x) {
  *s++ = 0; *s++ = 0; *s++ = x >> 31;
  x <<= 1;
  *s++ = x >> 24; *s++ = (x >> 16) & 0xff; *s++ = (x >> 8) & 0xff; *s++ = x & 0xff; *s++ = 0;
}

static void write_sector(ud sofs) {
  const uint64_t ofs = (uint64_t)sofs << 9;
  if ((uint64_t)bakefat_lseek64(sfd, ofs, SEEK_SET) != ofs) {
    msg_printf("fatal: error seeking to sector 0x%x in output file: %s\n", (unsigned)sofs, sfn);
    exit(2);
  }
  if ((size_t)write(sfd, sbuf, sizeof(sbuf)) != sizeof(sbuf)) {
    msg_printf("fatal: error writing to output file: %s\n", sfn);
    exit(2);
  }
}

/* It doesn't seek (i.e. it doesn't modify the file pointer). If it grows the file, it fills with NULs. */
static void set_file_size_scount(ud scount) {
  const uint64_t ofs = (uint64_t)scount << 9;
  if (bakefat_ftruncate64(sfd, ofs) != 0) {  /* It doesn't seek (i.e. it doesn't modify the file pointer). If it grows the file, it fills with NULs. */
    msg_printf("fatal: error setting the size of output file to 0x%x sectors: %s\n", (unsigned)scount, sfn);
    exit(2);
  }
}

struct fat_common_params {
  ud sector_count;
  ud cluster_count;
  ud sectors_per_fat;
  uw head_count;
  uw sectors_per_track;
  uw rootdir_entry_count;
  ub media_descriptor;
  ub log2_sectors_per_cluster;  /* Initial, unspecified value: (ub)-1. Valid specified value: 0, ..., 6. */
};

struct fat_params {
  struct fat_common_params fcp;
  ud hidden_sector_count;
  ud volume_id;
  ud cylinder_count;
  ud geometry_sector_count;
  uw reserved_sector_count;
  uw default_rootdir_entry_count;
  uw default_reserved_sector_count;
  ub default_log2_sectors_per_cluster;
  ub default_fat_count;
  ub fat_count;  /* 0 (unspecified), 1 or 2. */
  ub fat_fstype;  /* 0 (unspecified), 12, 16 or 32. */
  ub vhd_mode;  /* vhd_mode_t. 0 (unspecified), VHD_NOVHD == 1 (no VHD footer), VHD_FIXED == 2 (add fixed-size VHD footer). */
  ub os_compat;  /* os_compat_t. Operating system compatibility bitset. Default is 0 (no compatibility enforced). */
};

struct fat12_preset {
  const char *name;
  struct fat_common_params fcp;
};

/* !! Add superfloppy formats. */
static const struct fat12_preset fat12_presets[] = {
    { "160K",  {  320,  313, 1, 1,  8,  64, 0xfe, 0 } },
    { "180K",  {  360,  351, 2, 1,  9,  64, 0xfc, 0 } },
    { "320K",  {  640,  315, 1, 2,  8, 112, 0xff, 1 } },
    { "360K",  {  720,  354, 2, 2,  9, 112, 0xfd, 1 } },
    { "720K",  { 1440,  713, 3, 2,  9, 112, 0xf9, 1 } },
    { "1200K", { 2400, 2371, 7, 2, 15, 224, 0xf9, 0 } },
    { "1440K", { 2880, 2847, 9, 2, 18, 224, 0xf0, 0 } },
    { "2880K", { 5760, 2863, 9, 2, 36, 240, 0xf0, 1 } },
};

enum boot_signature_t { BOOT_SIGNATURE = 0xaa55, EXTENDED_BOOT_SIGNATURE = 0x29 };

static const char oem_name[] = "MSDOS5.0";

static noreturn void fatal0(const char *msg) {
  msg_printf("fatal: %s\n", msg);
  exit(2);
}

static const char *hdd_size_presets_m_21[] = {
    "2M", "4M", "8M", "16M", "32M", "64M", "128M", "256M", "512M", "1024M",
    "2048M", "4096M", "8192M", "16384M", "32768M", "65536M", "131072M",
    "262144M", "524288M", "1048576M", "2097152M",
};

static const char *hdd_size_presets_g_30[] = {
    "1G", "2G", "4G", "8G", "16G", "32G", "64G", "128G", "256G", "512G",
    "1024G", "2048G",
};

static const char *hdd_size_presets_t_40[] = {
    "1T", "2T",
};

static const char *sectors_per_cluster_presets_b_9[] = {
    "512B", "1024B", "2048B", "4096B", "8192B", "16384B", "32768B",
};

static const char *sectors_per_cluster_presets_s_9[] = {
    "1S", "2S", "4S", "8S", "16S", "32S", "64S",
};

static const char *sectors_per_cluster_presets_k_10[] = {
    "1K", "2K", "4K", "8K", "16K", "32K",
};

enum ptype_t {  /* Partition type. */
  PTYPE_EMPTY = 0,
  PTYPE_FAT12 = 1,
  PTYPE_FAT16_LESS_THAN_32MIB = 4,
  PTYPE_EXTENDED = 5,
  PTYPE_FAT16 = 6,
  PTYPE_HPFS_NTFS_EXFAT = 7,
  PTYPE_FAT32 = 0xb,
  PTYPE_FAT32_LBA = 0xc,
  PTYPE_FAT16_LBA = 0xe,
  PTYPE_EXTENDED_LBA = 0xf,
  PTYPE_MINIX_OLD = 0x80,
  PTYPE_MINIX = 0x81,
  PTYPE_LINUX_SWAP = 0x82,
  PTYPE_LINUX = 0x83,
  PTYPE_LINUX_EXTENDED = 0x85,
  PTYPE_LINUX_LVM = 0x8e,
  PTYPE_LINUX_RAID_AUTO = 0xfd
};

enum pstatus_t {  /* Partition status. */
  PSTATUS_INACTIVE = 0,
  PSTATUS_ACTIVE = 0x80
};

enum vhd_mode_t {  /* VHD mode. */
  VHD_UNKNOWN = 0,
  VHD_NOVHD = 1,  /* No VHD footer, create raw disk image. */
  VHD_FIXED = 2,  /* Fixed-size VHD. Same value as in the footer and in qemu-2.11.1/block/vpc.c. */
  VHD_DYNAMIC = 3  /* Dynamic (sparse) VHD. Currently not supported. Same value as in the footer and in qemu-2.11.1/block/vpc.c. */
};

enum os_compat_t {  /* Operating system compatibility bitset. */
  OSC_DOS3      = 1 << 3,  /* DOS 3.30--.    No FAT32 or 1FAT support. Limited HDD size support (16M and 32M only in bakefat). Must use 2K cluster size on HDD. Must use RDEC=512 for booting from HDD. Must use RSC=1. No 2880K support. No cluster size 32K on floppy support. No >=1K cluster size on 160K, 180K and 1440K floppy support. */
  OSC_DOS4      = 1 << 4,  /* DOS 4.x.       No FAT32 or 1FAT support. Must use RSC=1. No cluster size 32K of floppy support. No >=1K cluster size on 160K, 180K and 1400K floppy support. */
  OSC_DOS5_6    = 1 << 5,  /* DOS 5.00--6.x. No FAT32 or 1FAT support. No cluster size 32K on floppy support. No >=1K cluster size on 160K and 180K floppy support. */
  OSC_MSDOS70   = 1 << 6,  /* MS-DOS 7.0 (Windows 95 A, Windows 95 RTM). Same support as OSC_DOS5_6, but supports cluster size 32K on floppy. */
  OSC_MSDOS71_8 = 1 << 7,  /* MS-DOS 7.1 (Windows 95 OSR2, Windows 98), MS-DOS 8.0 (Windows ME), but supports FAT32 and 1FAT, and supports cluster size 32K on floppy, and supports cluster sizes 512B..32K on 160K and 180K floppy. */
  OSC_PCDOS70   = 1 << 0,  /* IBM PC DOS 7.0. Same support as OSC_DOS5_6. */
  OSC_PCDOS71   = 1 << 1   /* IBM PC DOS 7.1. Same support as OSC_DOS6_6, but supports FAT32 and 1FAT. */
};

/* 2040 GiB max VHD image size. This is enforced by QEMU 2.11.1
 * (`qemu-system-i386 -drive file=hd.img,format=vpc`: it fails with error
 * *File too large*.
 *
 * Maximum documented in: https://kb.msp360.com/standalone-backup/restore/vhd-disk-size-exceeded
 */
#define VHD_MAX_SECTORS 0xff000000U

static void create_fat(const struct fat_params *fpp) {
  const ud fat_sector_size = 0x200;
  const ud fat_rootdir_sector_count = (ud)fpp->fcp.rootdir_entry_count >> 4;
  const ud fat_fat_sec_ofs = fpp->hidden_sector_count + fpp->reserved_sector_count;
  const ud fat_rootdir_sec_ofs = fat_fat_sec_ofs + ((ud)fpp->fcp.sectors_per_fat << (fpp->fat_count - 1U));
  const ud fat_clusters_sec_ofs = fat_rootdir_sec_ofs + fat_rootdir_sector_count;
  const uw first_boot_sector_copy_sec_ofs = (fpp->fat_fstype != 32 || fpp->reserved_sector_count <= 2U) ? 0U : fpp->reserved_sector_count < 6U ? 2U : 6U;  /* 6 was created by Linux mkfs.vfat(1), also for Windows XP. */
  ud checksum, vhd_sector_count = 0;
#  ifdef DEBUG
    /* We have the +2 here because clusters 0 and 1 have a next-pointer in the FATs, but they are not stored on disk. */
    const ud min_sectors_per_fat =
        fpp->fat_fstype == 32 ? (fpp->fcp.cluster_count + (2U + 0x7fU)) >> 7 :
        fpp->fat_fstype == 16 ? (fpp->fcp.cluster_count + (2U + 0xffU)) >> 8 :
        /* FAT12: */ ((((fpp->fcp.cluster_count + 2) * 3 + 1) >> 1) + 0x1ff) >> 9;
    const ud min_sector_count = fat_clusters_sec_ofs - fpp->hidden_sector_count + ((ud)fpp->fcp.cluster_count << fpp->fcp.log2_sectors_per_cluster);
    const ud max_sector_count = min_sector_count + (ud)(1 << fpp->fcp.log2_sectors_per_cluster) - 1;
    /* Rootdir entry count must be a multiple of 0x10.  ; Some DOS msload boot code relies on this (i.e. rounding down == rounding up). */
    if (fpp->fat_fstype != 12 && fpp->fat_fstype != 16 && fpp->fat_fstype != 32) fatal0("ASSERT_BAD_FAT_FSTYPE");
    if ((fpp->fat_count - 1U) > 2U - 1U) fatal0("ASSERT_BAD_FAT_COUNT");
    if ((sd)fpp->fcp.cluster_count <= (fpp->fat_fstype == 32 ? 1 : 0)) fatal0("ASSERT_BAD_CLUSTER_COUNT");  /* We count the root directory cluster in FAT32. */
    if (fpp->fat_fstype == 12 && fpp->fcp.cluster_count > 0xff4) fatal0("TOO_MANY_CLUSTERS_FOR_FAT12");
    if (fpp->fat_fstype == 16 && fpp->fcp.cluster_count > 0xfff4) fatal0("TOO_MANY_CLUSTERS_FOR_FAT16");
    if (fpp->fat_fstype == 32 && fpp->fcp.cluster_count > 0xffffff5) fatal0("TOO_MANY_CLUSTERS_FOR_FAT32");
    if ((sd)fat_fat_sec_ofs < 0 || fat_rootdir_sec_ofs <= fat_fat_sec_ofs || fat_clusters_sec_ofs < fat_rootdir_sec_ofs) fatal0("FAT_TOO_LARGE_BEFORE_CLUSTERS");
    if (fpp->fcp.log2_sectors_per_cluster > 6) fatal0("ASSERT_BAD_SECTORS_PER_CLUSTER");
    if (fpp->fcp.cluster_count > (0xffffffffU >> fpp->fcp.log2_sectors_per_cluster)) fatal0("ASSERT_TOO_MANY_SECTORS_IN_CLUSTERS");  /* !! Use ...UL suffix for >16-bit integer literals. */
    if ((fpp->fcp.cluster_count << fpp->fcp.log2_sectors_per_cluster) > fpp->fcp.sector_count - fat_clusters_sec_ofs) fatal0("ASSERT_TOO_FEW_SECTORS");  /* This can signify an overflow in sector_count calculations. */
    if (fpp->fcp.sectors_per_fat < min_sectors_per_fat) fatal0("ASSERT_BAD_SECTORS_PER_FAT");
    if (fpp->fcp.rootdir_entry_count & 0xf) fatal0("ASSERT_BAD_ROOTDIR_ENTRY_COUNT");
    if (fpp->fat_fstype == 16 && fpp->fcp.cluster_count < 0xff7) fatal0("TOO_FEW_CLUSTERS_FOR_FAT16");
    if (fpp->fat_fstype == 32 && fpp->fcp.cluster_count < 0xfff5) fatal0("TOO_FEW_CLUSTERS_FOR_FAT32");
    if (fpp->fcp.sector_count < min_sector_count) fatal0("TOO_FEW_SECTORS");
    if (fpp->fat_fstype == 12 && fpp->fcp.sector_count > max_sector_count) fatal0("TOO_MANY_SECTORS");  /* Compared to fat12_preset. */
    if (fpp->hidden_sector_count > 0xffffU - fpp->reserved_sector_count) fatal0("TOO_MANY_HIDDEN_SECTORS");  /* It has to fit to a 16-bit word in the FAT header in the MBR. */
    if (fpp->fcp.sectors_per_track == 0 || fpp->hidden_sector_count % fpp->fcp.sectors_per_track) fatal0("BAD_HIDDEN_SECTOR_COUNT_MODULO");  /* MS-DOS <=6.x requires that hidden_sector_count is a multiple of sectors_per_track. */
#  else
    (void)fatal0;  /* !! Remove definition if not used for DEBUG. */
#  endif
  if (fpp->vhd_mode == VHD_FIXED) {
    vhd_sector_count = (fpp->geometry_sector_count + 0x7ffU) & ~0x7ffU;  /* Round up to the nearest MiB, as required by Microsoft Azure. */
#    ifdef DEBUG
      msg_printf("info: vhd_sector_count=%lu=0x%lx\n", (unsigned long)vhd_sector_count, (unsigned long)vhd_sector_count);
#    endif
    set_file_size_scount(vhd_sector_count + 1U);  /* +1U for the VHD footer sector. */
  } else {
    set_file_size_scount(fpp->geometry_sector_count);
  }

  /* Write boot sector containing the FAT header (superblock). */
  memcpy(sbuf, boot_bin + (fpp->fat_fstype == 12 ? BOOT_OFS_FAT12 : fpp->fat_fstype == 16 ? BOOT_OFS_FAT16 : BOOT_OFS_FAT32), 0x200);
  /* .header: jmp strict short .boot_code */
  /* nop  ; 0x90 for CHS. Another possible value is 0x0e for LBA. Who uses it? It is ignored by .boot_code. */
  s = sbuf + 3;  /* More info about FAT12, FAT16 and FAT32: https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system */
  memcpy(s, oem_name, 8); s += 8;
  dw(fat_sector_size);  /* The value 0x200 is hardcoded in boot_sector.boot_code, both explicitly and implicitly. */
  db((ub)1 << fpp->fcp.log2_sectors_per_cluster);
  dw(fpp->reserved_sector_count);
  db(fpp->fat_count);  /* Must be 1 or 2. MS-DOS 6.22 supports only 2. Windows 95 DOS mode supports 1 or 2. */
  dw(fpp->fcp.rootdir_entry_count);  /* Each FAT directory entry is 0x20 bytes. Each sector is 0x200 bytes. */
  dw(fpp->fcp.sector_count - fpp->hidden_sector_count > 0xffffU ? 0 : fpp->fcp.sector_count - fpp->hidden_sector_count);  /* 0 doesn't happen for our FAT12. */
  db(fpp->fcp.media_descriptor);   /* 0xf8 for HDD. 0xf8 is also used for some floppy disk formats as well. */
  dw(fpp->fat_fstype == 32 ? 0 : fpp->fcp.sectors_per_fat);
  /* FreeDOS 1.2 `dir c:' needs a correct value for .sectors_per_track and .head_count. MS-DOS 6.22 and FreeDOS 1.3 ignore these values (after boot). */
  dw(fpp->fcp.sectors_per_track);  /* Track == cylinder. Dummy nonzero value to pacify mtools(1). Here it is not overwritten with value from BIOS int 13h AH == 8. */
  dw(fpp->fcp.head_count);  /* Dummy nonzero value to pacify mtools(1). Here it is not overwritten with value from BIOS int 13h AH == 8. */
  dd(fpp->hidden_sector_count); /* Occupied by MBR and previous partitions. */
  dd(fpp->fcp.sector_count - fpp->hidden_sector_count);
  if (fpp->fat_fstype == 32) {  /* FAT32. */
    dd(fpp->fcp.sectors_per_fat);
    dw(0);  /* mirroring_flags, as created by Linux mkfs.vfat(1). */
    dw(0);  /* .version. */
    dd(2);  /* .rootdir_start_cluster. */
    dw(fpp->reserved_sector_count > 1U ? 1U : 0U);  /* .fsinfo_sec_ofs. */
    dw(first_boot_sector_copy_sec_ofs);
    s += 12;  /* .reserved. The values are 0. */
  }
#ifdef DEBUG
  if (0) {  /* These are already correct in boot_bin. */
    db(0);  /* fat_drive_number. */
    db(0);  /* fat_var_unused. Can be used as a temporary variable in .boot_code. */
    db(EXTENDED_BOOT_SIGNATURE);
  } else {
    s += 3;
  }
#else
  s += 3;
#endif
  dd(fpp->volume_id);
#ifdef DEBUG
  if (0) {  /* These are already correct in boot_bin. */
    memcpy(s, "NO NAME    ", 11); s += 11;  /* volume_label. */
    memcpy(s, fpp->fat_count == 12 ? "FAT12   " : fpp->fat_count == 16 ? "FAT16   " : "FAT132   ", 8);  /* fstype. */
  }
#endif
  if (fpp->fat_fstype == 12) {
    s = sbuf + gw(boot_bin + BOOT_OFS_FAT12_OFSS + 0); dw(fat_clusters_sec_ofs);
    s = sbuf + gw(boot_bin + BOOT_OFS_FAT12_OFSS + 2); dw(fpp->fcp.rootdir_entry_count);
    s = sbuf + gw(boot_bin + BOOT_OFS_FAT12_OFSS + 4); dw(fat_rootdir_sec_ofs);
    s = sbuf + gw(boot_bin + BOOT_OFS_FAT12_OFSS + 6); dw(fat_fat_sec_ofs);
  }
  write_sector(fpp->hidden_sector_count);
  if (first_boot_sector_copy_sec_ofs) write_sector(fpp->hidden_sector_count + first_boot_sector_copy_sec_ofs);

  if (fpp->hidden_sector_count) {  /* Write the MBR. */
    s = sbuf;
    memcpy(s, boot_bin + BOOT_OFS_MBR, 3);  /* The jump instruction to the MBR boot code. */
    if (fpp->fat_fstype != 32) memset(s + 0x3e, '-', 0x5a - 0x3e);
    memcpy(s + 0x5a, boot_bin + BOOT_OFS_MBR + 0x5a, 0x200 - 0x5a);
    s += 0xe; dw(fpp->hidden_sector_count + fpp->reserved_sector_count);  /* reserved_sector_count. */
    s += 3; dw(fpp->fcp.sector_count > 0xffffU ? 0 : fpp->fcp.sector_count);
    s += 0x18 - 0x15; dw(1);  /* sectors_per_track. Dummy value for Mtools to prevent the ``Total number of sectors (...) not a multiple of sectors per track (63)!'' error. This is unnecessary with the current adjust_geometry(...). */
    s += 0x1c - 0x1a; dd(0);  /* hidden_sector_count. */
    dd(fpp->fcp.sector_count);
    if (fpp->fat_fstype == 32) {
      s += 0x30 - 0x24;
      dw(fpp->hidden_sector_count + (fpp->reserved_sector_count ? 1U : 0U));  /* .fsinfo_sec_ofs. */
      dw(fpp->hidden_sector_count + first_boot_sector_copy_sec_ofs);
    }
    s = sbuf + 0x1be;  /* Partition 1. */
    db(PSTATUS_ACTIVE);  /* Status (PSTATUS.*: 0: inactive, 0x80: active (bootable)). */
    db(0);  /* CHS head of first sector. */
    dw(0);  /* CHS cylinder and sector of first sector. */
    db(fpp->fat_fstype == 32 ? PTYPE_FAT32_LBA : fpp->fcp.sector_count >> 16 ? PTYPE_FAT16 : PTYPE_FAT16_LESS_THAN_32MIB);  /* Partition type (PTYPE.*). */
    db(0);  /* CHS head of last sector. */
    dw(0);  /* CHS cylinder and sector of last sector. */
    dd(fpp->hidden_sector_count);  /* Sector offset (LBA) of the first sector. */
    dd(fpp->fcp.sector_count - fpp->hidden_sector_count);  /* Number of sectors. */
    write_sector(0);
  }

  if (fpp->fat_fstype == 32 && fpp->reserved_sector_count > 1U) {  /* Write fsinfo sector for FAT32. https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system#FS_Information_Sector */
    memset(s = sbuf, 0, sizeof(sbuf));
    dd('R' | 'R' << 8 | (ud)'a' << 16 | (ud)'A' << 24);  /* .header. */
    s += 0x1e0;  /* .reserved. The values are 0. */
    dd('r' | 'r' << 8 | (ud)'A' << 16 | (ud)'a' << 24);  /* .signature2. */
    dd(fpp->fcp.cluster_count - 1);  /* .free_cluster_count. -1 because the root directory occupies 1 cluster. */
    dd(2);  /* .most_recently_allocated_cluster_ofs. The root directory cluster. */
    s += 0xc + 2;  /* .reserved2 and first 2 bytes of .signature3. The values are 0. */
    dw(BOOT_SIGNATURE);
    write_sector(fpp->hidden_sector_count + 1U);
  }

  /* Write the first sector of each FAT. */
  memset(s = sbuf, 0, sizeof(sbuf));
  db(fpp->fcp.media_descriptor);
  dw(-1);
  if (fpp->fat_fstype == 16) {
    db(-1);
  } else if (fpp->fat_fstype == 32) {
    db(0xf);  /* First special cluster pointer: 0xffffff8, the low byte being the media_descriptor. */
    dd(0xfffffff);  /* Second special cluster pointer. */
    dd(0xffffff8);  /* Indicates empty root directory. */
  }
  write_sector(fat_fat_sec_ofs);
  if (fpp->fat_count > 1) write_sector(fat_fat_sec_ofs + fpp->fcp.sectors_per_fat);

  /* Write the VHD footer. */
  if (fpp->vhd_mode == VHD_FIXED) {
    /* VHD (.vhd) is the virtual hard disk (virtual HDD) file format
     * introduced by Connectix Virtual PC (now Microsoft Vitual PC). It has
     * many subformats, We use the fixed-size subformat. (Using the dynamic
     * subformat instead would make it possible to create sparse disk images
     * on FAT filesystems, with the default block size of 2 MiB.)
     *
     * File format docs: https://github.com/libyal/libvhdi/blob/main/documentation/Virtual%20Hard%20Disk%20(VHD)%20image%20format.asciidoc
     *
     * Docs about QEMU disk image file format support: https://qemu-project.gitlab.io/qemu/system/images.html
     *
     * VirtualBox 5.2.42 and QEMU 2.11.1 don't have the 127 GiB limit on
     * their virtual IDE controller: using EBIOS LBA (int 13h, AH == 42h),
     * all VHD sectors up to 2040 GiB can be accessed. The maximum sector
     * count with CHS access (int 13h, AH == 2h) is 1024 * 255 * 63 (==
     * 8032.5 MiB), sectors beyond that can't be accessed. QEMU 2.11.1 has a
     * bug in its BIOS disk geometry reporting (int 13h, AH == 8): it
     * reports 1 less than the actual number of cylinders -- but it can
     * still access the last cylinder.
     *
     * https://kb.msp360.com/standalone-backup/restore/vhd-disk-size-exceeded
     * says about MSP360 (Formerly CloudBerry) Backup for Windows: The
     * maximum size for a virtual hard disk is 2,040 gigabytes (GB).
     * However, any virtual hard disk attached to the IDE controller cannot
     * exceed 127 gigabytes (GB). [...] VHDX can handle up to 64 TiB.
     *
     * https://serverfault.com/a/770425 says: When QEMU creates a VHD image,
     * it goes by the original spec, calculating the current_size based on
     * the nearest CHS geometry (with an exception for disks > 127GB).
     * Apparently, Azure will only allow images that are sized to the
     * nearest MB, and the current_size as calculated from CHS cannot
     * guarantee that. Allow QEMU to create images similar to how Hyper-V
     * creates images, by setting current_size to the specified virtual disk
     * size. This introduces an option, force_size, to be passed to the vpc
     * format during image creation.
     *
     * From the QEMU 2.11.1 source file qemu-2.11.1/block/vpc.c: Microsoft
     * Virtual PC and Microsoft Hyper-V produce and read VHD image sizes
     * differently. VPC will rely on CHS geometry, while Hyper-V and
     * disk2vhd use the size specified in the footer.
     *
     * Example conversion command from raw (.img) to VHD (.vhd): `qemu-img convert -f raw -o subformat=fixed,force_size -O vpc hd.img hd.vhd`
     *
     * QEMU 2.11.1 int 13h AH == 8 has a bug: it reports 1 less cyls (for
     * both raw .img and .vhd); VirtualBox does it correctly.
     *
     * QEMU 2.11.1 autodetects the disk geometry (incorrectly) even if
     * specified in the .vhd file
     *
     * VirtualBox respects the disk geometry in the .vhd file up to the max
     * of C*H*S == 1024*16*63 =~ 504 MiB; above 1024 cyls it starts doing
     * transformations.
     */
    memset(s = sbuf, 0, sizeof(sbuf));
    memcpy(s, "conectix", 8); s += 8;  /* signature. */
    ddb(2);  /* features. Just the reserved bit is set. */
    ddb(0x10000);  /* format_version. */
    dd(-1);  /* next_offset high dword. */
    dd(-1);  /* next_offset low dword. */
    dd(0);  /* modification_time. */
    dd(fpp->geometry_sector_count > (ud)65535U * 16U * 255U ? (ud)('w' | 'i' << 8 | (ud)'n' << 16 | (ud)' ' << 24) :
       (ud)('v' | 'p' << 8 | (ud)'c' << 16 | (ud)' ' << 24));  /* creator_application: Typically "qemu" (CHS), "vpc " (CHS), "qem2" (force_size), "win " (force_size). */
    ddb(0x50003);  /* creator_version. */
    dd('W' | 'i' << 8 | (ud)'2' << 16 | (ud)'k' << 24);  /* host_os. */
    dsb(vhd_sector_count);  /* disk_size. */
    dsb(vhd_sector_count);  /* data_size. */
    if (fpp->geometry_sector_count <= (ud)1024U * 16U * 63U) {  /* Compatible with bos BIOS and IDE, <= 504 MiB. */
      dwb((fpp->geometry_sector_count + (16U * 63U - 1U)) / (16U * 63U));  /* disk_geometry.cyls. */  /* Round up. */
      db(16U);  /* disk_geometry.heads. */
      db(63U);  /* disk_geometry.secs. */
    } else if (fpp->geometry_sector_count >= (ud)65535U * 16U * 255U) {
      dwb(65535U);  /* disk_geometry.cyls. */
      db(16U);  /* disk_geometry.heads. */
      db(255U);  /* disk_geometry.secs. */
    } else {
      dwb((fpp->geometry_sector_count + (16U * 255U - 1U)) / (16U * 255U));  /* disk_geometry.cyls. */  /* Round up. */
      db(16U);  /* disk_geometry.heads. */
      db(255U);  /* disk_geometry.secs. */
    }
    ddb(2);  /* disk_type: VHD_FIXED --> 2, VHD_DYNAMIC --> 3. */
    dd(0);  /* checksum. On mismatch, QEMU reports a warning. */
    dd(fpp->volume_id);  /* First 4 bytes of identifier: 16-byte big-endian UUID. */
    dd(fpp->geometry_sector_count);  /* Next 4 bytes of identifier. */
    db(fpp->fat_fstype + (fpp->fat_count - 1U));  /* Next 1 byte of identifier. */
    /* VirtualBox doesn't allow adding a disk with the same UUID, so adding
     * two disk images created with bakefat (without the RNDUUID
     * command-line flag) won't work. QEMU doesn't have such a limitation.
     * */
    memcpy(s, "\xb5\xd4\x99\xe3\xbc\x63\x46", 7); s += 7;  /* identifier: Big endian UUID. */
    /*db(0);*/  /* saved_state. Trailing NUL bytes can be unspecified. */
    for (checksum = (ud)-1; s != sbuf; checksum -= *(const ub*)--s) {}
    s = sbuf + 0x40; ddb(checksum);
    write_sector(vhd_sector_count);
  }
}

static ud align_fat(struct fat_params *fpp, ud fat_clusters_sec_ofs) {
  ud sector_delta = 0;
  if (fpp->fcp.log2_sectors_per_cluster >= 3U && (fat_clusters_sec_ofs & 7U)) {  /* Simple alignment: align clusters to a multiple of 4K. */  /* !! Do better alignment, even for the FAT table and the root directory. */
    sector_delta = -fpp->fcp.log2_sectors_per_cluster & 7U;
    fpp->reserved_sector_count += sector_delta;
  }
  return sector_delta;
}

/* Adjust geometry for rounding in QEMU 2.11.1.
 *
 * Inputs: fpp->fcp.sector_count, fpp->fcp.cluster_count, fpp->fcp.log2_sectors_per_cluster, fpp->fat_fstype.
 * Outputs: fpp->geometry_sector_count, fpp->fcp.sector_count, fpp->fcp.cluster_count, fpp->cylinder_count (cyls), fpp->head_count (heads), fpp->sectors_per_track (secs).
 */
static void adjust_hdd_geometry(struct fat_params *fpp, ud fat_clusters_sec_ofs) {
  ud cyls, heads, hs;
  ub mod;
  fpp->fcp.sectors_per_track = 63U;
  fpp->fcp.head_count = heads =
      (fpp->fcp.sector_count <= (ud)1024U * 16U * 63U) ? 16U :
      (fpp->fcp.sector_count <= (ud)2048U * 32U * 63U) ? 32U :
      (fpp->fcp.sector_count <= (ud)4096U * 64U * 63U) ? 64U :
      (fpp->fcp.sector_count <= (ud)8192U * 128U * 63U) ? 128U : 255U;
  hs = fpp->fcp.head_count * 63U;
  cyls = fpp->fcp.sector_count == 0U ? (ud)0 : (fpp->fcp.sector_count - 1U) / hs + 1U;  /* This is round_up_div(fpp->fcp.sector_count, hs), but avoids overflow. */
  fpp->cylinder_count = cyls =
      (heads == 32U && cyls < 543U) ? 543U :
      (heads == 64U && cyls < 527U) ? 527U :
      (heads == 128U && cyls < 519U) ? 519U :
      (heads == 255U && cyls < 517U) ? 517U : cyls;
  fpp->geometry_sector_count = cyls * (fpp->fcp.head_count * 63U);
#  ifdef DEBUG
    msg_printf("info: geometry: sector_count=0x%lx geometry_sector_count=0x%lx CHS=%lu:%u:%u\n", (unsigned long)fpp->fcp.sector_count, (unsigned long)fpp->geometry_sector_count, (unsigned long)fpp->cylinder_count, (unsigned)fpp->fcp.head_count, (unsigned)fpp->fcp.sectors_per_track);
    if (fpp->fcp.sector_count > fpp->geometry_sector_count) fatal0("ASSERT_GEOMETRY_BAD_SECTOR_COUNT");
#  endif
 fix_sector_count:
  mod = fpp->fcp.sector_count % 63U;
  if (mod) {  /* Mtools requires sector_count-hidden_sector_count to be a multiple of sectors_per_track. */
    /* Here we can choose to round fpp->fcp.sector_count up or down, thus
     * keeping (up or down), increasing (up) or decreasing (down)
     * fpp->fcp.cluster_count.
     */
    hs = fpp->fcp.sector_count - fat_clusters_sec_ofs;
    if (((hs + (63U - mod)) >> fpp->fcp.log2_sectors_per_cluster) == (hs >> fpp->fcp.log2_sectors_per_cluster)) {
#      ifdef DEBUG
        msg_printf("info: geometry: rounding up without cluster count increase\n");
#      endif
     simple_round_up:  /* Rounding up doesn't increase fpp->fcp.cluster_count, so we round up. */
      fpp->fcp.sector_count += 63U - mod;
    } else if (((hs - mod) >> fpp->fcp.log2_sectors_per_cluster) < (fpp->fat_fstype == 16 ? 0xff7U : 0xfff5U)) {  /* Rounding down would make the filesystem too small, so we round up, possibly increasing fpp->fcp.sectors_per_fat. */
      /* This affects FAT16 2M, FAT32 32M, FAT32 64M. */
      hs = fpp->fcp.sector_count + 63U - mod;
      mod = fpp->fcp.log2_sectors_per_cluster;
      heads = (hs - fat_clusters_sec_ofs) >> mod;
      if ((fpp->fcp.sectors_per_fat << (8 - (fpp->fat_fstype == 32))) - 2U < heads) {  /* The new fpp->fcp.cluster_count doesn't fit in the old FAT table. */
        /* This affects FAT16 2M, FAT32 32M, FAT32 64M. */
        ++fpp->fcp.sectors_per_fat;
        fat_clusters_sec_ofs += fpp->fat_count;
        fat_clusters_sec_ofs += align_fat(fpp, fat_clusters_sec_ofs);  /* Realign because fat_clusters_sec_ofs has changed. Also ets fpp->fcp.sector_count. */
        fpp->fcp.sector_count = fat_clusters_sec_ofs + (fpp->fcp.cluster_count << mod);
#        ifdef DEBUG
          msg_printf("info: geometry: rounding up with sectors-per-fat increase: sector_count=0x%lx\n", (unsigned long)fpp->fcp.sector_count);
#        endif
        goto fix_sector_count;
      }
      /* TODO(pts): Add tests. This is never reached. */
#      ifdef DEBUG
        msg_printf("info: geometry: rounding up with cluster count increase: new cluster_count=0x%lx sector_count=0x%lx\n", (unsigned long)heads, (unsigned long)hs);
#      endif
      fpp->fcp.cluster_count = heads;
      fpp->fcp.sector_count = hs;
    } else {  /* Round down, decreasing fpp->fcp.cluster_count and maybe decreasing fpp->fcp.sector_count. */
      /* This affects most size configurations. */
      fpp->fcp.sector_count -= mod;
      mod = fpp->fcp.log2_sectors_per_cluster;
      fpp->fcp.cluster_count = (fpp->fcp.sector_count - fat_clusters_sec_ofs) >> mod;
      fpp->fcp.sector_count = fat_clusters_sec_ofs + (fpp->fcp.cluster_count << mod);
#      ifdef DEBUG
        msg_printf("info: geometry: rounding down: new cluster_count=0x%lx sector_count=0x%lx\n", (unsigned long)fpp->fcp.cluster_count, (unsigned long)fpp->fcp.sector_count);
#      endif
      mod = fpp->fcp.sector_count % 63U;
      if (mod) goto simple_round_up;
    }
  }
#  ifdef DEBUG
    if (fpp->fcp.cluster_count > (fpp->fat_fstype == 16 ? 0xfff4U : 0xffffff5U)) fatal0("ASSERT_TOO_MANY_CLUSTERS_AFTER_ROUNDING");
    if (fpp->fcp.sector_count > fpp->geometry_sector_count) fatal0("ASSERT_GEOMETRY_BAD_FINAL_SECTOR_COUNT");
    if (fpp->fcp.sector_count % 63U) fatal0("ASSERT_GEOMETRY_SECTOR_COUNT_MODULO_SECS");
#  endif
}

static ub is_aligned_fat32_sector_count_at_most(const struct fat_params *fpp, ud fat_cluster_count) {
  struct fat_params fp = *fpp;  /* This is a memcpy(). */
  ud fat_clusters_sec_ofs, fat_sector_in_cluster_count;
  fp.fcp.cluster_count = fat_cluster_count;
  fp.fcp.sectors_per_fat = (fp.fcp.cluster_count + (2U + 0x7fU)) >> 7U;
  fat_clusters_sec_ofs = fp.hidden_sector_count + fp.reserved_sector_count + ((ud)fp.fcp.sectors_per_fat << (fp.fat_count - 1U)) /* + (fp.fcp.rootdir_entry_count >> 4U) */;
  fat_clusters_sec_ofs += align_fat(&fp, fat_clusters_sec_ofs);
  fat_sector_in_cluster_count = fp.fcp.cluster_count << fp.fcp.log2_sectors_per_cluster;
  return fat_sector_in_cluster_count <= fp.fcp.sector_count && fat_clusters_sec_ofs <= fp.fcp.sector_count - fat_sector_in_cluster_count;
}

typedef char assert_ud_unsigned[(ud)-1 > 0 ? 1 : -1];
typedef char assert_ud_overflow_wrap[(ud)-1 + 1 == 0 ? 1 : -1];

typedef enum parseint_error_t {
  PARSEINT_OK = 0,
  PARSEINT_BAD_PREFIX = 1,
  PARSEINT_BAD_CHAR = 2,
  PARSEINT_OVERFLOW = 3,
  PARSEINT_TOO_SHORT = 4,  /* For parse_volume_id(...). */
  PARSEINT_NULL = 5,
} parseint_error_t;

/* Parses a C unsigned decimal, hexadecimal, octal literal, sets *result_ptr
 * to a result. Returns PARSEINT_OK == 0 on success.
 */
static parseint_error_t parse_ud(const char *s, ud *result_ptr) {
  ud u, limit;
  unsigned char base, digit;
  parseint_error_t result;
  if (!s) return PARSEINT_NULL;
#if '0' + 9 == '9' && 'a' + 25 == 'z' && 'A' + 25 == 'Z'
#else  /* e.g. EBCDIC. */
#  error Alphanumeric characters code must be consective.
#endif
#if ('A' | 32) == 'a'  /* ASCII. */
  if (s[0] == '0' && (s[1] | 32) == 'x')
#else
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
#endif
  {
    base = 16;
    limit = (ud)-1 / 16U;
    s += 2;
  } else if (s[0] == '0') {
    base = 8;
    limit = (ud)-1 / 8U;
    ++s;
  } else if ((s[0] - ('1' + 0U)) <= 9U - 1U) {
    base = 10;
    limit = (ud)-1 / 10U;
  } else {
    return PARSEINT_BAD_PREFIX;
  }
  for (u = 0, result = PARSEINT_OK; *s != '\0'; ++s) {
    digit = *s;
#if ('A' | 32) == 'a'  /* ASCII. */
    digit = (base == 16 && (digit | 32U) - ('a' + 0U) < 6U) ? (digit | 32U) - ('a' - 10U) : digit - ('0' + 0U);
#else
    digit = (base == 16 && (digit) - ('a' + 0U) < 6U) ? (digit - ('a' - 10U)) : (base == 16 && (digit) - ('A' + 0U) < 6U) ? digit - ('A' - 10U) : digit - ('0' + 0U);
#endif
    if (digit >= base) return PARSEINT_BAD_CHAR;
    if (u > limit) result = PARSEINT_OVERFLOW;
    u *= base;
    if (u + digit < u) result = PARSEINT_OVERFLOW;
    u += digit;
  }
  *result_ptr = u;
  return result;
}

/* Parses a FAT volume ID (8-digit hexadecimal, with a hyphen (dash, -) in
 * the middle), sets *result_ptr to a result. The parser is lenient on the
 * hyphens: it allows them anywhere. Returns PARSEINT_OK == 0 on success.
 */
static parseint_error_t parse_volume_id(const char *s, ud *result_ptr) {
  ud u;
  ub count, digit;
  for (u = 0, count = 8; *s != '\0'; ++s) {
    if ((digit = *s) == '-') continue;
    if (!count) return PARSEINT_OVERFLOW;
    digit = (digit | 32U) - ('a' + 0U) < 6U ? (digit | 32U) - ('a' - 10U) : digit - ('0' + 0U);
    if (digit > 0xfU) return PARSEINT_BAD_CHAR;
    --count;
    u <<= 4U;
    u |= digit;
  }
  if (count != 0) return PARSEINT_TOO_SHORT;
  *result_ptr = u;
  return PARSEINT_OK;
}

static noreturn void usage(ub is_help, const char *argv0) {
  char *p = sbuf;  /* TODO(pts): Check for overflow below. */
  const char **csp;
  const char *q, *cluster_size_flags, *hdd_image_size_flags;
  const struct fat12_preset *prp;
  /* TODO(pts): Precompute this help message, making the code shorter. */
  for (prp = fat12_presets; prp != ARRAY_END(fat12_presets); ++prp) {
    for (q = prp->name, *p++ = ' '; *q != '\0'; *p++ = *q++) {}
  }
  *p++ = '\0';
  hdd_image_size_flags = p;
  for (csp = hdd_size_presets_m_21; csp != ARRAY_END(hdd_size_presets_m_21); ++csp) {
    if (strcmp(*csp, "1024M") == 0) break;
    for (q = *csp, *p++ = ' '; *q != '\0'; *p++ = *q++) {}
  }
  for (csp = hdd_size_presets_g_30; csp != ARRAY_END(hdd_size_presets_g_30); ++csp) {
    if (strcmp(*csp, "1024G") == 0) break;
    for (q = *csp, *p++ = ' '; *q != '\0'; *p++ = *q++) {}
  }
  for (csp = hdd_size_presets_t_40; csp != ARRAY_END(hdd_size_presets_t_40); ++csp) {
    for (q = *csp, *p++ = ' '; *q != '\0'; *p++ = *q++) {}
  }
  *p++ = '\0';
  cluster_size_flags = p;
  for (csp = sectors_per_cluster_presets_k_10; csp != ARRAY_END(sectors_per_cluster_presets_k_10); ++csp) {
    for (q = *csp, *p++ = ' '; *q != '\0'; *p++ = *q++) {}
  }
  *p = '\0';
  /* This help message doesn't contain some alternate spellings of some flags. */
  msg_printf("bakefat: bootable external FAT disk image creator v%d\n"
             "Usage: %s <flag> [...] <outfile.img>\n"
             "Floppy image size flags:%s\n"
             "HDD image size flags:%s\n"
             "Cluster size flags: 512B%s\n%s%s",
             BAKEFAT_VERSION, argv0, sbuf, hdd_image_size_flags, cluster_size_flags,
             "Filesystem type flags: FAT12 FAT16 FAT32\n"
             "FAT count flags: 1FAT 2FATS FC=<number>\n"
             "Root directory entry count: RDEC=<number>\n"
             "Reserved sector count: RSC=<number>\n"
             "Volume ID: VID=<hex-with-hyphen>\n",
             "DOS compatibility flags: DOS3 DOS3.3 DOS4 DOS5 DOS6 DOS7 DOS7.0 DOS7.1 MSDOS7.0 MSDOS7.1 PCDOS7.0 PCDOS7.1 DOS8 WIN95A WIN95OSR2 WIN98 WINME\n"
             "VHD footer flags: NOVHD VHD\n");
  exit(is_help ? 0 : 1);
}

static noreturn void bad_usage0(const char *msg) {
  msg_printf("fatal: %s\n", msg);
  exit(1);
}

static noreturn void bad_usage1(const char *msg, const char *arg) {
  msg_printf("fatal: %s: %s\n", msg, arg);
  exit(1);
}

int main(int argc, char **argv) {
  const char **arg, **arge, **argfn = NULL;
  const char *flag;
  ub is_help;
  signed char log2_size = 0;  /* Unspecified. -1 means FAT12 preset. */
  const struct fat12_preset *prp;
  const char **csp;
  struct fat_params fp;
  int min_log2_spc, max_log2_spc;
  uw old_sectors_per_fat;
  ud fat_clusters_sec_ofs;
  ud hi, lo, mid;
  ud u;
  ub b;
  ub had_volume_id;

  (void)argc;
#  ifdef __MMLIBC386__
  stdout_fd = STDERR_FILENO;  /* For msg_printf(...). */
#  endif
  memset(&fp, '\0', sizeof(fp));
  fp.default_log2_sectors_per_cluster = fp.fcp.log2_sectors_per_cluster = (ub)-1;  /* Unspecified. */
  had_volume_id = 0;
  is_help = argv[1] && (strcasecmp(argv[1], "--help") == 0 || (!argv[2] && strcasecmp(argv[1], "help") == 0));
  for (arge = (const char **)argv + 1; ; ++arge) {
    if (!*arge) {  /* The last argument is the output image file name (<outfile.img>). */
      argfn = --arge;
      if ((char**)argfn != argv && argfn[0][0] == '-') argfn = ++arge;
      break;
    }
    if (strcmp(*arge, "--") == 0) {
      argfn = arge + 1;
      break;
    }
  }
  if (is_help || (char**)argfn == argv) usage(is_help, argv[0]);
  for (arg = (const char **)argv + 1; arg != arge; ++arg) {
    for (flag = *arg; *flag == '-' || *flag == '/'; ++flag) {}  /* Skip leading - and / characters in flag. */
    for (prp = fat12_presets; prp != ARRAY_END(fat12_presets); ++prp) {
      if (strcasecmp(flag, prp->name) == 0) {
        if (fp.fat_fstype && fp.fat_fstype != 12) goto error_conflicting_fat_fstype;
        if (log2_size && (log2_size != -1 || fp.fcp.sector_count != prp->fcp.sector_count)) { error_conflicting_size:
          bad_usage0("conflicting image sizes specified");
        }
        log2_size = -1;
        u = fp.fcp.rootdir_entry_count;  /* Save. */
        b = fp.fcp.log2_sectors_per_cluster;  /* Save. */
        fp.fcp = prp->fcp;  /* This is a memcpy(). */
        fp.default_fat_count = 2;
        fp.default_reserved_sector_count = 1;
        fp.default_rootdir_entry_count = fp.fcp.rootdir_entry_count;
        fp.default_log2_sectors_per_cluster = fp.fcp.log2_sectors_per_cluster;
        fp.fcp.rootdir_entry_count = u;  /* Restore. */
        fp.fcp.log2_sectors_per_cluster = b;  /* Restore. */
        fp.fat_fstype = 12;
        goto next_flag;
      }
    }
    for (csp = hdd_size_presets_m_21; csp != ARRAY_END(hdd_size_presets_m_21); ++csp) {
      if (strcasecmp(flag, *csp) == 0) { b = csp - hdd_size_presets_m_21 + 21; set_size: if (log2_size && (ub)log2_size != b) goto error_conflicting_size; log2_size = b; goto next_flag; }
    }
    for (csp = hdd_size_presets_g_30; csp != ARRAY_END(hdd_size_presets_g_30); ++csp) {
      if (strcasecmp(flag, *csp) == 0) { b = csp - hdd_size_presets_g_30 + 30; goto set_size; }
    }
    for (csp = hdd_size_presets_t_40; csp != ARRAY_END(hdd_size_presets_t_40); ++csp) {
      if (strcasecmp(flag, *csp) == 0) { b = csp - hdd_size_presets_t_40 + 40; goto set_size; }
    }
    for (csp = sectors_per_cluster_presets_b_9; csp != ARRAY_END(sectors_per_cluster_presets_b_9); ++csp) {
      if (strcasecmp(flag, *csp) == 0) {
        b = csp - sectors_per_cluster_presets_b_9 + 9 - 9;
        if (fp.fcp.log2_sectors_per_cluster != (ub)-1 && fp.fcp.log2_sectors_per_cluster != b) { error_conflicting_spc:
          bad_usage0("conflicting sectors-per-cluster specified");
        }
        fp.fcp.log2_sectors_per_cluster = b;
        goto next_flag;
      }
    }
    for (csp = sectors_per_cluster_presets_s_9; csp != ARRAY_END(sectors_per_cluster_presets_s_9); ++csp) {
      if (strcasecmp(flag, *csp) == 0) { if (fp.fcp.log2_sectors_per_cluster != (ub)-1) goto error_conflicting_spc; fp.fcp.log2_sectors_per_cluster = csp - sectors_per_cluster_presets_s_9 + 9 - 9; goto next_flag; }
    }
    for (csp = sectors_per_cluster_presets_k_10; csp != ARRAY_END(sectors_per_cluster_presets_k_10); ++csp) {
      if (strcasecmp(flag, *csp) == 0) { if (fp.fcp.log2_sectors_per_cluster != (ub)-1) goto error_conflicting_spc; fp.fcp.log2_sectors_per_cluster = csp - sectors_per_cluster_presets_k_10 + 10 - 9; goto next_flag; }
    }
    if (strcasecmp(flag, "FAT12") == 0) {
      if (fp.fat_fstype && fp.fat_fstype != 12) { error_conflicting_fat_fstype:
        bad_usage0("conflicting FAT type flags specified");
      }
      fp.fat_fstype = 12;
    } else if (strcasecmp(flag, "FAT16") == 0) {
      if (fp.fat_fstype && fp.fat_fstype != 16) goto error_conflicting_fat_fstype;
      fp.fat_fstype = 16;
    } else if (strcasecmp(flag, "FAT32") == 0) {
      if (fp.fat_fstype && fp.fat_fstype != 32) goto error_conflicting_fat_fstype;
      fp.fat_fstype = 32;
    } else if (strncasecmp(flag, "RDEC=", 5) == 0) {
      if (parse_ud(flag + 5, &u) != PARSEINT_OK) goto error_invalid_integer;
      if (u - 1U > 0xfff0U - 1U) bad_usage0("root directory entry count must be between 1 and 65520");
      if (fp.fcp.rootdir_entry_count && fp.fcp.rootdir_entry_count != u) {
        bad_usage0("conflicting root directory entry counts specified");
      }
      fp.fcp.rootdir_entry_count = u;
    } else if (strncasecmp(flag, "RSC=", 4) == 0) {
      if (parse_ud(flag + 4, &u) != PARSEINT_OK) goto error_invalid_integer;
      if (u - 1U > 0xffffU - 1U) bad_usage0("reserved sector count must be between 1 and 65535");
      if (fp.reserved_sector_count && fp.reserved_sector_count != u) {
        bad_usage0("conflicting reserved sector counts specified");
      }
      fp.reserved_sector_count = u;
    } else if (strncasecmp(flag, "VID=", 4) == 0) {
      if (parse_volume_id(flag + 4, &u) != PARSEINT_OK) bad_usage1("invalid FAT volume ID in flag", flag);
      if (had_volume_id && fp.volume_id != u) bad_usage0("conflicting FAT volume IDs specified");
      fp.volume_id = u;
      had_volume_id = 1;
    } else if (strncasecmp(flag, "FC=", 3) == 0) {
      if (parse_ud(flag + 3, &u) != PARSEINT_OK) { error_invalid_integer:
        bad_usage1("invalid integer in flag", flag);
      }
      if (u - 1U > 2U - 1U) bad_usage0("FAT FAT count must be 1 or 2");
      if (fp.fat_count && fp.fat_count != u) { error_conflicting_fat_count:
        bad_usage0("conflicting FAT FAT counts specified");
      }
      fp.fat_count = u;
    } else if (strcasecmp(flag, "1FAT") == 0 || strcasecmp(flag, "1F") == 0) {
      if (fp.fat_count && fp.fat_count != 1) goto error_conflicting_fat_count;
      fp.fat_count = 1;
    } else if (strcasecmp(flag, "2FATS") == 0 ||strcasecmp(flag, "2F") == 0) {
      if (fp.fat_count && fp.fat_count != 2) goto error_conflicting_fat_count;
      fp.fat_count = 2;
    } else if (strcasecmp(flag, "NOVHD") == 0) {
      if (fp.vhd_mode && fp.vhd_mode != VHD_NOVHD) { error_conflicting_vhd_mode:
        bad_usage0("conflicting VHD values specified");
      }
      fp.vhd_mode = VHD_NOVHD;
    } else if (strcasecmp(flag, "VHD") == 0) {
      if (fp.vhd_mode && fp.vhd_mode != VHD_FIXED) goto error_conflicting_vhd_mode;
      fp.vhd_mode = VHD_FIXED;
    } else if (strcasecmp(flag, "DOS3") == 0 || strcasecmp(flag, "DOS3.3") == 0) {
      fp.os_compat |= OSC_DOS3;
    } else if (strcasecmp(flag, "DOS4") == 0) {
      fp.os_compat |= OSC_DOS4;
    } else if (strcasecmp(flag, "DOS5") == 0 || strcasecmp(flag, "DOS6") == 0) {
      fp.os_compat |= OSC_DOS5_6;
    } else if (strcasecmp(flag, "DOS7") == 0) {
      fp.os_compat |= OSC_MSDOS70 | OSC_MSDOS71_8 | OSC_PCDOS70 | OSC_PCDOS71;
    } else if (strcasecmp(flag, "DOS7.0") == 0) {
      fp.os_compat |= OSC_MSDOS70 | OSC_PCDOS70;
    } else if (strcasecmp(flag, "DOS7.1") == 0) {
      fp.os_compat |= OSC_MSDOS71_8 | OSC_PCDOS71;
    } else if (strcasecmp(flag, "PCDOS7.0") == 0) {
      fp.os_compat |= OSC_PCDOS70;
    } else if (strcasecmp(flag, "PCDOS7.1") == 0) {
      fp.os_compat |= OSC_PCDOS71;
    } else if (strcasecmp(flag, "MSDOS7.0") == 0 || strcasecmp(flag, "WIN95A") == 0 || strcasecmp(flag, "WIN95RTM") == 0) {
      fp.os_compat |= OSC_MSDOS70;
    } else if (strcasecmp(flag, "MSDOS7.1") == 0 || strcasecmp(flag, "DOS8") == 0 || strcasecmp(flag, "WIN95OSR2") == 0 || strcasecmp(flag, "WIN98") == 0 || strcasecmp(flag, "WINME") == 0) {
      fp.os_compat |= OSC_MSDOS71_8;
    } else {
      msg_printf("fatal: unknown command-line flag: %s\n", flag);
      exit(1);
    }
   next_flag: ;
  }
  if (!*argfn) bad_usage0("output filename not specified");
  if (argfn[1]) bad_usage0("multiple output filenames specified");
  sfn = *argfn;

  if (!had_volume_id) fp.volume_id = 0x1234abcd;
  if (!fp.fat_fstype) {  /* Autodetect. */
    fp.fat_fstype = log2_size < 0 ? 12 : log2_size <= 31 ? 16 : 32;  /* Use FAT16 for up to 2 GiB, use FAT32 for anything larger. FAT16 doesn't support more than 2 GiB. */
  }
  if (!fp.fat_count) {  /* Autodetect. */
    if ((fp.fat_count = fp.default_fat_count) == 0) {
      fp.fat_count = fp.fat_fstype == 32 ? 1 : 2;  /* 2 for compatibility with MS-DOS <=6.22. */
    }
  }
  if (!fp.vhd_mode) fp.vhd_mode = (log2_size < 0) ? VHD_NOVHD : VHD_FIXED;  /* Autodetect. */
  if (!fp.reserved_sector_count) {  /* Autodetect. */
    if ((fp.reserved_sector_count = fp.default_reserved_sector_count) == 0) {
      fp.reserved_sector_count = fp.fat_fstype == 32 ? 17 : 1;  /* 17 for compatibility with the Windows XP FAT32 boot sector code (written during Windows XP installation), which loads additional boot code from sector 8. */
    }
  }
  if (!fp.fcp.rootdir_entry_count) {
    if ((fp.fcp.rootdir_entry_count = fp.default_rootdir_entry_count) == 0) {  /* Autodetect. */
      fp.fcp.rootdir_entry_count = 128;  /* !! Maybe 256? Look at alignment. */
      if (fp.os_compat & OSC_DOS3) fp.fcp.rootdir_entry_count = 512;  /* For compatibility with DOS 3.30. It can read and write a filesystem with another value, but it can't boot from it. */
    }
  }
  fp.fcp.rootdir_entry_count = (fp.fcp.rootdir_entry_count + 0xf) & ~0xf;  /* Round up to a multiple of 16. */
  if (fp.fat_fstype == 32) fp.fcp.rootdir_entry_count = 0;
  if (fp.fcp.log2_sectors_per_cluster == (ub)-1) fp.fcp.log2_sectors_per_cluster = fp.default_log2_sectors_per_cluster;  /* Can still be (ub)-1 (unspecified) for non-floppy. */
  if (fp.os_compat & (OSC_DOS3 | OSC_DOS4 | OSC_DOS5_6 | OSC_MSDOS70 | OSC_PCDOS70)) {
    if (fp.fat_count != 2) bad_usage0("OS compatibility requires 2FATS");
  }
  if (fp.os_compat & (OSC_DOS3 | OSC_DOS4)) {
    if (fp.reserved_sector_count != 1) bad_usage0("OS compatibility requires RSC=1");
  }
  if (fp.os_compat & (OSC_DOS3 | OSC_DOS4 | OSC_DOS5_6 | OSC_MSDOS70 | OSC_PCDOS70 | OSC_PCDOS71)) {
    if (fp.fcp.log2_sectors_per_cluster > 0U && (fp.fcp.sector_count == (160U << 1) || fp.fcp.sector_count == (180U << 1) || (fp.fcp.sector_count == (1440U << 1) && (fp.os_compat & (OSC_DOS3 | OSC_DOS4))))) bad_usage0("OS compatibility requires cluster size 512B for this floppy size");
    if (fp.fcp.log2_sectors_per_cluster > 1U && (fp.os_compat & OSC_DOS3)) bad_usage0("OS compatibility requires floppy cluster size 512B or 1K");
    if (fp.fcp.log2_sectors_per_cluster > 5U && (fp.os_compat & (OSC_DOS4 | OSC_DOS5_6 | OSC_PCDOS70 | OSC_PCDOS71))) bad_usage0("OS compatibility requires floppy cluster size at most 16K");
  }
  if (log2_size < 0) {  /* Floppy FAT12. */
    if (fp.fat_fstype != 12) bad_usage0("only FAT12 is supported for floppy");  /* Because boot code is not implemented. */
    if (fp.os_compat & (OSC_DOS3 | OSC_DOS4)) {
      if (fp.fcp.sector_count == (2880U << 1)) bad_usage0("OS compatibility conflicts with 2880K; use e.g. 1440K instead");
    }
    if (fp.fcp.rootdir_entry_count != fp.default_rootdir_entry_count ||
        fp.fcp.log2_sectors_per_cluster != fp.default_log2_sectors_per_cluster ||
        fp.reserved_sector_count != fp.default_reserved_sector_count ||
        fp.fat_count != fp.default_fat_count ||
        0) { /* Recalculate (fp.fcp.cluster_count, fp.fcp.sectors_per_fat). */
#    if DEBUG
      if (!fp.fcp.sector_count) fatal0("ASSERT_SECTORS");
#    endif
      fp.fcp.sectors_per_fat = 0;
      do {
        old_sectors_per_fat = fp.fcp.sectors_per_fat;
        u = fp.hidden_sector_count + fp.reserved_sector_count + ((ud)fp.fcp.sectors_per_fat << (fp.fat_count - 1U)) + (fp.fcp.rootdir_entry_count >> 4);
        if (fp.fcp.sector_count <= u) goto fatal_no_clusters;
        fp.fcp.cluster_count = (fp.fcp.sector_count - u) >> fp.fcp.log2_sectors_per_cluster;
        if (fp.fcp.cluster_count == 0) goto fatal_no_clusters;
        if ((sd)fp.fcp.cluster_count <= 0) bad_usage0("FAT12 filesystem too small, no space for clusters");
        fp.fcp.sectors_per_fat = ((((fp.fcp.cluster_count + 2) * 3 + 1) >> 1) + 0x1ff) >> 9;  /* FAT12. */
      } while (fp.fcp.sectors_per_fat != old_sectors_per_fat);  /* Repeat until a fixed point is found for (fp.fcp.cluster_count, fp.fcp.sectors_per_fat). */
      if (fp.fcp.cluster_count > 0xff4) bad_usage0("floppy FAT12 cluster size too small to this floppy size, specify at least 1K");  /* This happens with: 2880K 512B */
    }
    fp.geometry_sector_count = fp.fcp.sector_count;
#    ifdef DEBUG
      fp.cylinder_count = fp.geometry_sector_count / ((ud)fp.fcp.head_count * fp.fcp.sectors_per_track);
#    endif
  } else {
    fp.hidden_sector_count = 63U;  /* Partition 1 starts here, after the MBR and the rest of cylinder 0, head 0. Must be a multiple of sectors_per_track for MS-DOS <=6.x */
    fp.fcp.media_descriptor = 0xf8;  /* 0xf8 for HDD. 0xf8 is also used by some nonstandard floppy disk formats. */
    if (!log2_size) bad_usage0("image size not specified");
#    ifdef DEBUG
      if (log2_size < 21) fatal0("ASSERT_IMAGE_TOO_SMALL");
      if (log2_size > 43) fatal0("ASSERT_IMAGE_TOO_LARGE");
#    endif
    if (fp.fat_fstype == 12) {
      bad_usage0("FAT12 is not supported for hard disk");  /* Because boot code is not implemented. */
    } else if (fp.fat_fstype == 16) {
      /* No need to check `if (log2_size < 12 + 9) bad_usage0("FAT16 too small");', because we we have log_size >= 21 (2M) here, we don't support smaller values. */
      if (log2_size > 16 + 15) bad_usage0("FAT16 too large, maximum is FAT16 2G");
    } else /* if (fp.fat_fstype == 32) */ {
      if (log2_size < 16 + 9) bad_usage0("FAT32 too small, minimum is FAT32 32M");
      /* No need to check `if (log2_size > 28 + 15) bad_usage0("FAT32 too large");', because we we have log_size <= 41 (2T) <= 43 here, we don't support larger values. */
    }
    /* Cluster count limits:
     * For FAT16: 12 <= log2_size - (log2_spc + 9) <= 16.  log2_size - 25 <= log2_spc <= log2_size - 21.
     * For FAT32: 16 <= log2_size - (log2_spc + 9) <= 28.  log2_size - 37 <= log2_spc <= log2_size - 25.
     */
    if (fp.fcp.log2_sectors_per_cluster == (ub)-1) {
      if (fp.fat_fstype == 16) {
        min_log2_spc = ((fp.os_compat & OSC_DOS3) && log2_size >= 23) ? 2 : (int)log2_size - 25;  /* DOS 3.30 requires cluster size 2K on HDD. */
        fp.fcp.log2_sectors_per_cluster = min_log2_spc >= 0 ? min_log2_spc : 0;
      } else {
        min_log2_spc = (int)log2_size - 37;
        /* Use 4K clusters if possible. */
        fp.fcp.log2_sectors_per_cluster = log2_size == 26 ? 1 : log2_size == 27 ? 2 : log2_size - 28U <= 40U - 28U ? 3 : min_log2_spc >= 0 ? min_log2_spc : 0;
      }
    } else {
      min_log2_spc = log2_size - (fp.fat_fstype == 16 ? 16U + 9U : 28U + 9U);
      max_log2_spc = log2_size - (fp.fat_fstype == 16 ? 12U + 9U : 16U + 9U);
      if (max_log2_spc < (int)fp.fcp.log2_sectors_per_cluster) bad_usage0("sectors-per-cluster too large for this image size");
      if (min_log2_spc > (int)fp.fcp.log2_sectors_per_cluster) bad_usage0("sectors-per-cluster too small for this image size");
    }
    if (fp.os_compat & (OSC_DOS3 | OSC_DOS4 | OSC_DOS5_6 | OSC_MSDOS70 | OSC_PCDOS70)) {
      if (fp.fat_fstype != 16) bad_usage0("OS compatibility requires FAT16 on HDD");
    }
    if (fp.os_compat & OSC_DOS3) {
      if (log2_size - 24U > 25U - 24U) bad_usage0("OS compatibility requires 16M or 32M");
      if (fp.fcp.log2_sectors_per_cluster != 2) bad_usage0("OS compatibility requires cluster size 2K");
      if (fp.fcp.rootdir_entry_count != 512) bad_usage0("OS compatiiblity requires RDEC=512 for booting");
    }
    fp.fcp.cluster_count = ((ud)1 << (log2_size - (fp.fcp.log2_sectors_per_cluster + 9U))) - 2;  /* -2 is for the 2 special cluster entries at the beginning of the FAT table. */
    /* fp.fcp.cluster_count = (fp.fat_fstype == 16 ? auto_fat16_cluster_counts_12 - 12 : auto_fat32_cluster_counts_16 - 16)[log2_size - (fp.fcp.log2_sectors_per_cluster + 9U)]; */
    /* FAT filesystem cluster limits based on:
     *
     * [MS-EFI-FAT32]: https://github.com/LeeKyuHyuk/fat16/raw/refs/heads/master/documentation/fatgen103.pdf
     * [FAT32-Win2000]: https://web.archive.org/web/20150511155943/https://support.microsoft.com/en-us/kb/184006/en-us
     * [FAT32-WinXP]: https://web.archive.org/web/20150513003749/https://support.microsoft.com/en-us/kb/314463
     * [MSDOS-Win95]: https://web.archive.org/web/20150612114245/https://support.microsoft.com/en-us/kb/67321
     * [WinNT4]: experimentation by the Mtools team with Windows NT 4.0: https://github.com/Distrotech/mtools/blob/13058eb225d3e804c8c29a9930df0e414e75b18f/mformat.c#L222
     * [Mtools]: https://github.com/Distrotech/mtools/blob/13058eb225d3e804c8c29a9930df0e414e75b18f/mformat.c#L222 and https://github.com/Distrotech/mtools/blob/13058eb225d3e804c8c29a9930df0e414e75b18f/msdos.h#L215-L225
     * [Linux3.13]: https://github.com/torvalds/linux/blob/d8ec26d7f8287f5788a494f56e8814210f0e64be/include/uapi/linux/msdos_fs.h#L65-L67
     * [Linux6.13]: https://github.com/torvalds/linux/blob/ffd294d346d185b70e28b1a28abe367bbfe53c04/include/uapi/linux/msdos_fs.h#L65-L67
     *
     * The documented cluster limits:
     *
     * * FAT12: at most 0xff4 clusters [MS-EFI-FAT32] [Linux3.13] [Linux6.13] [Mtools], at most 0xff6 clusters [MSDOS-Win95] [WinNT4]
     * * FAT16: at least 0xff5 clusters [MS-EFI-FAT32] [Linux3.13] [Linux6.13] [Mtools], at least 0xff7 clusters [MSDOS-Win95] [WinNT4], at most 0xfff4 clusters [MS-EFI-FAT32] [Linux3.13] [Linux6.13] [Mtools]
     * * FAT32: at least 0xfff5 clusters [MS-EFI-FAT32] [Linux3.13] [Linux6.13] [Mtools], at most 0xffffff5 clusters [FAT32-Win2000] [FAT32-WinXP], at most 0xffffff6 clusters [Linux3.13] [Linux6.13]
     *
     * So our conservative limits become:
     *
     * * FAT12: at least 1 cluster, at most 0xff4 clusters
     * * FAT16: at least 0xff7 clusters, at most 0xfff4 clusters
     * * FAT32: at least 0xfff5 clusters, at most 0xffffff5 clusters; but we want to fit the entire partition in <2TiB, so we will allow less
     */
    if (fp.fat_fstype == 16) {
      if (fp.fcp.cluster_count == 0xfffeU) fp.fcp.cluster_count -= 10U;  /* Maximum 0xfff4 clusters on a FAT16 filesystem. */
    } else if (fp.fat_fstype == 32) {
      if (log2_size == 41) {  /* Avoid overflows below, make sure that fp.geometry_sector_count fits to ud (32-bit unsigned). */
        fp.fcp.sector_count = (fp.vhd_mode >= VHD_FIXED ? VHD_MAX_SECTORS : (ud)0xffffffffU) / (255U * 63U) * (255U * 63U);  /* An upper limit. */
       limit_fat32_by_sector_count:
        if (fp.fcp.sector_count <= fp.hidden_sector_count + fp.reserved_sector_count) goto fatal_no_clusters;
        /* !! TODO(pts): Make hi lower by doing this without ud overflow: (...) * 512U / ((1U << fp.fcp.log2_sectors_per_cluster) + (2U << fp.fat_count)). */
        hi = (fp.fcp.sector_count - fp.hidden_sector_count - fp.reserved_sector_count) >> fp.fcp.log2_sectors_per_cluster;  /* An upper limit on fp.fcp.cluster_count. */
        lo = hi - ((hi + (2U + 0x7fU)) >> 7U << (fp.fat_count - 1U));  /* A lower limit on fp.fcp.cluster_count. */
        while (lo < hi) {  /* Binary search. About 21 iterations. */
          mid = lo + ((hi - lo) >> 1U);
          if (is_aligned_fat32_sector_count_at_most(&fp, mid + 1U)) {
            lo = mid + 1U;
          } else {
            hi = mid;
          }
        }
        if ((fp.fcp.cluster_count = lo) == 0) { fatal_no_clusters:
          /* This can happen e.g. if a very large fp.fcp.rootdir_entry_count or fp.reserved_sector_count was specified, such as `160K RSC=314'. */
          fatal0("FAT filesystem too small, no space for even a single cluster");
        }
      } else if (fp.fcp.cluster_count == (ud)0xffffffeU) {
        fp.fcp.cluster_count -= 9U;  /* Maximum 0xffffff5 clusters on a FAT32 filesystem. */
      } else if (log2_size == 37 && fp.vhd_mode >= VHD_FIXED) {
        /* Limit to ~127.498 GiB instead of 128 GiB, for better VHD
         * compatibility of the virtual IDE controller in Virtual PC.
         *
         * The ~127.498 GiB limit probably still applied to the virtual IDE
         * controller in Virtual PC 2007 (no definitive evidence on the
         * web). Hyper-V (introduced in 2008) has increased the limit to
         * 2040 GiB.
         */
        fp.fcp.sector_count = (ud)65535U * 16U * 255U / (255U * 63U) * (255U * 63U);
        goto limit_fat32_by_sector_count;
      }
    }
    /*if (fp.fat_fstype == 32 && log2_size == 41) fp.fcp.cluster_count -= 0x1fff5 + (0x7ebbc5>>6) - 0x1f73e;*/
    fp.fcp.sectors_per_fat = fp.fat_fstype == 32 ? (fp.fcp.cluster_count + (2U + 0x7fU)) >> 7U : /* fat16: */ (fp.fcp.cluster_count + (2U + 0xffU)) >> 8U;
    fat_clusters_sec_ofs = fp.hidden_sector_count + fp.reserved_sector_count + ((ud)fp.fcp.sectors_per_fat << (fp.fat_count - 1U)) + (fp.fcp.rootdir_entry_count >> 4U);
    fat_clusters_sec_ofs += align_fat(&fp, fat_clusters_sec_ofs);
   recalc_sector_count:
    fp.fcp.sector_count = fat_clusters_sec_ofs + (fp.fcp.cluster_count << fp.fcp.log2_sectors_per_cluster);
    if (fp.fat_fstype == 16 && log2_size == 25 && fp.fcp.log2_sectors_per_cluster == 2 && fp.fcp.sector_count >> 16) {  /* Use at most 0xffff sectors, for compatibility with DOS 3.30. 4.01 supports much more, reaching 2 GiB FAT16. */
      fp.fcp.cluster_count -= (fp.fcp.sector_count - 0xffffU - 1U + ((ud)1 << 2U)) >> 2U;
      goto recalc_sector_count;
    }
#    if DEBUG
      if (fp.fcp.cluster_count > ((ud)0xffffffffU >> fp.fcp.log2_sectors_per_cluster) ||
          fp.fcp.sector_count <= fat_clusters_sec_ofs ||
          (fp.fcp.cluster_count << fp.fcp.log2_sectors_per_cluster) > fp.fcp.sector_count - fat_clusters_sec_ofs
         ) fatal0("ASSERT_SECTOR_COUNT_OVERFLOW");
#    endif
    adjust_hdd_geometry(&fp, fat_clusters_sec_ofs);
  }
#  if DEBUG
    msg_printf("info: cluster_count=0x%lx sector_count=%lu=0x%lx geometry_sector_count=%lu=0x%lx CHS=%lu:%u:%u\n", (unsigned long)fp.fcp.cluster_count, (unsigned long)fp.fcp.sector_count, (unsigned long)fp.fcp.sector_count, (unsigned long)fp.geometry_sector_count, (unsigned long)fp.geometry_sector_count, (unsigned long)fp.cylinder_count, (unsigned)fp.fcp.head_count, (unsigned)fp.fcp.sectors_per_track);
#  endif
  if ((sfd = open(sfn, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666)) < 0) {
    msg_printf("fatal: error opening output file: %s\n", sfn);
    exit(2);
  }
  bakefat_set_sparse(sfd);
  create_fat(&fp);
  close(sfd);
  return 0;
}
