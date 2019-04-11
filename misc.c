struct stat sb;
/*
 * file:        misc.c
 * description: various support functions for CS 5600/7600 file system
 *              startup argument parsing and checking, etc.
 *
 * CS 5600, Computer Systems, Northeastern CCIS
 * Peter Desnoyers, November 2016
 * Philip Gust, March 2019
 */

#define FUSE_USE_VERSION 27
#define _XOPEN_SOURCE 500
#define _ATFILE_SOURCE
#define _BSD_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <fuse.h>
#include "image.h"

#include "fsx600.h"		/* only for certain constants */

/*********** DO NOT MODIFY THIS FILE *************/

// should be defined in string.h but is not on macos
extern char *strdup(const char* str);
/**
 * All homework functions accessed through operations structure. */
extern struct fuse_operations fs_ops;

/**  disk block device */
struct blkdev *disk;

struct data {
    char *image_name;
    int   part;
    int   cmd_mode;
} _data;
int homework_part;

/**
 * Constant: maximum path length
 */
enum { MAX_PATH = 4096 };

static void help(){
    printf("Arguments:\n");
    printf(" -cmdline : Enter an interactive REPL that provides a filesystem view into the image\n");
    printf(" -image <name.img> : Use the provided image file that contains the filesystem\n");
//    printf(" -part # : Give either 1, 2 or 3 that correlates to the question in the homework being tested. This will set the homework_part global variable, which may be useful for you as your program runs.\n");
}

/*
 * See comments in /usr/include/fuse/fuse_opts.h for details of
 * FUSE argument processing.
 *
 *  usage: ./homework -image disk.img [-part #] directory
 *              disk.img  - name of the image file to mount
 *              directory - directory to mount it on
 */
static struct fuse_opt opts[] = {
        {"-image %s", offsetof(struct data, image_name), 0},
        {"-cmdline", offsetof(struct data, cmd_mode), 1},
// PJG -- temporary
//    {"-part %d", offsetof(struct data, part), 0},
        FUSE_OPT_END
};

/* Utility functions
 */

/*
 * strmode - translate a numeric mode into a string
 *
 * @param output buffer
 * @param mode the numeric mode
 * @return pointer to output buffer
 */
static char *strmode(char *buf, int mode)
{
    int mask = 0400;
    char *str = "rwxrwxrwx", *retval = buf;
    *buf++ = S_ISDIR(mode) ? 'd' : '-';
    for (mask = 0400; mask != 0; str++, mask = mask >> 1)
        *buf++ = (mask & mode) ? *str : '-';
    *buf++ = 0;
    return retval;
}

/**
 * Split string into array of at most n tokens.
 *
 * If toks is NULL, p is not altered and function returns
 * the token count. Otherwise, p is altered by strtok()
 * and function returns tokens in the toks array, which
 * point to elements of p string.
 *
 * @param p the character string
 * @param toks token array
 * @param n max number of tokens to retrieve, 0 = unlimited
 * @param delim the delimiter string between tokens
 */
static int split(char *p, char *toks[], int n, char *delim)
{
    if (n == 0) {
        n = INT_MAX;
    }
    if (toks == NULL) {
        // do not alter p if not returning names
        p = strdup(p);
    }
    char *str;
    char *lasts = NULL;
    int i;
    for (i = 0; i < n && (str = strtok_r(p, delim, &lasts)) != NULL; i++) {
        p = NULL;
        if (toks != NULL) {
            toks[i] = str;
        }
    }
    if (toks == NULL) {
        free(p);
    }
    return i;
}

/** current working directory */
static char cwd[MAX_PATH];

/**
 * Update cwd global from global paths[] array elements.
 *
 * @param array of path elements
 * @param depth number of elements
 */
static void update_cwd(char *paths[], int npaths)
{
    char *p = cwd;
    if (npaths == 0) {
        strcpy(cwd,"/");
    } else {
        for (int i = 0; i < npaths; i++) {
            p += sprintf(p, "/%s", paths[i]);
        }
    }
}

