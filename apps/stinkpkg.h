/* .stinkpkg package format -- a tiny tar-ish container for OS packages.
 *
 * Wire layout (all little-endian, no padding between fields):
 *
 *   +-----------------------------+
 *   | magic         u32  "SPKG"   |  0x474B5053
 *   | format ver    u16  1        |
 *   | flags         u16  reserved |
 *   | name          char[32]      |  short package name (e.g. "doom")
 *   | version       char[16]      |  version string (e.g. "1.0.0")
 *   | description   char[128]     |  human-readable summary
 *   | dep_count     u32           |  number of dependency name strings
 *   | file_count    u32           |  number of payload files
 *   | payload_off   u32           |  byte offset of payload area from start
 *   | payload_size  u32           |  total payload bytes
 *   +-----------------------------+
 *   | dep_count x   { name[32] }  |  dependencies (just names today)
 *   | file_count x  { entry }     |  file table (see below)
 *   | payload bytes...            |  concatenated file contents
 *   +-----------------------------+
 *
 * File-table entry (40 bytes):
 *   name           char[32]
 *   size           u32         (bytes in payload area)
 *   offset         u32         (byte offset from payload_off)
 *
 * Integrity:
 *   - SHA-256 of the entire file (computed by the package server) is
 *     published alongside the .stinkpkg in the repo index.
 *   - stink-pkg recomputes that SHA-256 over the downloaded bytes and refuses
 *     to install on any mismatch or missing entry (fail closed). See
 *     index_sha()/hex32() in stinkpkg.c and sha256() in libstink_sha256.c.
 *   - An ed25519 signature over that SHA is a future hardening step.
 */
#ifndef STINKPKG_H
#define STINKPKG_H

#define STINKPKG_MAGIC      0x474B5053u   /* 'S','P','K','G' little-endian */
#define STINKPKG_VERSION    1u

#define STINKPKG_NAME_LEN   32
#define STINKPKG_VER_LEN    16
#define STINKPKG_DESC_LEN   128
#define STINKPKG_FILE_LEN   32

/* Header flag bits. Bit 0 = payload compressed via zlib/deflate. The
 * make-stinkpkg.py builder sets it with --compress; userland currently
 * refuses flagged packages until the inflate decoder lands. */
#define STINKPKG_FLAG_COMPRESSED  0x0001u

struct stinkpkg_hdr {
	unsigned int   magic;
	unsigned short format_ver;
	unsigned short flags;
	char           name[STINKPKG_NAME_LEN];
	char           version[STINKPKG_VER_LEN];
	char           description[STINKPKG_DESC_LEN];
	unsigned int   dep_count;
	unsigned int   file_count;
	unsigned int   payload_off;
	unsigned int   payload_size;
} __attribute__((packed));

struct stinkpkg_dep {
	char name[STINKPKG_NAME_LEN];
} __attribute__((packed));

struct stinkpkg_file {
	char         name[STINKPKG_FILE_LEN];
	unsigned int size;
	unsigned int offset;            /* relative to header.payload_off */
} __attribute__((packed));

#endif
