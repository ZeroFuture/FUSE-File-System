/*
 * file:        fsx600.h
 * description: Data structures for CS 5600/7600 file system.
 *
 * CS 5600, Computer Systems, Northeastern CCIS
 * Peter Desnoyers,  November 2016
 * Philip Gust, March 2019
 */
#ifndef __CSX600_H__
#define __CSX600_H__

enum {
	FS_BLOCK_SIZE = 1024,		/* block size in bytes */
	FS_MAGIC = 0x37363030		/* magic number for superblock */
};

/**
 *  Entry in a directory
 */
enum {FS_FILENAME_SIZE = 28 };
struct fs_dirent {
    uint32_t valid : 1;			/* entry valid flag */
    uint32_t isDir : 1;			/* entry is directory flag */
    uint32_t inode : 30;		/* entry inode */
    char name[FS_FILENAME_SIZE];/* with trailing NUL */
};								/* total 32 bytes */

/**
 * Superblock - holds file system parameters.
 */
struct fs_super {
    uint32_t magic;				/* magic number */
    uint32_t inode_map_sz;		/* inode map size in blocks */
    uint32_t inode_region_sz;	/* inode region size in blocks */
    uint32_t block_map_sz;		/* block map size in blocks */
    uint32_t num_blocks;		/* total blocks, including SB, bitmaps, inodes */
    uint32_t root_inode;		/* always inode 1 */

    /* pad out to an entire block */
    char pad[FS_BLOCK_SIZE - 6 * sizeof(uint32_t)]; 
};								/* total FS_BLOCK_SIZE bytes */

/**
 * Inode - holds file entry information
 */
enum {N_DIRECT = 6 };			/* number direct entries */
struct fs_inode {
    uint16_t uid;				/* user ID of file owner */
    uint16_t gid;				/* group ID of file owner */
    uint32_t mode;				/* permissions | type: file, directory, ... */
    uint32_t ctime;				/* creation time */
    uint32_t mtime;				/* last modification time */
    int32_t size;				/* size in bytes */
    uint32_t direct[N_DIRECT];	/* direct block pointers */
    uint32_t indir_1;			/* single indirect block pointer */
    uint32_t indir_2;			/* double indirect block pointer */
    uint32_t pad[3];            /* 64 bytes per inode */
};								/* total 64 bytes */

/**
 * Constants for blocks
 *   DIRENTS_PER_BLK   - number of directory entries per block
 *   INODES_PER_BLOCK  - number of inodes per block
 *   PTRS_PER_BLOCK    - number of inode pointers per block
 *   BITS_PER_BLOCK    - number of bits per block
 */
enum {
    DIRENTS_PER_BLK = FS_BLOCK_SIZE / sizeof(struct fs_dirent),
	INODES_PER_BLK = FS_BLOCK_SIZE / sizeof(struct fs_inode),
    PTRS_PER_BLK = FS_BLOCK_SIZE / sizeof(uint32_t),
	BITS_PER_BLK = FS_BLOCK_SIZE * 8
};

#endif