/**
 * Get current working directory.
 */
static const char *get_cwd(void)
{
    return cwd;
}

/**
 * Normalize path array to eliminate empty names,
 * and current (.) and parent (..) directory names.
 *
 * @param names array of name elements
 * @param nnames the number of elements
 * @param the new nnames value after normalization
 */
static int normalize_paths(char *names[], int nnames) {
    int dest = 0;
    for (int src = 0; src < nnames; src++) {
        if (strcmp(names[src],".") == 0) {
            // ignore current directory path element
        } else if (strcmp(names[src], "..") == 0) {
            if (dest > 0) dest--;  // back up one directory element
        } else {  // add directory element
            if (dest < src) {
                strcpy(names[dest], names[src]);
            }
            dest++;
        }
    }
    return dest;
}

/**
 * Copy full path name of path to pathbuf. If path
 * starts with '/', copies path to pathbuf. Otherwise,
 * prepends cwd to path separated by '/'.
 *
 * @param path an absolute or relative path
 * @param pathbuf result path buffer must be MAX_PATH length
 * @return pointer to path buffer
 */
static char *full_path(const char *path, char *pathbuf) {
    // split into path elements and normalize path
    char tmp_path[MAX_PATH];
    if (*path == '/') {
        strcpy(tmp_path, path);
    } else {
        sprintf(tmp_path, "%s/%s", get_cwd(), path);
    }

    // split path into path names
    int npaths = split(tmp_path, NULL, 0, "/");  // get path count
    char **names = malloc(npaths*sizeof(char*));
    split(tmp_path, names, npaths, "/");	// split into path names

    npaths = normalize_paths(names, npaths);
    if (npaths == 0) {
        strcpy(pathbuf, "/");
    } else {
        // append path elements into pathbuf
        char *bufp = pathbuf;
        for (int i = 0; i < npaths; i++) {
            bufp += sprintf(bufp, "/%s", names[i]);
        }
    }
    free(names);

    return pathbuf;
}

/**
 * Change directory.
 *
 * @param argv arg[0] is new directory
 */
static int do_cd1(char *argv[])
{
    struct stat sb;
    char pathbuf[MAX_PATH];
    int retval = fs_ops.getattr(full_path(argv[0], pathbuf), &sb);
    if (retval == 0) {
        if (S_ISDIR(sb.st_mode)) {
            int nnames = split(pathbuf, NULL, 0, "/");  // get count
            char **names = malloc(nnames * sizeof(char*));

            split(pathbuf, names, nnames, "/");
            update_cwd(names, nnames);

            free(names);
            return 0;
        } else {
            return -ENOTDIR;
        }
    } else {
        return -ENOENT;
    }
}

/**
 * Change to root directory.
 */
static int do_cd0(char *argv[])
{
    char *args[] = {"/"};
    return do_cd1(args);
}

/**
 * Print current working directory
 */
static int do_pwd(char *argv[])
{
    printf("%s\n", get_cwd());
    return 0;
}

static char lsbuf[DIRENTS_PER_BLK][MAX_PATH]; /** buffer to list directory entries */
static int  lsi;  /* current ls index */

static void init_ls(void)
{
    lsi = 0;
}

static int filler(void *buf, const char *name, const struct stat *sb, off_t off)
{
    sprintf(lsbuf[lsi++], "%s\n", name);
    return 0;
}

/**
 * Sort and print directory listings.
 */
static void print_ls(void)
{
    int i;
    qsort(lsbuf, lsi, MAX_PATH, (void*)strcmp);
    for (i = 0; i < lsi; i++) {
        printf("%s", lsbuf[i]);
    }
}

/**
 * List directory relative to current working directory.
 *
 * @param argv argv[0] is a directory name
 */
