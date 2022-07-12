/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc */
#define set_bit(bitnr, addr) ({ \
register int __res __asm__("ax"); \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

//操作系统中有一个超级块数组
//把需要挂在设备文件系统的super_block读到高速缓冲区中并且放到超级块数组中
struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
//跟文件系统的设备号
int ROOT_DEV = 0;

static void lock_super(struct super_block *sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

static void free_super(struct super_block *sb)
{
	cli();
	sb->s_lock = 0;
	wake_up(&(sb->s_wait)); //唤醒在等待这个块的进程
	sti();
}

static void wait_on_super(struct super_block *sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}

struct super_block *get_super(int dev)
{
	struct super_block *s;

	if (!dev)
		return NULL;
	s = 0 + super_block;
	while (s < NR_SUPER + super_block) //从super_block数组中进行遍历
		if (s->s_dev == dev)
		{
			wait_on_super(s);
			if (s->s_dev == dev) //确保在等待的过程中此超级块没有被修改占用
				return s;
			s = 0 + super_block; //未知错误重新进行相关操作
		}
		else
			s++;
	return NULL;
}

void put_super(int dev)
{
	struct super_block *sb;
	struct m_inode *inode;
	int i;

	if (dev == ROOT_DEV)
	{
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	if (!(sb = get_super(dev)))
		return;
	if (sb->s_imount) //超级块安装在哪
	{
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	//先判断一下此超级块的状态 内否被卸载 锁定进行卸载操作
	lock_super(sb);
	sb->s_dev = 0;
	for (i = 0; i < I_MAP_SLOTS; i++) //释放超级块中所有i节点位图
		brelse(sb->s_imap[i]);
	for (i = 0; i < Z_MAP_SLOTS; i++) //释放超级块中所有逻辑块位图
		brelse(sb->s_zmap[i]);
	free_super(sb);
	return;
}

static struct super_block *read_super(int dev)
{
	struct super_block *s;
	struct buffer_head *bh;
	int i, block;

	if (!dev)
		return NULL;
	check_disk_change(dev); //检查是否换过盘片（即是否是软盘设备，要经常插拔的），若换过，高速缓冲区关于该设备的缓冲块均失效，需要做失效处理，即释放原来加载的文件系统
	if (s = get_super(dev))
		return s;
	//寻找超级块数组中的空槽
	for (s = 0 + super_block;; s++)
	{
		if (s >= NR_SUPER + super_block)
			return NULL;
		if (!s->s_dev) //超级块对应的dev为空
			break;
	}
	//设置超级块在内存中的动态配置项
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	lock_super(s);
	if (!(bh = bread(dev, 1)))
	{
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	//设置超级块固有的设备参数
	*((struct d_super_block *)s) =
		*((struct d_super_block *)bh->b_data);
	brelse(bh);
	//检测标识码
	if (s->s_magic != SUPER_MAGIC)
	{
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	//清空 i节点位图 和 逻辑块 位图
	for (i = 0; i < I_MAP_SLOTS; i++)
		s->s_imap[i] = NULL;
	for (i = 0; i < Z_MAP_SLOTS; i++)
		s->s_zmap[i] = NULL;
	block = 2;
	//根据取出的超级块读取对应设备的i节点位图
	for (i = 0; i < s->s_imap_blocks; i++)
		if (s->s_imap[i] = bread(dev, block))
			block++;
		else
			break;
	for (i = 0; i < s->s_zmap_blocks; i++)
		if (s->s_zmap[i] = bread(dev, block))
			block++;
		else
			break;
	//如果读出的块数不等于因该站有的块数，则说明文件系统位图有问题，释放申请的资源返回
	if (block != 2 + s->s_imap_blocks + s->s_zmap_blocks)
	{
		for (i = 0; i < I_MAP_SLOTS; i++)
			brelse(s->s_imap[i]);
		for (i = 0; i < Z_MAP_SLOTS; i++)
			brelse(s->s_zmap[i]);
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	//对于申请空闲i节点的函数来讲，如果设备上所与的i节点都被使用，则返回为0。所以0号节点不能使用，逻辑块也是如此。
	s->s_imap[0]->b_data[0] |= 1;
	s->s_zmap[0]->b_data[0] |= 1;
	free_super(s);
	return s;
}

int sys_umount(char *dev_name)
{
	struct m_inode *inode;
	struct super_block *sb;
	int dev;

	if (!(inode = namei(dev_name)))
		return -ENOENT;
	//inode节点中第一个直接块号是设备号
	dev = inode->i_zone[0];
	if (!S_ISBLK(inode->i_mode))
	{
		iput(inode);
		return -ENOTBLK;
	}
	iput(inode);
	if (dev == ROOT_DEV)
		return -EBUSY;
	//获取超级块失败 或者文件系统没有被挂接
	if (!(sb = get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	//检索设备是否有正在使用的inode节点
	for (inode = inode_table + 0; inode < inode_table + NR_INODE; inode++)
		if (inode->i_dev == dev && inode->i_count)
			return -EBUSY;
	sb->s_imount->i_mount = 0;
	//这里为啥要释放要是 其目录下 或者跟目录下有别的文件怎么办？
	iput(sb->s_imount);
	sb->s_imount = NULL;
	iput(sb->s_isup);
	sb->s_isup = NULL;
	put_super(dev);
	sync_dev(dev);
	return 0;
}

int sys_mount(char *dev_name, char *dir_name, int rw_flag)
{
	struct m_inode *dev_i, *dir_i;
	struct super_block *sb;
	int dev;

	if (!(dev_i = namei(dev_name)))
		return -ENOENT;
	dev = dev_i->i_zone[0];
	if (!S_ISBLK(dev_i->i_mode))
	{
		iput(dev_i);
		return -EPERM;
	}
	iput(dev_i);
	if (!(dir_i = namei(dir_name)))
		return -ENOENT;
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO)
	{
		iput(dir_i);
		return -EBUSY;
	}
	if (!S_ISDIR(dir_i->i_mode))
	{
		iput(dir_i);
		return -EPERM;
	}
	if (!(sb = read_super(dev)))
	{
		iput(dir_i);
		return -EBUSY;
	}
	if (sb->s_imount)
	{
		iput(dir_i);
		return -EBUSY;
	}
	if (dir_i->i_mount)
	{
		iput(dir_i);
		return -EPERM;
	}
	sb->s_imount = dir_i;
	dir_i->i_mount = 1;
	dir_i->i_dirt = 1; /* NOTE! we don't iput(dir_i) */
	return 0;		   /* we do that in umount */
}

void mount_root(void)
{
	int i, free;
	struct super_block *p;
	struct m_inode *mi;

	if (32 != sizeof(struct d_inode))
		panic("bad i-node size");
	for (i = 0; i < NR_FILE; i++)
		file_table[i].f_count = 0;
	if (MAJOR(ROOT_DEV) == 2)
	{
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	for (p = &super_block[0]; p < &super_block[NR_SUPER]; p++)
	{
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL;
	}
	if (!(p = read_super(ROOT_DEV)))
		panic("Unable to mount root");
	if (!(mi = iget(ROOT_DEV, ROOT_INO)))
		panic("Unable to read root i-node");
	mi->i_count += 3; /* NOTE! it is logically used 4 times, not 1 */
	p->s_isup = p->s_imount = mi;
	current->pwd = mi;
	current->root = mi;
	free = 0;
	i = p->s_nzones;
	while (--i >= 0)
		if (!set_bit(i & 8191, p->s_zmap[i >> 13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r", free, p->s_nzones);
	free = 0;
	i = p->s_ninodes + 1;
	while (--i >= 0)
		if (!set_bit(i & 8191, p->s_imap[i >> 13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r", free, p->s_ninodes);
}
