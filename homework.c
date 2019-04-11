#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsizeof-array-argument"
#pragma ide diagnostic ignored "bugprone-sizeof-expression"
/*
 * file:        homework.c
 * description: skeleton file for CS 5600/7600 file system
 *
 * CS 5600, Computer Systems, Northeastern CCIS
 * Peter Desnoyers, November 2016
 * Philip Gust, March 2019
 */

#define FUSE_USE_VERSION 27

#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <fuse.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>

#include "fsx600.h"
#include "blkdev.h"


//extern int homework_part;       /* set by '-part n' command-line option */

/* 
 * disk access - the global variable 'disk' points to a blkdev
 * structure which has been initialized to access the image file.
 *
 * NOTE - blkdev access is in terms of 1024-byte blocks
 */
extern struct blkdev *disk;

/* by defining bitmaps as 'fd_set' pointers, you can use existing
 * macros to handle them. 
 *   FD_ISSET(##, inode_map);
 *   FD_CLR(##, block_map);
 *   FD_SET(##, block_map);
 */

/** pointer to inode bitmap to determine free inodes */
static fd_set *inode_map;
static int     inode_map_base;

/** pointer to inode blocks */
static struct fs_inode *inodes;
/** number of inodes from superblock */
static int   n_inodes;
/** number of first inode block */
static int   inode_base;

/** pointer to block bitmap to determine free blocks */
fd_set *block_map;
/** number of first data block */
static int     block_map_base;

/** number of available blocks from superblock */
static int   n_blocks;

/** number of root inode from superblock */
static int   root_inode;

/** array of dirty metadata blocks to write  -- optional */
static void **dirty;

/** length of dirty array -- optional */
static int    dirty_len;

/** total size of direct blocks */
static int DIR_SIZE = BLOCK_SIZE * N_DIRECT;
static int INDIR1_SIZE = (BLOCK_SIZE / sizeof(uint32_t)) * BLOCK_SIZE;
static int INDIR2_SIZE = (BLOCK_SIZE / sizeof(uint32_t)) * (BLOCK_SIZE / sizeof(uint32_t)) * BLOCK_SIZE;

/* Suggested functions to implement -- you are free to ignore these
 * and implement your own instead
 */

/**
 * Find inode for existing directory entry.
 *
 * @param fs_dirent ptr to first dirent in directory
 * @param name the name of the directory entry
 * @return the entry inode, or 0 if not found.
 */
static int find_in_dir(struct fs_dirent *de, char *name)
{
    for (int i = 0; i < DIRENTS_PER_BLK; i++) {
        //found, return its inode
        if (de[i].valid && strcmp(de[i].name, name) == 0) {
            return de[i].inode;
        }
    }
    return 0;
}

/**
 * Look up a single directory entry in a directory.
 *
 * Errors
 *   -EIO     - error reading block
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - intermediate component of path not a directory
 *
 */
static int lookup(int inum, char *name)
{
    //get corresponding directory
    struct fs_inode cur_dir = inodes[inum];
    //init buff entries
    struct fs_dirent entries[DIRENTS_PER_BLK];
    memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
    if (disk->ops->read(disk, cur_dir.direct[0], 1, &entries) < 0) exit(1);
    int inode = find_in_dir(entries, name);
    return inode == 0 ? -ENOENT : inode;
}

/**
 * Parse path name into tokens at most nnames tokens after
 * normalizing paths by removing '.' and '..' elements.
 *
 * If names is NULL,path is not altered and function  returns
 * the path count. Otherwise, path is altered by strtok() and
 * function returns names in the names array, which point to
 * elements of path string.
 *
 * @param path the directory path
 * @param names the argument token array or NULL
 * @param nnames the maximum number of names, 0 = unlimited
 * @return the number of path name tokens
 */
static int parse(char *path, char *names[], int nnames)
{
    char *_path = strdup(path);
    int count = 0;
    char *token = strtok(_path, "/");
    while (token != NULL) {
        if (strlen(token) > FS_FILENAME_SIZE - 1) return -EINVAL;
        if (strcmp(token, "..") == 0 && count > 0) count--;
        else if (strcmp(token, ".") != 0) {
            if (names != NULL && count < nnames) {
                names[count] = (char*)malloc(sizeof(char*));
                memset(names[count], 0, sizeof(char*));
                strcpy(names[count], token);
            }
            count++;
        }
        token = strtok(NULL, "/");
    }
    //if the number of names in the path exceed the maximum
    if (nnames != 0 && count > nnames) return -1;
	return count;
}

/**
 * free allocated char ptr array
 * @param arr arr to be freed
 */
static void free_char_ptr_array(char *arr[], int len) {
    for (int i = 0; i < len; i++) {
        free(arr[i]);
    }
}

/**
 * Return inode number for specified file or
 * directory.
 *
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path the file path
 * @return inode of path node or error
 */
static int translate(char *path)
{
    if (strcmp(path, "/") == 0 || strlen(path) == 0) return root_inode;
    int inode_idx = root_inode;
    //get number of names
    int num_names = parse(path, NULL, 0);
    //if the number of names in the path exceed the maximum, return an error, error type to be fixed if necessary
    if (num_names < 0) return -ENOTDIR;
    if (num_names == 0) return root_inode;
    //copy all the names
    char *names[num_names];
    parse(path, names, num_names);
    //lookup inode

    for (int i = 0; i < num_names; i++) {
        //if token is not a directory return error
        if (!S_ISDIR(inodes[inode_idx].mode)) {
            free_char_ptr_array(names, num_names);
            return -ENOTDIR;
        }
        //lookup and record inode
        inode_idx = lookup(inode_idx, names[i]);
        if (inode_idx < 0) {
            free_char_ptr_array(names, num_names);
            return -ENOENT;
        }
    }
    free_char_ptr_array(names, num_names);
    return inode_idx;
}

