/*
 * file:        blkdev.h
 * description: Block device structure for CS 7600 HW3
 *
 * Peter Desnoyers, Northeastern Computer Science, 2015
 * Philip Gust, Northeastern Computer Science, 2019
 */
#ifndef __BLKDEV_H__
#define __BLKDEV_H__

/**  block device block size */
enum {BLOCK_SIZE = 1024};

/** block device operation status */
enum {SUCCESS = 0, E_BADADDR = -1, E_UNAVAIL = -2, E_SIZE = -3};

/** Definition of a block device */
struct blkdev {
    struct blkdev_ops *ops;		/* operations on block device */
    void *private;				/* block device private state */
};

/** Operations on a block device */
struct blkdev_ops {
    int  (*num_blocks)(struct blkdev *dev);
    int  (*read)(struct blkdev *dev, int first_blk, int num_blks, void *buf);
    int  (*write)(struct blkdev *dev, int first_blk, int num_blks, void *buf);
    int  (*flush)(struct blkdev *dev, int first_blk, int num_blks);
    void (*close)(struct blkdev *dev);
};

#endif
