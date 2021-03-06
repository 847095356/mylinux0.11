/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

// inode有很多 这个就是用来快速管理inode节点的
struct m_inode inode_table[NR_INODE] = {
	{
		0,
	},
};

static void read_inode(struct m_inode *inode);
static void write_inode(struct m_inode *inode);

static inline void wait_on_inode(struct m_inode *inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode *inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock = 1;
	sti();
}

static inline void unlock_inode(struct m_inode *inode)
{
	inode->i_lock = 0;
	wake_up(&inode->i_wait);
}

//释放对应设备下的所有inode节点  这是内存中正在操作的inode节点
void invalidate_inodes(int dev)
{
	int i;
	struct m_inode *inode;

	inode = 0 + inode_table;
	for (i = 0; i < NR_INODE; i++, inode++)
	{
		wait_on_inode(inode);
		if (inode->i_dev == dev)
		{
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0;
		}
	}
}

void sync_inodes(void)
{
	int i;
	struct m_inode *inode;

	inode = 0 + inode_table;
	for (i = 0; i < NR_INODE; i++, inode++)
	{
		wait_on_inode(inode);
		if (inode->i_dirt && !inode->i_pipe)
			write_inode(inode);
	}
}

// create 是否创建新的逻辑块 1创建 0不创建
static int _bmap(struct m_inode *inode, int block, int create)
{
	struct buffer_head *bh;
	int i;

	if (block < 0)
		panic("_bmap: block<0");
	if (block >= 7 + 512 + 512 * 512)
		panic("_bmap: block>big");
	if (block < 7)
	{
		if (create && !inode->i_zone[block])
			if (inode->i_zone[block] = new_block(inode->i_dev))
			{
				inode->i_ctime = CURRENT_TIME;
				inode->i_dirt = 1;
			}
		return inode->i_zone[block];
	}
	block -= 7;
	if (block < 512)
	{
		// 先创建用来存储一次间接块的块
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7] = new_block(inode->i_dev))
			{
				inode->i_dirt = 1;
				inode->i_ctime = CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		if (!(bh = bread(inode->i_dev, inode->i_zone[7])))
			return 0;
		i = ((unsigned short *)(bh->b_data))[block];
		// 创建对应的逻辑块
		if (create && !i)
			if (i = new_block(inode->i_dev))
			{
				((unsigned short *)(bh->b_data))[block] = i;
				bh->b_dirt = 1;
			}
		brelse(bh);
		return i;
	}
	//  > 512的处理
	block -= 512;
	if (create && !inode->i_zone[8])
		if (inode->i_zone[8] = new_block(inode->i_dev))
		{
			inode->i_dirt = 1;
			inode->i_ctime = CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	if (!(bh = bread(inode->i_dev, inode->i_zone[8])))
		return 0;
	i = ((unsigned short *)bh->b_data)[block >> 9]; //移位操作9 = 512     10 = 1024
	if (create && !i)
		if (i = new_block(inode->i_dev))
		{
			((unsigned short *)(bh->b_data))[block >> 9] = i;
			bh->b_dirt = 1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh = bread(inode->i_dev, i)))
		return 0;
	i = ((unsigned short *)bh->b_data)[block & 511]; //找一级间接块上的偏移
	if (create && !i)
		if (i = new_block(inode->i_dev))
		{
			((unsigned short *)(bh->b_data))[block & 511] = i;
			bh->b_dirt = 1;
		}
	brelse(bh);
	return i;
}

int bmap(struct m_inode *inode, int block)
{
	return _bmap(inode, block, 0);
}

int create_block(struct m_inode *inode, int block)
{
	return _bmap(inode, block, 1);
}