int do_ls1(char *argv[])
{
    char pathbuf[MAX_PATH];
    full_path(argv[0], pathbuf);
    struct fuse_file_info info;
    memset(&info, 0, sizeof(struct fuse_file_info));
    int retval;
    if ((retval = fs_ops.opendir(pathbuf, &info)) == 0) {
        init_ls();
        retval = fs_ops.readdir(pathbuf, NULL, filler, 0, &info);
        print_ls();
        fs_ops.releasedir(pathbuf, &info);
    }
    return retval;
}

/**
 * List current working directory
 */
static int do_ls0(char *argv[])
{
    char *cwd = strdup(get_cwd());
    int status = do_ls1(&cwd);
    free(cwd);
    return status;
}

/**
 * Callback adds directory entry listing to ls buffer.
 * Form of function is specified by Fuse readdir API.
 */
static int dashl_filler(void *buf, const char *name, const struct stat *sb, off_t off)
{
    char mode[16], time[26], *lasts;
    sprintf(lsbuf[lsi++], "%5lld %s %2d %4d %4d %8lld %s %s\n",
            sb->st_blocks, strmode(mode, sb->st_mode),
            sb->st_nlink, sb->st_uid, sb->st_gid, sb->st_size,
            strtok_r(ctime_r(&sb->st_mtime,time),"\n",&lasts), name);
    return 0;
}


/**
 * Long list of specified directory.
 *
 * @path path of directory to list
 */
static int _lsdashl(const char *path)
{
    struct stat sb;
    char pathbuf[MAX_PATH];
    full_path(path, pathbuf);
    int retval = fs_ops.getattr(pathbuf, &sb);
    if (retval == 0) {
        init_ls();
        struct fuse_file_info info;
        memset(&info, 0, sizeof(struct fuse_file_info));
        if (S_ISDIR(sb.st_mode)) {
            if ((retval = fs_ops.opendir(pathbuf, &info)) == 0) {
                /* read directory information */
                retval = fs_ops.readdir(pathbuf, NULL, dashl_filler, 0, &info);
            }
            fs_ops.releasedir(pathbuf, &info);
        } else {
            /* read file information */
            retval = dashl_filler(NULL, pathbuf, &sb, 0);
        }
        /* print file or directory information */
        print_ls();
    }
    return retval;
}

/**
 * Long list of specified directory
 * relative to current directory.
 *
 * @param argv argv[0] is directory name
 */
static int do_lsdashl1(char *argv[])
{
    char pathbuf[MAX_PATH];
    full_path(argv[0], pathbuf);
    return _lsdashl(pathbuf);
}

/**
 * Long list of current directory.
 *
 * @param argv unused
 */
static int do_lsdashl0(char *argv[])
{
    char *cwd = strdup(get_cwd());
    int status = do_lsdashl1(&cwd);
    free(cwd);
    return status;
}

/**
 * Modify the mode of a file or directory name specified
 * by argv[0] using the mode string specified by argv[1].
 * The file or directory name will be interpreted relative
 * to the current working directory.
 *
 * @param argv argv[0] is mode, argv[1] is a file or
 * directory name
 */
static int do_chmod(char *argv[])
{
    char path[MAX_PATH];
    int mode = strtol(argv[0], NULL, 8);
    full_path(argv[1], path);
    return fs_ops.chmod(path, mode);
}

/**
 * Rename directory.
 *
 * @param argv argv[0] is old name, arg[1] is new name
 *   relative to working directory
 */
static int do_rename(char *argv[])
{
    char p1[MAX_PATH], p2[MAX_PATH];
    full_path(argv[0], p1);
    full_path(argv[1], p2);
    return fs_ops.rename(p1, p2);
}

/**
 * Make directory.
 *
 * @param argv argv[0] is directory name
 */
static int do_mkdir(char *argv[])
{
    char path[MAX_PATH];
    full_path(argv[0], path);
    return fs_ops.mkdir(path, 0777);
}

/**
 * Remove a directory.
 *
 * @param argv argv[0] is directory name
 */
static int do_rmdir(char *argv[])
{
    char path[MAX_PATH];
    full_path(argv[0], path);
    return fs_ops.rmdir(path);
}