/**
 *  Return inode number for path to specified file
 *  or directory, and a leaf name that may not yet
 *  exist.
 *
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path the file path
 * @param leaf pointer to space for FS_FILENAME_SIZE leaf name
 * @return inode of path node or error
 */
static int translate_1(char *path, char *leaf)
{
    if (strcmp(path, "/") == 0 || strlen(path) == 0) return root_inode;
    int inode_idx = root_inode;
    //get number of names
    int num_names = parse(path, NULL, 0);
    //if the number of names in the path exceed the maximum, return an error, error type to be fixed if necessary
    if (num_names < 0) return -ENOTDIR;
    if (num_names == 0) return root_inode;
    //copy all the names
    char *names[num_names];
    parse(path, names, num_names);
    //lookup inode

    for (int i = 0; i < num_names - 1; i++) {
        //if token is not a directory return error
        if (!S_ISDIR(inodes[inode_idx].mode)) {
            free_char_ptr_array(names, num_names);
            return -ENOTDIR;
        }
        //lookup and record inode
        inode_idx = lookup(inode_idx, names[i]);
        if (inode_idx < 0) {
            free_char_ptr_array(names, num_names);
            return -ENOENT;
        }
    }
    strcpy(leaf, names[num_names - 1]);
    free_char_ptr_array(names, num_names);
    return inode_idx;
}

/**
 * Mark a inode as dirty.
 *
 * @param in pointer to an inode
 */
static void mark_inode(struct fs_inode *in)
{
    int inum = in - inodes;
    int blk = inum / INODES_PER_BLK;
    dirty[inode_base + blk] = (void*)inodes + blk * FS_BLOCK_SIZE;
}

/**
 * Flush dirty metadata blocks to disk.
 */
void flush_metadata(void)
{
    int i;
    for (i = 0; i < dirty_len; i++) {
        if (dirty[i]) {
            disk->ops->write(disk, i, 1, dirty[i]);
            dirty[i] = NULL;
        }
    }
}

/**
 * Count number of free blocks
 * @return number of free blocks
 */
int num_free_blk() {
    int count = 0;
    for (int i = 0; i < n_blocks; i++) {
        if (!FD_ISSET(i, block_map)) {
            count++;
        }
    }
    return count;
}

/**
 * Returns a free block number or -ENOSPC if none available.
 *
 * @return free block number or -ENOSPC if none available
 */
static int get_free_blk(void)
{
    for (int i = 0; i < n_blocks; i++) {
        if (!FD_ISSET(i, block_map)) {
            char buff[BLOCK_SIZE];
            memset(buff, 0, BLOCK_SIZE);
            if (disk->ops->write(disk, i, 1, buff) < 0) exit(1);
            FD_SET(i, block_map);
            return i;
        }
    }
    return -ENOSPC;
}

/**
 * Return a block to the free list
 *
 * @param  blkno the block number
 */
static void return_blk(int blkno)
{
    FD_CLR(blkno, block_map);
}

static void update_blk(void)
{
    if (disk->ops->write(disk,
                         block_map_base,
                         inode_base - block_map_base,
                         block_map) < 0)
        exit(1);
}

/**
 * Returns a free inode number
 *
 * @return a free inode number or -ENOSPC if none available
 */
static int get_free_inode(void)
{
    for (int i = 2; i < n_inodes; i++) {
        if (!FD_ISSET(i, inode_map)) {
            FD_SET(i, inode_map);
            return i;
        }
    }
    return -ENOSPC;
}

/**
 * Return a inode to the free list.
 *
 * @param  inum the inode number
 */
static void return_inode(int inum)
{
    FD_CLR(inum, inode_map);
}

static void update_inode(int inum)
{
    if (disk->ops->write(disk,
                         inode_base + inum / INODES_PER_BLK,
                         1,
                         &inodes[inum - (inum % INODES_PER_BLK)]) < 0)
        exit(1);
    if (disk->ops->write(disk,
                         inode_map_base,
                         block_map_base - inode_map_base,
                         inode_map) < 0)
        exit(1);
}

/**
 * Find free directory entry.
 *
 * @return index of directory free entry or -ENOSPC
 *   if no space for new entry in directory
 */
static int find_free_dir(struct fs_dirent *de)
{
    for (int i = 0; i < DIRENTS_PER_BLK; i++) {
        if (!de[i].valid) {
            return i;
        }
    }
    return -ENOSPC;
}

/**
 * Determines whether directory is empty.
 *
 * @param de ptr to first entry in directory
 * @return 1 if empty 0 if has entries
 */
static int is_empty_dir(struct fs_dirent *de)
{
    for (int i = 0; i < DIRENTS_PER_BLK; i++) {
        if (de[i].valid) {
            return 0;
        }
    }
    return 1;
}

/**
 * Copy stat from inode to sb
 * @param inode inode to be copied from
 * @param sb holder to hold copied stat
 * @param inode_idx inode_idx
 */