//主要是对i_count引用次数进行操作，把i节点引用数值减1
//并且若是管道i节点，则唤醒等待进程
//若是块设备文件i节点则刷新设备
//若i节点的链接计数为0，则释放该i节点占用的所有磁盘逻辑块，并释放该节点
//释放一个inode节点
void iput(struct m_inode *inode)
{
	if (!inode)
		return;
	wait_on_inode(inode);
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	if (inode->i_pipe)
	{
		wake_up(&inode->i_wait);
		if (--inode->i_count)
			return;
		free_page(inode->i_size);
		inode->i_count = 0;
		inode->i_dirt = 0;
		inode->i_pipe = 0;
		return;
	}
	if (!inode->i_dev)
	{
		inode->i_count--;
		return;
	}
	if (S_ISBLK(inode->i_mode)) //判断是否是块设备
	{
		sync_dev(inode->i_zone[0]); //块设备的inode磁盘映射的第一个直接块号存其设备号
		wait_on_inode(inode);
	}
repeat:
	if (inode->i_count > 1)
	{
		inode->i_count--;
		return;
	}
	if (!inode->i_nlinks)
	{
		truncate(inode);
		free_inode(inode);
		return;
	}
	if (inode->i_dirt)
	{
		write_inode(inode); /* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	inode->i_count--;
	return;
}

struct m_inode *get_empty_inode(void)
{
	struct m_inode *inode;
	static struct m_inode *last_inode = inode_table;
	int i;

	do
	{
		inode = NULL;
		for (i = NR_INODE; i; i--)
		{
			if (++last_inode >= inode_table + NR_INODE) //如果用完了就从第一个开始用 就等于第一个
				last_inode = inode_table;
			if (!last_inode->i_count) //如果此inode节点的引用为0就证明没用就用这个
			{
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock) //没有未回写的数据 没有被锁定
					break;
			}
		}
		if (!inode)
		{
			for (i = 0; i < NR_INODE; i++)
				printk("%04x: %6d\t", inode_table[i].i_dev,
					   inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		wait_on_inode(inode);
		while (inode->i_dirt)
		{
			write_inode(inode);
			wait_on_inode(inode);
		}
	} while (inode->i_count);
	memset(inode, 0, sizeof(*inode));
	inode->i_count = 1;
	return inode;
}

struct m_inode *get_pipe_inode(void)
{
	struct m_inode *inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	if (!(inode->i_size = get_free_page()))
	{
		inode->i_count = 0;
		return NULL;
	}
	//这个节点要有2个进程操作 1读 1写
	inode->i_count = 2; /* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	inode->i_pipe = 1;
	return inode;
}

struct m_inode *iget(int dev, int nr)
{
	struct m_inode *inode, *empty;

	if (!dev)
		panic("iget with dev==0");
	empty = get_empty_inode();
	inode = inode_table;
	while (inode < NR_INODE + inode_table)
	{
		if (inode->i_dev != dev || inode->i_num != nr)
		{
			inode++;
			continue;
		}
		wait_on_inode(inode);
		if (inode->i_dev != dev || inode->i_num != nr) //不知道出了什么问题重新执行一次
		{
			inode = inode_table;
			continue;
		}
		inode->i_count++;
		if (inode->i_mount)
		{
			int i;

			for (i = 0; i < NR_SUPER; i++)
				if (super_block[i].s_imount == inode)
					break;
			if (i >= NR_SUPER)
			{
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode);
			dev = super_block[i].s_dev;
			nr = ROOT_INO;
			inode = inode_table;
			continue;
		}
		if (empty)
			iput(empty);
		return inode;
	}
	if (!empty)
		return (NULL);
	inode = empty;
	inode->i_dev = dev;
	inode->i_num = nr;
	read_inode(inode);
	return inode;
}

// read_inode读出来的inode节点是不包含inode内存动态信息的
static void read_inode(struct m_inode *inode)
{
	struct super_block *sb;
	struct buffer_head *bh;
	int block;

	lock_inode(inode);
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
			(inode->i_num - 1) / INODES_PER_BLOCK;
	if (!(bh = bread(inode->i_dev, block)))
		panic("unable to read i-node block");
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num - 1) % INODES_PER_BLOCK]; //一个block 有好几个节点 要读此块上的第几个节点
	brelse(bh);
	unlock_inode(inode);
}

static void write_inode(struct m_inode *inode)
{
	struct super_block *sb;
	struct buffer_head *bh;
	int block;

	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev)
	{
		unlock_inode(inode);
		return;
	}
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to write inode without device");
	//计算当前inode节点的逻辑块号
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
			(inode->i_num - 1) / INODES_PER_BLOCK;
	if (!(bh = bread(inode->i_dev, block)))
		panic("unable to read i-node block");
	((struct d_inode *)bh->b_data)
		[(inode->i_num - 1) % INODES_PER_BLOCK] =
			*(struct d_inode *)inode;
	bh->b_dirt = 1;
	inode->i_dirt = 0;
	brelse(bh);
	unlock_inode(inode);
}