/**
 * Remove a file.
 *
 * @param argv argv[0] is file name
 */
static int do_rm(char *argv[])
{
    char path[MAX_PATH];
    full_path(argv[0], path);
    return fs_ops.unlink(path);
}

static int blksiz;		/* size of block buffer */
static char *blkbuf;	/* block buffer for coping files */

/**
 * Copy a file from localdir into filesystem
 *
 * @param argv arg[0] is local file, argv[1] is
 *   filesystem file name
 */
static int do_put(char *argv[])
{
    char *outside = argv[0], *inside = argv[1];
    char path[MAX_PATH];
    int len, fd, offset = 0, val;

    if ((fd = open(outside, O_RDONLY, 0)) < 0) {
        return fd;
    }
    full_path(inside, path);
    if ((val = fs_ops.mknod(path, 0777 | S_IFREG, 0)) != 0) {
        return val;
    }

    struct fuse_file_info info;
    memset(&info, 0, sizeof(struct fuse_file_info));
    if ((val = fs_ops.open(path, &info)) != 0) {
        return val;
    }
    while ((len = read(fd, blkbuf, blksiz)) > 0) {
        val = fs_ops.write(path, blkbuf, len, offset, &info);
        if (val != len) {
            break;
        }
        offset += len;
    }
    close(fd);
    fs_ops.release(path, &info);
    return (val >= 0) ? 0 : val;
}

/**
 * Copy a file from localdir into file system with
 * same name.
 *
 * @param argv arg[0] is file name; file system name
 *   relative to current directory
 */
static int do_put1(char *argv[])
{
    char *args2[] = {argv[0], argv[0]};
    return do_put(args2);
}

/**
 * Copy a file from filesystem to localdir
 *
 * @param argv arg[0] is filesystem file name,
 *   argv[1] is local file
 */
static int do_get(char *argv[])
{
    char *inside = argv[0], *outside = argv[1];
    char path[MAX_PATH];
    int len, fd, offset = 0;

    if ((fd = open(outside, O_WRONLY|O_CREAT|O_TRUNC, 0777)) < 0) {
        return fd;
    }
    full_path(inside, path);
    struct fuse_file_info info;
    memset(&info, 0, sizeof(struct fuse_file_info));
    int val;
    if ((val = fs_ops.open(path, &info)) != 0) {
        return val;
    }
    while (1) {
        len = fs_ops.read(path, blkbuf, blksiz, offset, &info);
        if (len >= 0) {
            len = write(fd, blkbuf, len);
            if (len <= 0) {
                break;
            }
            offset += len;
        }
    }
    close(fd);
    fs_ops.release(path, &info);
    return (len >= 0) ? 0 : len;
}

/**
 * Copy a file from filesystem to localdir with
 * same name.
 *
 * @param argv arg[0] is file name; fileystem name
 *   relative to current directory
 */
static int do_get1(char *argv[])
{
    char *args2[] = {argv[0], argv[0]};
    return do_get(args2);
}

/**
 * Retrieve and print a file.
 *
 * @param argv argv[0] is file name
 */
static int do_show(char *argv[])
{
    char path[MAX_PATH];
    int len, offset = 0;

    full_path(argv[0], path);
    struct fuse_file_info info;
    memset(&info, 0, sizeof(struct fuse_file_info));
    int val;
    if ((val = fs_ops.open(path, &info)) != 0) {
        return val;
    }
    while ((len = fs_ops.read(path, blkbuf, blksiz, offset, &info)) > 0) {
        fwrite(blkbuf, len, 1, stdout);
        offset += len;
    }
    fs_ops.release(path, &info);
    return (len >= 0) ? 0 : len;
}

/**
 * Print filesystem statistics
 *
 * @argv unused
 */
static int do_statfs(char *argv[])
{
    struct statvfs st;
    int retval = fs_ops.statfs("/", &st);
    if (retval == 0) {
        printf("block size: %lu\n", st.f_bsize);
        printf("no. blocks: %u\n", st.f_blocks);
        printf("avail blocks: %u\n", st.f_bavail);
        printf("max name length: %lu\n", st.f_namemax);
    }
    return retval;
}