static void cpy_stat(struct fs_inode *inode, struct stat *sb) {
    memset(sb, 0, sizeof(*sb));
    sb->st_uid = inode->uid;
    sb->st_gid = inode->gid;
    sb->st_mode = (mode_t) inode->mode;
    sb->st_atime = inode->mtime;
    sb->st_ctime = inode->ctime;
    sb->st_mtime = inode->mtime;
    sb->st_size = inode->size;
    sb->st_blksize = FS_BLOCK_SIZE;
    sb->st_nlink = 1;
    sb->st_blocks = (inode->size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
}

/* Fuse functions
 */

/**
 * init - this is called once by the FUSE framework at startup.
 *
 * This is a good place to read in the super-block and set up any
 * global variables you need. You don't need to worry about the
 * argument or the return value.
 *
 * @param conn fuse connection information - unused
 * @return unused - returns NULL
 */
void* fs_init(struct fuse_conn_info *conn)
{
	// read the superblock
    struct fs_super sb;
    if (disk->ops->read(disk, 0, 1, &sb) < 0) {
        exit(1);
    }

    root_inode = sb.root_inode;

    /* The inode map and block map are written directly to the disk after the superblock */

    // read inode map
    inode_map_base = 1;
    inode_map = malloc(sb.inode_map_sz * FS_BLOCK_SIZE);
    if (disk->ops->read(disk, inode_map_base, sb.inode_map_sz, inode_map) < 0) {
        exit(1);
    }

    // read block map
    block_map_base = inode_map_base + sb.inode_map_sz;
    block_map = malloc(sb.block_map_sz * FS_BLOCK_SIZE);
    if (disk->ops->read(disk, block_map_base, sb.block_map_sz, block_map) < 0) {
        exit(1);
    }

    /* The inode data is written to the next set of blocks */
    inode_base = block_map_base + sb.block_map_sz;
    n_inodes = sb.inode_region_sz * INODES_PER_BLK;
    inodes = malloc(sb.inode_region_sz * FS_BLOCK_SIZE);
    if (disk->ops->read(disk, inode_base, sb.inode_region_sz, inodes) < 0) {
        exit(1);
    }

    // number of blocks on device
    n_blocks = sb.num_blocks;

    // dirty metadata blocks
    dirty_len = inode_base + sb.inode_region_sz;
    dirty = calloc(dirty_len*sizeof(void*), 1);

    /* your code here */

    return NULL;
}

/* Note on path translation errors:
 * In addition to the method-specific errors listed below, almost
 * every method can return one of the following errors if it fails to
 * locate a file or directory corresponding to a specified path.
 *
 * ENOENT - a component of the path is not present.
 * ENOTDIR - an intermediate component of the path (e.g. 'b' in
 *           /a/b/c) is not a directory
 */

/* note on splitting the 'path' variable:
 * the value passed in by the FUSE framework is declared as 'const',
 * which means you can't modify it. The standard mechanisms for
 * splitting strings in C (strtok, strsep) modify the string in place,
 * so you have to copy the string and then free the copy when you're
 * done. One way of doing this:
 *
 *    char *_path = strdup(path);
 *    int inum = translate(_path);
 *    free(_path);
 */

/**
 * getattr - get file or directory attributes. For a description of
 * the fields in 'struct stat', see 'man lstat'.
 *
 * Note - fields not provided in CS5600fs are:
 *    st_nlink - always set to 1
 *    st_atime, st_ctime - set to same value as st_mtime
 *
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path the file path
 * @param sb pointer to stat struct
 * @return 0 if successful, or -error number
 */
static int fs_getattr(const char *path, struct stat *sb)
{
    fs_init(NULL);

    char *_path = strdup(path);
    int inode_idx = translate(_path);
    if (inode_idx < 0) return inode_idx;
    struct fs_inode* inode = &inodes[inode_idx];
    cpy_stat(inode, sb);
    return SUCCESS;
}

/**
 * readdir - get directory contents.
 *
 * For each entry in the directory, invoke the 'filler' function,
 * which is passed as a function pointer, as follows:
 *     filler(buf, <name>, <statbuf>, 0)
 * where <statbuf> is a struct stat, just like in getattr.
 *
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path the directory path
 * @param ptr  filler buf pointer
 * @param filler filler function to call for each entry
 * @param offset the file offset -- unused
 * @param fi the fuse file information
 */
static int fs_readdir(const char *path, void *ptr, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{
    char *_path = strdup(path);
    int inode_idx = translate(_path);
    if (inode_idx < 0) return inode_idx;
    struct fs_inode *inode = &inodes[inode_idx];
    if (!S_ISDIR(inode->mode)) return -ENOTDIR;
    struct fs_dirent entries[DIRENTS_PER_BLK];
    memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
    struct stat sb;
    if (disk->ops->read(disk, inode->direct[0], 1, entries) < 0) exit(1);
    for (int i = 0; i < DIRENTS_PER_BLK; i++) {
        if (entries[i].valid) {
            cpy_stat(&inodes[entries[i].inode], &sb);
            filler(ptr, entries[i].name, &sb, 0);
        }
    }
    return SUCCESS;
}

/**
 * open - open file directory.
 *
 * You can save information about the open directory in
 * fi->fh. If you allocate memory, free it in fs_releasedir.
 *
 * Errors
 *   -ENOENT  - a component of the path is not present.
 *   -ENOTDIR - an intermediate component of path not a directory
 *
 * @param path the file path
 * @param fi fuse file system information
 */
static int fs_opendir(const char *path, struct fuse_file_info *fi)
{
    char *_path = strdup(path);
    int inode_idx = translate(_path);
    if (inode_idx < 0) return inode_idx;
    if (!S_ISDIR(inodes[inode_idx].mode)) return -ENOTDIR;
    fi->fh = (uint64_t) inode_idx;
    return SUCCESS;
}

/**
 * Release resources when directory is closed.
 * If you allocate memory in fs_opendir, free it here.
 *
 * @param path the directory path
 * @param fi fuse file system information
 */
static int fs_releasedir(const char *path, struct fuse_file_info *fi)
{
    char *_path = strdup(path);
    int inode_idx = translate(_path);
    if (inode_idx < 0) return inode_idx;
    if (!S_ISDIR(inodes[inode_idx].mode)) return -ENOTDIR;
    fi->fh = (uint64_t) -1;
    return SUCCESS;
}

static int set_attributes_and_update(struct fs_dirent *de, char *name, mode_t mode, bool isDir)
{
    //get free directory and inode
    int freed = find_free_dir(de);
    int freei = get_free_inode();
    int freeb = isDir ? get_free_blk() : 0;
    if (freed < 0 || freei < 0 || freeb < 0) return -ENOSPC;
    struct fs_dirent *dir = &de[freed];
    struct fs_inode *inode = &inodes[freei];
    strcpy(dir->name, name);
    dir->inode = freei;
    dir->isDir = true;
    dir->valid = true;
    inode->uid = getuid();
    inode->gid = getgid();
    inode->mode = mode;
    inode->ctime = inode->mtime = time(NULL);
    inode->size = 0;
    inode->direct[0] = freeb;
    //update map and inode
    update_inode(freei);
    update_blk();
    return SUCCESS;
}

/**
 * mknod - create a new file with permissions (mode & 01777)
 * minor device numbers extracted from mode. Behavior undefined
 * when mode bits other than the low 9 bits are used.
 *
 * The access permissions of path are constrained by the
 * umask(2) of the parent process.
 *
 * Errors
 *   -ENOTDIR  - component of path not a directory
 *   -EEXIST   - file already exists
 *   -ENOSPC   - free inode not available
 *   -ENOSPC   - results in >32 entries in directory
 *
 * @param path the file path
 * @param mode the mode, indicating block or character-special file
 * @param dev the character or block I/O device specification
 */
static int fs_mknod(const char *path, mode_t mode, dev_t dev)
{
    //get current and parent inodes
    mode |= S_IFREG;
    if (!S_ISREG(mode) || strcmp(path, "/") == 0) return -EINVAL;
    char *_path = strdup(path);
    char name[FS_FILENAME_SIZE];
    int inode_idx = translate(_path);
    int parent_inode_idx = translate_1(_path, name);
    if (inode_idx >= 0) return -EEXIST;
    if (parent_inode_idx < 0) return parent_inode_idx;
    //read parent info
    struct fs_inode *parent_inode = &inodes[parent_inode_idx];
    if (!(S_ISDIR(parent_inode->mode))) return -ENOTDIR;

    struct fs_dirent entries[DIRENTS_PER_BLK];
    memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
    if (disk->ops->read(disk, parent_inode->direct[0], 1, entries) < 0)
        exit(1);
    //assign inode and directory and update
    int res = set_attributes_and_update(entries, name, mode, false);
    if (res < 0) return res;

    //write entries buffer into disk
    if (disk->ops->write(disk, parent_inode->direct[0], 1, entries) < 0)
        exit(1);
    return SUCCESS;
}

/**
 *  mkdir - create a directory with the given mode. Behavior
 *  undefined when mode bits other than the low 9 bits are used.
 *
 * Errors
 *   -ENOTDIR  - component of path not a directory
 *   -EEXIST   - directory already exists
 *   -ENOSPC   - free inode not available
 *   -ENOSPC   - results in >32 entries in directory
 *
 * @param path path to file
 * @param mode the mode for the new directory
 * @return 0 if successful, or error value
 */ 
static int fs_mkdir(const char *path, mode_t mode)
{
    //get current and parent inodes
    mode |= S_IFDIR;
    if (!S_ISDIR(mode) || strcmp(path, "/") == 0) return -EINVAL;
    char *_path = strdup(path);
    char name[FS_FILENAME_SIZE];
    int inode_idx = translate(_path);
    int parent_inode_idx = translate_1(_path, name);
    if (inode_idx >= 0) return -EEXIST;
    if (parent_inode_idx < 0) return parent_inode_idx;
    //read parent info
    struct fs_inode *parent_inode = &inodes[parent_inode_idx];
    if (!S_ISDIR(parent_inode->mode)) return -ENOTDIR;

    struct fs_dirent entries[DIRENTS_PER_BLK];
    memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
    if (disk->ops->read(disk, parent_inode->direct[0], 1, entries) < 0)
        exit(1);
    //assign inode and directory and update
    int res = set_attributes_and_update(entries, name, mode, true);
    if (res < 0) return res;

    //write entries buffer into disk
    if (disk->ops->write(disk, parent_inode->direct[0], 1, entries) < 0)
        exit(1);
    return SUCCESS;
}

static void fs_truncate_dir(uint32_t *de) {
    for (int i = 0; i < N_DIRECT; i++) {
        if (de[i]) return_blk(de[i]);
        de[i] = 0;
    }
}

static void fs_truncate_indir1(int blk_num) {
    uint32_t entries[PTRS_PER_BLK];
    memset(entries, 0, PTRS_PER_BLK * sizeof(uint32_t));
    if (disk->ops->read(disk, blk_num, 1, entries) < 0)
        exit(1);
    //clear each blk and wipe from blk_map
    for (int i = 0; i < PTRS_PER_BLK; i++) {
        if (entries[i]) return_blk(entries[i]);
        entries[i] = 0;
    }
}

static void fs_truncate_indir2(int blk_num) {
    uint32_t entries[PTRS_PER_BLK];
    memset(entries, 0, PTRS_PER_BLK * sizeof(uint32_t));
    if (disk->ops->read(disk, blk_num, 1, entries) < 0)
        exit(1);
    //clear each double link
    for (int i = 0; i < PTRS_PER_BLK; i++) {
        if (entries[i]) fs_truncate_indir1(entries[i]);
        entries[i] = 0;
    }
}

/**
 * truncate - truncate file to exactly 'len' bytes.
 *
 * Errors:
 *   ENOENT  - file does not exist
 *   ENOTDIR - component of path not a directory
 *   EINVAL  - length invalid (only supports 0)
 *   EISDIR	 - path is a directory (only files)
 *
 * @param path the file path
 * @param len the length
 * @return 0 if successful, or error value
 */
static int fs_truncate(const char *path, off_t len)
{
    /* you can cheat by only implementing this for the case of len==0,
     * and an error otherwise.
     */
    //cheat
    if (len != 0) return -EINVAL;		/* invalid argument */

    //get inode
    char *_path = strdup(path);
    int inode_idx = translate(_path);
    if (inode_idx < 0) return inode_idx;
    struct fs_inode *inode = &inodes[inode_idx];
    if (S_ISDIR(inode->mode)) return -EISDIR;

    //clear direct
    fs_truncate_dir(inode->direct);

    //clear indirect1
    if (inode->indir_1) {
        fs_truncate_indir1(inode->indir_1);
        return_blk(inode->indir_1);
    }
    inode->indir_1 = 0;

    //clear indirect2
    if (inode->indir_2) {
        fs_truncate_indir2(inode->indir_2);
        return_blk(inode->indir_2);
    }
    inode->indir_2 = 0;

    inode->size = 0;

    //update at the end for efficiency
    update_inode(inode_idx);
    update_blk();

    return SUCCESS;
}

/**
 * unlink - delete a file.
 *
 * Errors
 *   -ENOENT   - file does not exist
 *   -ENOTDIR  - component of path not a directory
 *   -EISDIR   - cannot unlink a directory
 *
 * @param path path to file
 * @return 0 if successful, or error value
 */
static int fs_unlink(const char *path)
{
    //truncate first
    int res = fs_truncate(path, 0);
    if (res < 0) return res;

    //get inodes and check
    char *_path = strdup(path);
    char name[FS_FILENAME_SIZE];
    int inode_idx = translate(_path);
    int parent_inode_idx = translate_1(_path, name);
    struct fs_inode *inode = &inodes[inode_idx];
    struct fs_inode *parent_inode = &inodes[parent_inode_idx];
    if (inode_idx < 0 || parent_inode_idx < 0) return -ENOENT;
    if (S_ISDIR(inode->mode)) return -EISDIR;
    if (!S_ISDIR(parent_inode->mode)) return -ENOTDIR;

    //remove entire entry from parent dir
    struct fs_dirent entries[DIRENTS_PER_BLK];
    memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
    if (disk->ops->read(disk, parent_inode->direct[0], 1, entries) < 0)
        exit(1);
    for (int i = 0; i < DIRENTS_PER_BLK; i++) {
        if (entries[i].valid && strcmp(entries[i].name, name) == 0) {
            memset(&entries[i], 0, sizeof(struct fs_dirent));
        }
    }
    if (disk->ops->write(disk, parent_inode->direct[0], 1, entries) < 0)
        exit(1);

    //clear inode
    memset(inode, 0, sizeof(struct fs_inode));
    return_inode(inode_idx);

    //update
    update_inode(inode_idx);
    update_blk();

    return SUCCESS;
}

/**
 * rmdir - remove a directory.
 *
 * Errors
 *   -ENOENT   - file does not exist
 *   -ENOTDIR  - component of path not a directory
 *   -ENOTDIR  - path not a directory
 *   -ENOEMPTY - directory not empty
 *
 * @param path the path of the directory
 * @return 0 if successful, or error value
 */
static int fs_rmdir(const char *path)
{
    //can not remove root
    if (strcmp(path, "/") == 0) return -EINVAL;

    //get inodes and check
    char *_path = strdup(path);
    char name[FS_FILENAME_SIZE];
    int inode_idx = translate(_path);
    int parent_inode_idx = translate_1(_path, name);
    struct fs_inode *inode = &inodes[inode_idx];
    struct fs_inode *parent_inode = &inodes[parent_inode_idx];
    if (inode_idx < 0 || parent_inode_idx < 0) return -ENOENT;
    if (!S_ISDIR(inode->mode)) return -ENOTDIR;
    if (!S_ISDIR(parent_inode->mode)) return -ENOTDIR;

    //check if dir if empty
    struct fs_dirent entries[DIRENTS_PER_BLK];
    memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
    if (disk->ops->read(disk, inode->direct[0], 1, entries) < 0)
        exit(1);
    int res = is_empty_dir(entries);
    if (res == 0) return -ENOTEMPTY;

    //remove entry from parent dir
    memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
    if (disk->ops->read(disk, parent_inode->direct[0], 1, entries) < 0)
        exit(1);
    for (int i = 0; i < DIRENTS_PER_BLK; i++) {
        if (entries[i].valid && strcmp(entries[i].name, name) == 0) {
            memset(&entries[i], 0, sizeof(struct fs_dirent));
        }
    }
    if (disk->ops->write(disk, parent_inode->direct[0], 1, entries) < 0)
        exit(1);

    //return blk and clear inode
    return_blk(inode->direct[0]);
    return_inode(inode_idx);
    memset(inode, 0, sizeof(struct fs_inode));

    //update
    update_inode(inode_idx);
    update_blk();

    return SUCCESS;
}

/**
 * rename - rename a file or directory.
 *
 * Note that this is a simplified version of the UNIX rename
 * functionality - see 'man 2 rename' for full semantics. In
 * particular, the full version can move across directories, replace a
 * destination file, and replace an empty directory with a full one.
 *
 * Errors:
 *   -ENOENT   - source file or directory does not exist
 *   -ENOTDIR  - component of source or target path not a directory
 *   -EEXIST   - destination already exists
 *   -EINVAL   - source and destination not in the same directory
 *
 * @param src_path the source path
 * @param dst_path the destination path.
 * @return 0 if successful, or error value
 */
static int fs_rename(const char *src_path, const char *dst_path)
{
    //deep copy both path
    char *_src_path = strdup(src_path);
    char *_dst_path = strdup(dst_path);
    //get inodes
    int src_inode_idx = translate(_src_path);
    int dst_inode_idx = translate(_dst_path);
    //if src inode does not exist return error
    if (src_inode_idx < 0) return src_inode_idx;
    //if dst already exist return error
    if (dst_inode_idx >= 0) return -EEXIST;

    //get parent directory inode
    char src_name[FS_FILENAME_SIZE];
    char dst_name[FS_FILENAME_SIZE];
    int src_parent_inode_idx = translate_1(_src_path, src_name);
    int dst_parent_inode_idx = translate_1(_dst_path, dst_name);
    //src and dst should be in the same directory (same parent)
    if (src_parent_inode_idx != dst_parent_inode_idx) return -EINVAL;
    int parent_inode_idx = src_parent_inode_idx;
    if (parent_inode_idx < 0) return parent_inode_idx;

    //read parent dir inode
    struct fs_inode *parent_inode = &inodes[parent_inode_idx];
    if (!S_ISDIR(parent_inode->mode)) return -ENOTDIR;

    struct fs_dirent entries[DIRENTS_PER_BLK];
    memset(entries, 0, DIRENTS_PER_BLK * sizeof(struct fs_dirent));
    if (disk->ops->read(disk, parent_inode->direct[0], 1, entries) < 0) exit(1);

    //make change to buff
    for (int i = 0; i < DIRENTS_PER_BLK; i++) {
        if (entries[i].valid && strcmp(entries[i].name, src_name) == 0) {
            memset(entries[i].name, 0, sizeof(entries[i].name));
            strcpy(entries[i].name, dst_name);
        }
    }

    //write buff to inode
    if (disk->ops->write(disk, parent_inode->direct[0], 1, entries)) exit(1);
    return SUCCESS;
}

/**
 * chmod - change file permissions
 *
 * Errors:
 *   -ENOENT   - file does not exist
 *   -ENOTDIR  - component of path not a directory
 *
 * @param path the file or directory path
 * @param mode the mode_t mode value -- see man 'chmod'
 *   for description
 * @return 0 if successful, or error value
 */
static int fs_chmod(const char *path, mode_t mode)
{
    char* _path = strdup(path);
    int inode_idx = translate(_path);
    if (inode_idx < 0) return inode_idx;
    struct fs_inode *inode = &inodes[inode_idx];
    //protect system from other modes
    mode |= S_ISDIR(inode->mode) ? S_IFDIR : S_IFREG;
    //change through reference
    inode->mode = mode;
    update_inode(inode_idx);
    return SUCCESS;
}

/**
 * utime - change access and modification times.
 *
 * Errors:
 *   -ENOENT   - file does not exist
 *   -ENOTDIR  - component of path not a directory
 *
 * @param path the file or directory path.
 * @param ut utimbuf - see man 'utime' for description.
 * @return 0 if successful, or error value
 */
int fs_utime(const char *path, struct utimbuf *ut)
{
    char *_path = strdup(path);
    int inode_idx = translate(_path);
    if (inode_idx < 0) return inode_idx;
    struct fs_inode *inode = &inodes[inode_idx];
    //change through reference
    inode->mtime = ut->modtime;
    update_inode(inode_idx);
    return SUCCESS;
}

static void fs_read_blk(int blk_num, char *buf, size_t len, size_t offset) {
    char entries[BLOCK_SIZE];
    memset(entries, 0, BLOCK_SIZE * sizeof(char));
    if (disk->ops->read(disk, blk_num, 1, entries) < 0) exit(1);
    memcpy(buf, entries + offset, len);
}

static size_t fs_read_dir(size_t inode_idx, char *buf, size_t len, size_t offset) {
    struct fs_inode *inode = &inodes[inode_idx];
    size_t blk_num = offset / BLOCK_SIZE;
    size_t blk_offset = offset % BLOCK_SIZE;
    size_t len_to_read = len;
    while (blk_num < N_DIRECT && len_to_read > 0) {
        size_t cur_len_to_read = len_to_read > BLOCK_SIZE ? (size_t) BLOCK_SIZE - blk_offset : len_to_read;
        size_t temp = blk_offset + cur_len_to_read;

        if (!inode->direct[blk_num]) {
            return len - len_to_read;
        }

        fs_read_blk(inode->direct[blk_num], buf, temp, blk_offset);

        buf += temp;
        len_to_read -= temp;
        blk_num++;
        blk_offset = 0;
    }
    return len - len_to_read;
}

static size_t fs_read_indir1(size_t blk, char *buf, size_t len, size_t offset) {
    uint32_t blk_indices[PTRS_PER_BLK];
    memset(blk_indices, 0, PTRS_PER_BLK * sizeof(uint32_t));
    if (disk->ops->read(disk, (int) blk, 1, blk_indices) < 0) exit(1);

    size_t blk_num = offset / BLOCK_SIZE;
    size_t blk_offset = offset % BLOCK_SIZE;
    size_t len_to_read = len;
    while (blk_num < PTRS_PER_BLK && len_to_read > 0) {
        size_t cur_len_to_read = len_to_read > BLOCK_SIZE ? (size_t) BLOCK_SIZE - blk_offset : len_to_read;
        size_t temp = blk_offset + cur_len_to_read;

        if (!blk_indices[blk_num]) {
            return len - len_to_read;
        }

        fs_read_blk(blk_indices[blk_num], buf, temp, blk_offset);

        buf += temp;
        len_to_read -= temp;
        blk_num++;
        blk_offset = 0;
    }
    return len - len_to_read;
}

static size_t fs_read_indir2(size_t blk, char *buf, size_t len, size_t offset) {
    uint32_t blk_indices[PTRS_PER_BLK];
    memset(blk_indices, 0, PTRS_PER_BLK * sizeof(uint32_t));
    if (disk->ops->read(disk, (int) blk, 1, blk_indices) < 0) return 0;

    size_t blk_num = offset / INDIR1_SIZE;
    size_t blk_offset = offset % INDIR1_SIZE;
    size_t len_to_read = len;
    while (blk_num < PTRS_PER_BLK && len_to_read > 0) {
        size_t cur_len_to_read = len_to_read > INDIR1_SIZE ? (size_t) INDIR1_SIZE - blk_offset : len_to_read;
        size_t temp = blk_offset + cur_len_to_read;

        if (!blk_indices[blk_num]) {
            return len - len_to_read;
        }

        temp = fs_read_indir1(blk_indices[blk_num], buf, temp, blk_offset);

        buf += temp;
        len_to_read -= temp;
        blk_num++;
        blk_offset = 0;
    }
    return len - len_to_read;
}

/**
 * read - read data from an open file.
 *
 * Should return exactly the number of bytes requested, except:
 *   - if offset >= file len, return 0
 *   - if offset+len > file len, return bytes from offset to EOF
 *   - on error, return <0
 *
 * Errors:
 *   -ENOENT  - file does not exist
 *   -ENOTDIR - component of path not a directory
 *   -EIO     - error reading block
 *
 * @param path the path to the file
 * @param buf the read buffer
 * @param len the number of bytes to read
 * @param offset to start reading at
 * @param fi fuse file info
 */
static int fs_read(const char *path, char *buf, size_t len, off_t offset,
		    struct fuse_file_info *fi)
{
    char *_path = strdup(path);
    int inode_idx = translate(_path);
    if (inode_idx < 0) return inode_idx;
    struct fs_inode *inode = &inodes[inode_idx];
    if (S_ISDIR(inode->mode)) return -EISDIR;
    if (offset >= inode->size) return 0;

    //if len go beyond inode size, read to EOF
    if (offset + len > inode->size) {
        len = (size_t) inode->size - offset;
    }

    //len need to read
    size_t len_to_read = len;

    //read direct blocks
    if (len_to_read > 0 && offset < DIR_SIZE) {
        //len finished read
        size_t temp = fs_read_dir(inode_idx, buf, len_to_read, (size_t) offset);
        len_to_read -= temp;
        offset += temp;
        buf += temp;
    }

    //read indirect 1 blocks
    if (len_to_read > 0 && offset < DIR_SIZE + INDIR1_SIZE) {
        //len finished read
        size_t temp = fs_read_indir1(inode->indir_1, buf, len_to_read, (size_t) offset - DIR_SIZE);
        len_to_read -= temp;
        offset += temp;
        buf += temp;
    }

    //read indirect 2 blocks
    if (len_to_read > 0 && offset < DIR_SIZE + INDIR1_SIZE + INDIR2_SIZE) {
        //len finshed read
        size_t temp = fs_read_indir2(inode->indir_2, buf, len_to_read, (size_t) offset - DIR_SIZE - INDIR1_SIZE);
        len_to_read -= temp;
        offset += temp;
        buf += temp;
    }

    return (int) (len - len_to_read);
}

static void fs_write_blk(int blk_num, const char *buf, size_t len, size_t offset) {
    char entries[BLOCK_SIZE];
    memset(entries, 0, BLOCK_SIZE * sizeof(char));
    if (disk->ops->read(disk, blk_num, 1, entries) < 0) exit(1);
    memcpy(entries + offset, buf, len);
    if (disk->ops->write(disk, blk_num, 1, entries) < 0) exit(1);
}

static size_t fs_write_dir(size_t inode_idx, const char *buf, size_t len, size_t offset) {
    struct fs_inode *inode = &inodes[inode_idx];

    size_t blk_num = offset / BLOCK_SIZE;
    size_t blk_offset = offset % BLOCK_SIZE;
    size_t len_to_write = len;
    while (blk_num < N_DIRECT && len_to_write > 0) {
        size_t cur_len_to_write = len_to_write > BLOCK_SIZE ? (size_t) BLOCK_SIZE - blk_offset : len_to_write;
        size_t temp = blk_offset + cur_len_to_write;

        if (!inode->direct[blk_num]) {
            int freeb = get_free_blk();
            if (freeb < 0) return len - len_to_write;
            inode->direct[blk_num] = freeb;
            update_inode(inode_idx);
        }

        fs_write_blk(inode->direct[blk_num], buf, temp, blk_offset);

        buf += temp;
        len_to_write -= temp;
        blk_num++;
        blk_offset = 0;
    }
    return len - len_to_write;
}

static size_t fs_write_indir1(size_t blk, const char *buf, size_t len, size_t offset) {
    uint32_t blk_indices[PTRS_PER_BLK];
    memset(blk_indices, 0, PTRS_PER_BLK * sizeof(uint32_t));
    if (disk->ops->read(disk, (int) blk, 1, blk_indices) < 0) exit(1);

    size_t blk_num = offset / BLOCK_SIZE;
    size_t blk_offset = offset % BLOCK_SIZE;
    size_t len_to_write = len;
    while (blk_num < PTRS_PER_BLK && len_to_write > 0) {
        size_t cur_len_to_write = len_to_write > BLOCK_SIZE ? (size_t) BLOCK_SIZE - blk_offset : len_to_write;
        size_t temp = blk_offset + cur_len_to_write;

        if (!blk_indices[blk_num]) {
            int freeb = get_free_blk();
            if (freeb < 0) return len - len_to_write;
            blk_indices[blk_num] = freeb;
            //write back
            if (disk->ops->write(disk, blk, 1, blk_indices) < 0)
                exit(1);
        }

        fs_write_blk(blk_indices[blk_num], buf, temp, blk_offset);

        buf += temp;
        len_to_write -= temp;
        blk_num++;
        blk_offset = 0;
    }
    return len - len_to_write;
}

static size_t fs_write_indir2(size_t blk, const char *buf, size_t len, size_t offset) {
    uint32_t blk_indices[PTRS_PER_BLK];
    memset(blk_indices, 0, PTRS_PER_BLK * sizeof(uint32_t));
    if (disk->ops->read(disk, (int) blk, 1, blk_indices) < 0) return 0;

    size_t blk_num = offset / INDIR1_SIZE;
    size_t blk_offset = offset % INDIR1_SIZE;
    size_t len_to_write = len;
    while (blk_num < PTRS_PER_BLK && len_to_write > 0) {
        size_t cur_len_to_write = len_to_write > INDIR1_SIZE ? (size_t) INDIR1_SIZE - blk_offset : len_to_write;
        size_t temp = blk_offset + cur_len_to_write;
        len_to_write -= temp;
        if (!blk_indices[blk_num]) {
            int freeb = get_free_blk();
            if (freeb < 0) return len - len_to_write;
            blk_indices[blk_num] = freeb;
            //write back
            if (disk->ops->write(disk, blk, 1, blk_indices) < 0)
                exit(1);
        }

        temp = fs_write_indir1(blk_indices[blk_num], buf, temp, blk_offset);
        if (temp == 0) return len - len_to_write;
        buf += temp;
        blk_num++;
        blk_offset = 0;
    }
    return len - len_to_write;
}

/**
 *  write - write data to a file
 *
 * It should return exactly the number of bytes requested, except on
 * error.
 *
 * Errors:
 *   -ENOENT  - file does not exist
 *   -ENOTDIR - component of path not a directory
 *   -EINVAL  - if 'offset' is greater than current file length.
 *  			(POSIX semantics support the creation of files with
 *  			"holes" in them, but we don't)
 *
 * @param path the file path
 * @param buf the buffer to write
 * @param len the number of bytes to write
 * @param offset the offset to starting writing at
 * @param fi the Fuse file info for writing
 */
static int fs_write(const char *path, const char *buf, size_t len,
		     off_t offset, struct fuse_file_info *fi)
{
    char *_path = strdup(path);
    int inode_idx = translate(_path);
    if (inode_idx < 0) return inode_idx;
    struct fs_inode *inode = &inodes[inode_idx];
    if (S_ISDIR(inode->mode)) return -EISDIR;
    if (offset > inode->size) return 0;

    //len need to write
    size_t len_to_write = len;

    //write direct blocks
    if (len_to_write > 0 && offset < DIR_SIZE) {
        //len finished write
        size_t temp = fs_write_dir(inode_idx, buf, len_to_write, (size_t) offset);
        len_to_write -= temp;
        offset += temp;
        buf += temp;
    }

    //write indirect 1 blocks
    if (len_to_write > 0 && offset < DIR_SIZE + INDIR1_SIZE) {
        //need to allocate indir_1
        if (!inode->indir_1) {
            int freeb = get_free_blk();
            if (freeb < 0) return len - len_to_write;
            inode->indir_1 = freeb;
            update_inode(inode_idx);
        }
        size_t temp = fs_write_indir1(inode->indir_1, buf, len_to_write, (size_t) offset - DIR_SIZE);
        len_to_write -= temp;
        offset += temp;
        buf += temp;
    }

    //write indirect 2 blocks
    if (len_to_write > 0 && offset < DIR_SIZE + INDIR1_SIZE + INDIR2_SIZE) {
        //need to allocate indir_2
        if (!inode->indir_2) {
            int freeb = get_free_blk();
            if (freeb < 0) return len - len_to_write;
            inode->indir_2 = freeb;
            update_inode(inode_idx);
        }
        //len finshed write
        size_t temp = fs_write_indir2(inode->indir_2, buf, len_to_write, (size_t) offset - DIR_SIZE - INDIR1_SIZE);
        len_to_write -= temp;
        offset += len_to_write;
    }

    if (offset > inode->size) inode->size = offset;

    //update inode and blk
    update_inode(inode_idx);
    update_blk();

    return (int) (len - len_to_write);
}

/**
 * Open a filesystem file or directory path.
 *
 * Errors:
 *   -ENOENT  - file does not exist
 *   -ENOTDIR - component of path not a directory
 *
 * @param path the path
 * @param fuse file info data
 */
static int fs_open(const char *path, struct fuse_file_info *fi)
{
    char *_path = strdup(path);
    int inode_idx = translate(_path);
    if (inode_idx < 0) return inode_idx;
    if (S_ISDIR(inodes[inode_idx].mode)) return -EISDIR;
    fi->fh = (uint64_t) inode_idx;
    return SUCCESS;
}

/**
 * Release resources created by pending open call.
 *
 * Errors:
 *   -ENOENT  - file does not exist
 *   -ENOTDIR - component of path not a directory
 *
 * @param path the file name
 * @param fi the fuse file info
 */
static int fs_release(const char *path, struct fuse_file_info *fi)
{
    char *_path = strdup(path);
    int inode_idx = translate(_path);
    if (inode_idx < 0) return inode_idx;
    if (S_ISDIR(inodes[inode_idx].mode)) return -EISDIR;
    fi->fh = (uint64_t) -1;
    return SUCCESS;
}

/**
 * statfs - get file system statistics.
 * See 'man 2 statfs' for description of 'struct statvfs'.
 *
 * Errors
 *   none -  Needs to work
 *
 * @param path the path to the file
 * @param st the statvfs struct
 */
static int fs_statfs(const char *path, struct statvfs *st)
{
    /* needs to return the following fields (set others to zero):
     *   f_bsize = BLOCK_SIZE
     *   f_blocks = total image - metadata
     *   f_bfree = f_blocks - blocks used
     *   f_bavail = f_bfree
     *   f_namelen = <whatever your max namelength is>
     *
     * this should work fine, but you may want to add code to
     * calculate the correct values later.
     */

    //clear original stats
    memset(st, 0, sizeof(*st));
    st->f_bsize = FS_BLOCK_SIZE;
    st->f_blocks = (fsblkcnt_t) (n_blocks - root_inode - inode_base);
    st->f_bfree = (fsblkcnt_t) num_free_blk();
    st->f_bavail = st->f_bfree;
    st->f_namemax = FS_FILENAME_SIZE - 1;

    return 0;
}

/**
 * Operations vector. Please don't rename it, as the
 * skeleton code in misc.c assumes it is named 'fs_ops'.
 */
struct fuse_operations fs_ops = {
    .init = fs_init,
    .getattr = fs_getattr,
    .opendir = fs_opendir,
    .readdir = fs_readdir,
    .releasedir = fs_releasedir,
    .mknod = fs_mknod,
    .mkdir = fs_mkdir,
    .unlink = fs_unlink,
    .rmdir = fs_rmdir,
    .rename = fs_rename,
    .chmod = fs_chmod,
    .utime = fs_utime,
    .truncate = fs_truncate,
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .release = fs_release,
    .statfs = fs_statfs,
};


#pragma clang diagnostic pop