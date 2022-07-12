/*
 * This file has definitions for some important file table
 * structures etc.
 */

#ifndef _FS_H
#define _FS_H

#include <sys/types.h>

/* devices are as follows: (same as minix, so we can use the minix
 * file system. These are major numbers.)
 *
 * 0 - unused (nodev)
 * 1 - /dev/mem
 * 2 - /dev/fd
 * 3 - /dev/hd
 * 4 - /dev/ttyx
 * 5 - /dev/tty
 * 6 - /dev/lp
 * 7 - unnamed pipes
 */

#define IS_SEEKABLE(x) ((x) >= 1 && (x) <= 3)

#define READ 0
#define WRITE 1
#define READA 2	 /* read-ahead - don't pause */
#define WRITEA 3 /* "write-ahead" - silly, but somewhat useful */

void buffer_init(long buffer_end);

#define MAJOR(a) (((unsigned)(a)) >> 8)
#define MINOR(a) ((a)&0xff)

#define NAME_LEN 14
#define ROOT_INO 1

#define I_MAP_SLOTS 8
#define Z_MAP_SLOTS 8
#define SUPER_MAGIC 0x137F

#define NR_OPEN 20
#define NR_INODE 32
#define NR_FILE 64
#define NR_SUPER 8
#define NR_HASH 307
#define NR_BUFFERS nr_buffers
#define BLOCK_SIZE 1024
#define BLOCK_SIZE_BITS 10
#ifndef NULL
#define NULL ((void *)0)
#endif

#define INODES_PER_BLOCK ((BLOCK_SIZE) / (sizeof(struct d_inode)))
#define DIR_ENTRIES_PER_BLOCK ((BLOCK_SIZE) / (sizeof(struct dir_entry)))

#define PIPE_HEAD(inode) ((inode).i_zone[0])
#define PIPE_TAIL(inode) ((inode).i_zone[1])
#define PIPE_SIZE(inode) ((PIPE_HEAD(inode) - PIPE_TAIL(inode)) & (PAGE_SIZE - 1))
#define PIPE_EMPTY(inode) (PIPE_HEAD(inode) == PIPE_TAIL(inode))
#define PIPE_FULL(inode) (PIPE_SIZE(inode) == (PAGE_SIZE - 1))
#define INC_PIPE(head) \
	__asm__("incl %0\n\tandl $4095,%0" ::"m"(head))

typedef char buffer_block[BLOCK_SIZE];

struct buffer_head
{
	char *b_data;				/* 数据区 pointer to data block (1024 bytes) */
	unsigned long b_blocknr;	/* 数据逻辑块号 1K block number */
	unsigned short b_dev;		/* 块设备号（硬盘上的） 1K device (0 = free) */
	unsigned char b_uptodate;	//更新的标志位
	unsigned char b_dirt;		/* 是否为脏位 写盘的时候检索的标志位 0-clean,1-dirty */
	unsigned char b_count;		/* users using this block */
	unsigned char b_lock;		/* 锁 0 - ok, 1 -locked */
	struct task_struct *b_wait; //等待该高速缓冲区释放的进程结构体指针 等待该高速缓冲区解锁的进程指针
	struct buffer_head *b_prev;
	struct buffer_head *b_next;
	struct buffer_head *b_prev_free; //构成了空闲缓冲区的循环链表（当前高速缓冲区中所有剩余的没有用到的缓冲区的循环链表）
	struct buffer_head *b_next_free;
};

//Linux把inode分为两种方式保存，一种是在硬盘中的inode（d_inode），一种是在内存中的inode（m_inode）。m_inode除了完全包含d_inode中的字段之外还有一些专门的字段。
struct d_inode
{
	unsigned short i_mode;
	unsigned short i_uid;
	unsigned long i_size;
	unsigned long i_time;
	unsigned char i_gid;
	unsigned char i_nlinks;
	unsigned short i_zone[9];
};