/**
 * Print files statistics
 *
 * @param argv argv[0] is file name relative
 *   to current directory
 */
static int do_stat(char *argv[])
{
    struct stat sb;
    int retval = fs_ops.getattr(argv[0], &sb);
    if (retval == 0) {
        char mode[16], time[26], *lasts;
        printf("%5lld %s %2d %4d %4d %8lld %s %s\n",
               sb.st_blocks, strmode(mode, sb.st_mode),
               sb.st_nlink, sb.st_uid, sb.st_gid, sb.st_size,
               strtok_r(ctime_r(&sb.st_mtime,time),"\n",&lasts), argv[0]);
    }
    return retval;
}

/**
 * Set read/write block size
 *
 * @param size read/write block size
 */
static void _blksiz(int size)
{
    blksiz = size;	// record new block size
    if (blkbuf) {	// free old block buffer
        free(blkbuf);
    }
    blkbuf = malloc(blksiz);	// create new block buffer
    printf("read/write block size: %d\n", blksiz);
}

/**
 * Set read/write block size
 *
 * @param argv argv[0] is block size as string
 */
static int do_blksiz(char *argv[])
{
    _blksiz(atoi(argv[0]));
    return 0;
}

/**
 * Truncate file.
 *
 * @param argv argv[0] is file name relative
 *   to current directory
 */
static int do_truncate(char *argv[])
{
    char path[MAX_PATH];
    full_path(argv[0], path);
    return fs_ops.truncate(path, 0);
}

/**
 * Set access and modification time.
 *
 * @param argv argv[0] is file name relative
 *   to current directory
 */
static int do_utime(char *argv[])
{
    struct utimbuf ut;
    char path[MAX_PATH];
    full_path(argv[0], path);
    ut.actime = ut.modtime = time(NULL);	// set access time to now
    return fs_ops.utime(path, &ut);
}

/**
 * Creates regular file or set access and modification time.
 *
 * @param argv argv[0] is file name relative
 *   to current directory
 */
static int do_touch(char *argv[])
{
    char path[MAX_PATH];
    full_path(argv[0], path);
    // try creating new file
    int status = fs_ops.mknod(path, 0777 | S_IFREG, 0);
    if (status == -EEXIST) {
        // if exists, modify its access/mod time to now
        struct utimbuf ut;
        ut.actime = ut.modtime = time(NULL);
        status =fs_ops.utime(path, &ut);
    }
    return status;
}

/** struct serves a dispatch table for commands */
static struct {
    char *name;
    int   nargs;
    int   (*f)(char *args[]);
    char  *help;
} cmds[] = {
        {"cd", 0, do_cd0, "cd - change to root directory"},
        {"cd", 1, do_cd1, "cd <dir> - change to directory"},
        {"pwd", 0, do_pwd, "pwd - display current directory"},
        {"ls", 0, do_ls0, "ls - list files in current directory"},
        {"ls", 1, do_ls1, "ls <dir> - list specified directory"},
        {"ls-l", 0, do_lsdashl0, "ls-l - display detailed file listing"},
        {"ls-l", 1, do_lsdashl1, "ls-l <file> - display detailed file info"},
        {"chmod", 2, do_chmod, "chmod <mode> <file> - change permissions"},
        {"rename", 2, do_rename, "rename <oldname> <newname> - rename file"},
        {"mkdir", 1, do_mkdir, "mkdir <dir> - create directory"},
        {"rmdir", 1, do_rmdir, "rmdir <dir> - remove directory"},
        {"rm", 1, do_rm, "rm <file> - remove file"},
        {"put", 2, do_put, "put <outside> <inside> - copy a file from localdir into file system"},
        {"put", 1, do_put1, "put <name> - ditto, but keep the same name"},
        {"get", 2, do_get, "get <inside> <outside> - retrieve a file from file system to local directory"},
        {"get", 1, do_get1, "get <name> - ditto, but keep the same name"},
        {"show", 1, do_show, "show <file> - retrieve and print a file"},
        {"statfs", 0, do_statfs, "statfs - print file system info"},
        {"blksiz", 1, do_blksiz, "blksiz - set read/write block size"},
        {"truncate", 1, do_truncate, "truncate <file> - truncate to zero length"},
        {"utime", 1, do_utime, "utime <file> - set modified time to current time"},
        {"touch", 1, do_touch, "touch <file> - create file or set modified time to current time"},
        {"stat", 1, do_stat, "stat <file> - print file info"},
        {0, 0, 0}
};