struct m_inode
{
	unsigned short i_mode;	  // i_mode一共10位，第一位表明结点文件类型，后9位依次为：I结点所有者、所属组成员、其他成员的权限（权限有读写执行三种）。
							  // 文件的类型和属性	dr-xr-xr-x  8 system         system                0 1970-08-31 12:08 cpuset
							  // 最前面代表文件类型 -普通文件 d目录 s符号链接 ppipe管道 c字符设备 b块设备 l软链接文件
	unsigned short i_uid;	  // 宿主用户ID
	unsigned long i_size;	  // 该文件的大小	如果是目录文件则是总文件大小
	unsigned long i_mtime;	  // 该文件的修改时间
	unsigned char i_gid;	  // 宿主的组id
	unsigned char i_nlinks;	  // 链接数 表明有多少文件链接到此结点 有其他的硬链接连接到本文件 那么链接+1
	unsigned short i_zone[9]; // 该文件映射在逻辑块号的数组 文件数据对应在磁盘上的位置 块设备的inode磁盘映射的第一个直接块号存其设备号 1K 一级7K
							  // 文件和磁盘的映射  i_zone[6]  直接块号  如果你的文件只占用7个逻辑块那么这个数组中的每一个单元存储了一个逻辑块号
							  // i_zone[7]一次间接块号   如果占用的逻辑块较多 大于7 小于512+7  则占用一次间接块号
							  // i_zone[8]二次间接快号   如果占用的逻辑块太多 大鱼512+7 小于512*512+7 则启动二次间接逻辑块
	/* these are in memory also */
	struct task_struct *i_wait; // task_struct是进程pCB结构。I结点的等待队列。
	unsigned long i_atime;		// 最后访问的时间
	unsigned long i_ctime;		// i节点自身被修改的时间
	unsigned short i_dev;		// i节点所在的设备号
	unsigned short i_num;		// i节点号   就是逻辑块的索引
	unsigned short i_count;		// i节点被打开的次数，0是空闲  I结点被打开次数，主要用于判断文件是否共享！
	unsigned char i_lock;		// 判断I结点是否被锁住。
	unsigned char i_dirt;		// 判断I结点是否需要写回磁盘。
	unsigned char i_pipe;		// i节点用作管道标志
								// pipi会对应一块无聊内存，所以根本不需要写盘
	unsigned char i_mount;		// 如果有文件系统安装在此结点上，则置此位   也就是说这个节点作为一个目录了
	unsigned char i_seek;		// 在lseek调用时置此位  lseek是一个用于改变读写一个文件时读写指针位置的一个系统调用
	unsigned char i_update;		// i节点已更新的标志
};

struct file
{
	unsigned short f_mode;
	unsigned short f_flags;
	unsigned short f_count;
	struct m_inode *f_inode;
	off_t f_pos;
};

struct super_block
{
	unsigned short s_ninodes;		// i节点数
	unsigned short s_nzones;		//逻辑块数
	unsigned short s_imap_blocks;	// i节点位图个数
	unsigned short s_zmap_blocks;	//逻辑块位图个数
	unsigned short s_firstdatazone; //第一个逻辑块号
	unsigned short s_log_zone_size; // log2（磁盘块/逻辑块）
	unsigned long s_max_size;		//最大文件长度
	unsigned short s_magic;			//文件系统幻数
	/* These are only in memory */
	struct buffer_head *s_imap[8]; // i节点位图在高速缓冲区块指针数组
	struct buffer_head *s_zmap[8]; //逻辑块位图在高速缓冲区块指针数组
	unsigned short s_dev;		   //设备号
	struct m_inode *s_isup;		   //根目录的i节点
	struct m_inode *s_imount;	   //要安装到目录的i节点
	unsigned long s_time;
	struct task_struct *s_wait; //等待超级块的进程
	unsigned char s_lock;
	unsigned char s_rd_only;
	unsigned char s_dirt; //已被修改的配置
};

struct d_super_block
{
	unsigned short s_ninodes;
	unsigned short s_nzones;
	unsigned short s_imap_blocks;
	unsigned short s_zmap_blocks;
	unsigned short s_firstdatazone;
	unsigned short s_log_zone_size;
	unsigned long s_max_size;
	unsigned short s_magic;
};

struct dir_entry
{
	unsigned short inode;	//i节点号
	char name[NAME_LEN];	//名字
};

extern struct m_inode inode_table[NR_INODE];
extern struct file file_table[NR_FILE];
extern struct super_block super_block[NR_SUPER];
extern struct buffer_head *start_buffer;
extern int nr_buffers;

extern void check_disk_change(int dev);
extern int floppy_change(unsigned int nr);
extern int ticks_to_floppy_on(unsigned int dev);
extern void floppy_on(unsigned int dev);
extern void floppy_off(unsigned int dev);
extern void truncate(struct m_inode *inode);
extern void sync_inodes(void);
extern void wait_on(struct m_inode *inode);
extern int bmap(struct m_inode *inode, int block);
extern int create_block(struct m_inode *inode, int block);
extern struct m_inode *namei(const char *pathname);
extern int open_namei(const char *pathname, int flag, int mode,
					  struct m_inode **res_inode);
extern void iput(struct m_inode *inode);
extern struct m_inode *iget(int dev, int nr);
extern struct m_inode *get_empty_inode(void);
extern struct m_inode *get_pipe_inode(void);
extern struct buffer_head *get_hash_table(int dev, int block);
extern struct buffer_head *getblk(int dev, int block);
extern void ll_rw_block(int rw, struct buffer_head *bh);
extern void brelse(struct buffer_head *buf);
extern struct buffer_head *bread(int dev, int block);
extern void bread_page(unsigned long addr, int dev, int b[4]);
extern struct buffer_head *breada(int dev, int block, ...);
extern int new_block(int dev);
extern void free_block(int dev, int block);
extern struct m_inode *new_inode(int dev);
extern void free_inode(struct m_inode *inode);
extern int sync_dev(int dev);
extern struct super_block *get_super(int dev);
extern int ROOT_DEV;

extern void mount_root(void);

#endif