/**
 * Command loop for interactive command interpreter.
 */
static int cmdloop(void)
{
    char line[MAX_PATH];

    update_cwd(NULL, 0);

    while (true) {
        printf("cmd> "); fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL)
            break;

        if (!isatty(0)) {
            printf("%s", line);
        }

        if (line[0] == '#')	{/* comment lines */
            continue;
        }

        // split input into command and args
        char *args[10];
        int i, nargs = split(line, args, 10, " \t\r\n");

        // continue if empty line
        if (nargs == 0) {
            continue;
        }

        // quit if command is "quit" or "exit"
        if ((strcmp(args[0], "quit") == 0) || (strcmp(args[0], "exit") == 0)) {
            break;
        }

        // provide help if command is "help" or "?"
        if ((strcmp(args[0], "help") == 0) || (strcmp(args[0], "?") == 0)) {
            for (i = 0; cmds[i].name != NULL; i++) {
                printf("%s\n", cmds[i].help);
            }
            continue;
        }

        // validate command and arguments
        for (i = 0; cmds[i].name != NULL; i++) {
            if ((strcmp(args[0], cmds[i].name) == 0) && (nargs == cmds[i].nargs+1)) {
                break;
            }
        }

        // if command not recognized or incorrect arg count
        if (cmds[i].name == NULL) {
            if (nargs > 0) {
                printf("bad command: %s\n", args[0]);
            }
            continue;
        }

        // process command
        int err = cmds[i].f(&args[1]);
        if (err != 0) {
            printf("error: %s\n", strerror(-err));
        }
    }
    return 0;
}


/**************/

/**
 * This function fixes up argv arguments when run under GDB
 * on Eclipse CDT. Eclipse adds single-quotes around all
 * arguments, which must be stripped out.
 *
 * @param argc the number of parameters
 * @param argv the parameter values
 */
static void fixup(int argc, char *argv[]) {
    for (int i = 0; i < argc; i++) {
        int len = strlen(argv[i]);
        if (argv[i][0] == '\'' && argv[i][len-1] == '\'') {
            argv[i][len-1] = '\0';
            argv[i]++;
        }
    }
}

int main(int argc, char **argv)
{
    fixup(argc, argv);

    /* Argument processing and checking
     */
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    if (fuse_opt_parse(&args, &_data, opts, NULL) == -1){
        help();
        exit(1);
    }

    if (_data.image_name == 0){
        fprintf(stderr, "You must provide an image\n");
        help();
        exit(1);
    }

    char *file = _data.image_name;
    if (strcmp(file+strlen(file)-4, ".img") != 0) {
        fprintf(stderr, "bad image file (must end in .img): %s\n", file);
        help();
        exit(1);
    }

    if ((disk = image_create(file)) == NULL) {
        fprintf(stderr, "cannot open image file '%s': %s\n", file, strerror(errno));
        help();
        exit(1);
    }

//    homework_part = _data.part;
    homework_part = 2; // PJG

    if (_data.cmd_mode) {  /* process interactive commands */
        fs_ops.init(NULL);
        _blksiz(FS_BLOCK_SIZE);
        cmdloop();
        return 0;
    }

    /** pass control to fuse */
    return fuse_main(args.argc, args.argv, &fs_ops, NULL);
}