/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

volatile void do_exit(long code);

static inline volatile void oom(void)
{
	printk("out of memory\n\r");
	// do_exit应该使用退出代码，这里用了信号值SIGSEGV(11)相同值的出错码含义是
	// “资源暂时不可用”，正好同义。
	do_exit(SIGSEGV);
}

#define invalidate() \
	__asm__("movl %%eax,%%cr3" ::"a"(0)) // 刷新页变换高速缓冲。

/* these are not to be changed without changing head.s etc */
#define LOW_MEM 0x100000
#define PAGING_MEMORY (15 * 1024 * 1024)
#define PAGING_PAGES (PAGING_MEMORY >> 12)
#define MAP_NR(addr) (((addr)-LOW_MEM) >> 12)
#define USED 100

#define CODE_SPACE(addr) ((((addr) + 4095) & ~4095) < \
						  current->start_code + current->end_code)

static long HIGH_MEMORY = 0;

#define copy_page(from, to)                                     \
	__asm__("cld ; rep ; movsl" ::"S"(from), "D"(to), "c"(1024) \
			: "cx", "di", "si")

static unsigned char mem_map[PAGING_PAGES] = {
	0,
};

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 */
// 在主内存区中取空闲屋里页面。如果已经没有可用物理内存页面，则返回0.
// 输入：%1(ax=0) - 0; %2(LOW_MEM)内存字节位图管理的其实位置；%3(cx=PAGING_PAGES);
// %4(edi=mem_map+PAGING_PAGES-1).
// 输出：返回%0(ax=物理内存页面起始地址)。
// 上面%4寄存器实际指向mem_map[]内存字节位图的最后一个字节。本函数从位图末端开
// 始向前扫描所有页面标志（页面总数PAGING_PAGE），若有页面空闲（内存位图字节为
// 0）则返回页面地址。注意！本函数只是指出在主内存区的一页空闲物理内存页面，但
// 并没有映射到某个进程的地址空间中去。后面的put_page()函数即用于把指定页面映射
// 到某个进程地址空间中。当然对于内核使用本函数并不需要再使用put_page()进行映射，
// 因为内核代码和数据空间（16MB）已经对等地映射到物理地址空间。
unsigned long get_free_page(void)
{
	register unsigned long __res asm("ax");

	__asm__("std ; repne ; scasb\n\t"
			"jne 1f\n\t"
			"movb $1,1(%%edi)\n\t"
			"sall $12,%%ecx\n\t"
			"addl %2,%%ecx\n\t"
			"movl %%ecx,%%edx\n\t"
			"movl $1024,%%ecx\n\t"
			"leal 4092(%%edx),%%edi\n\t"
			"rep ; stosl\n\t"
			"movl %%edx,%%eax\n"
			"1:"
			: "=a"(__res)
			: "0"(0), "i"(LOW_MEM), "c"(PAGING_PAGES),
			  "D"(mem_map + PAGING_PAGES - 1)
			: "di", "cx", "dx");
	return __res;
}

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
//// 释放物理地址addr开始的1页面内存。
// 物理地址1MB以下的内容空间用于内核程序和缓冲，不作为分配页面的内存空间。因此
// 参数addr需要大于1MB.
void free_page(unsigned long addr)
{
	// 首先判断参数给定的物理地址addr的合理性。如果物理地址addr小于内存低端(1MB)
	// 则表示在内核程序或高速缓冲中，对此不予处理。如果物理地址addr>=系统所含物
	// 理内存最高端，则显示出错信息并且内核停止工作。
	if (addr < LOW_MEM)
		return;
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
	// 如果对参数addr验证通过，那么就根据这个物理地址换算出从内存低端开始记起的
	// 内存页面号。页面号 ＝ (addr - LOW_MEM)/4096.可见页面号从0号开始记起。此时
	// addr中存放着页面号。如果该页面号对应的页面映射字节不等于0，则减1返回。此
	// 时该映射字节值应该为0，表示页面已释放。如果对应页面字节原本就是0，表示该
	// 物理页面本来就是空闲的，说明内核代码出问题。于是显示出错信息并停机。
	addr -= LOW_MEM;
	addr >>= 12;
	if (mem_map[addr]--)
		return;
	mem_map[addr] = 0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
//找到页目录的基址，然后挨个遍历；遍历时找到页表项，进而找到使用的物理内存，挨个释放！　

//// 根据指定的线性地址和限长(页表个数)，释放对应内存页表指定的内存块并置表项为
// 空闲。页目录位于物理地址0开始处，共1024项，每项4字节，共占4K字节。每个目录项
// 指定一个页表。内核页表从物理地址0x1000处开始(紧接着目录空间)，共4个页表。每
// 个页表有1024项，每项4字节。因此占4K（1页）内存。各进程（除了在内核代码中的进
// 程0和1）的页表所占据的页面在进程被创建时由内核为其主内存区申请得到。每个页表
// 项对应1页物理内存，因此一个页表最多可映射4MB的物理内存。
// 参数：from - 起始线性基地址；size - 释放的字节长度。
int free_page_tables(unsigned long from, unsigned long size)
{
	unsigned long *pg_table;
	unsigned long *dir, nr;

	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
	// 然后计算参数size给出的长度所占的页目录项数（4MB的进位整数倍），也即所占
	// 页表数。因为1个页表可管理4MB物理内存，所以这里用右移22位的方式把需要复制
	// 的内存长度值除以4MB.其中加上0x3fffff(即4MB-1)用于得到进位整数倍结果，即
	// 除操作若有余数则进1。例如，如果原size=4.01Mb，那么可得到结果sieze=2。接
	// 着结算给出的线性基地址对应的其实目录项。对应的目录项号＝from>>22.因为每
	// 项占4字节，并且由于页目录表从物理地址0开始存放，因此实际目录项指针＝目录
	// 项号<<2，也即(from>>20)。& 0xffc确保目录项指针范围有效，即用于屏蔽目录项
	// 指针最后2位。因为只移动了20位，因此最后2位是页表项索引的内容，应屏蔽掉。
	size = (size + 0x3fffff) >> 22;
	dir = (unsigned long *)((from >> 20) & 0xffc); /* _pg_dir = 0 */
	// 此时size是释放的页表个数，即页目录项数，而dir是起始目录项指针。现在开始
	// 循环操作页目录项，依次释放每个页表中的页表项。如果当前目录项无效（P位＝0）
	// 表示该目录项没有使用(对应的页表不存在)，则继续处理下一个目录项。否则从目
	// 录项总取出页表地址pg_table，并对该页表中的1024个表项进行处理。释放有效页
	// 表项(P位＝1)对应的物理内存页表。然后该页表项清零，并继续处理下一页表项。
	// 当一个页表所有表项都处理完毕就释放该页表自身占据的内存页面，并继续处理下
	// 一页目录项。最后刷新也页变换高速缓冲，并返回0.
	for (; size-- > 0; dir++)
	{
		if (!(1 & *dir))
			continue;
		pg_table = (unsigned long *)(0xfffff000 & *dir); // 取页表地址
		for (nr = 0; nr < 1024; nr++)
		{
			if (1 & *pg_table) // 若该项有效，则释放对应页。
				free_page(0xfffff000 & *pg_table);
			*pg_table = 0; // 该页表项内容清零。
			pg_table++;	   // 指向页表中下一项。
		}
		free_page(0xfffff000 & *dir); // 释放该页表所占内存页面。
		*dir = 0;					  // 对应页表的目录项清零
	}
	invalidate(); // 刷新页变换高速缓冲。
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */
//// 复制页目录表项和页表项：暂时不拷贝具体的页，提高进程fork的效率
// 复制指定线性地址和长度内存对应的页目录项和页表项，从而被复制的页目录和页表对
// 应的原物理内存页面区被两套页表映射而共享使用。复制时，需申请新页面来存放新页
// 表，原物理内存区将被共享。此后两个进程（父进程和其子进程）将共享内存区，直到
// 有一个进程执行写操作时，内核才会为写操作进程分配新的内存页(写时复制机制)。
// 参数from、to是线性地址，size是需要复制（共享）的内存长度，单位是byte.
int copy_page_tables(unsigned long from, unsigned long to, long size)
{
	unsigned long *from_page_table;
	unsigned long *to_page_table;
	unsigned long this_page;
	unsigned long *from_dir, *to_dir;
	unsigned long nr;

	// 首先检测参数给出的原地址from和目的地址to的有效性。原地址和目的地址都需要
	// 在4Mb内存边界地址上。否则出错死机。作这样的要求是因为一个页表的1024项可
	// 管理4Mb内存。源地址from和目的地址to只有满足这个要求才能保证从一个页表的
	// 第一项开始复制页表项，并且新页表的最初所有项都是有效的。然后取得源地址和
	// 目的地址的起始目录项指针(from_dir 和 to_dir).再根据参数给出的长度size计
	// 算要复制的内存块占用的页表数(即目录项数)。
	if ((from & 0x3fffff) || (to & 0x3fffff)) //检测是否是从4MB边缘开始的
		panic("copy_page_tables called with wrong alignment");
	// from_dir 和 to_dir 都是二级目录的地址
	from_dir = (unsigned long *)((from >> 20) & 0xffc); /* _pg_dir = 0 */
	to_dir = (unsigned long *)((to >> 20) & 0xffc);
	//二级目录的索引个数
	size = ((unsigned)(size + 0x3fffff)) >> 22;
	// 在得到了源起始目录项指针from_dir和目的起始目录项指针to_dir以及需要复制的
	// 页表个数size后，下面开始对每个页目录项依次申请1页内存来保存对应的页表，并
	// 且开始页表项复制操作。如果目的目录指定的页表已经存在(P=1)，则出错死机。
	// 如果源目录项无效，即指定的页表不存在(P=1),则继续循环处理下一个页目录项。
	for (; size-- > 0; from_dir++, to_dir++)
	{
		if (1 & *to_dir) //
			panic("copy_page_tables: already exist");
		if (!(1 & *from_dir)) //本身不存在跳过
			continue;
		// 在验证了当前源目录项和目的项正常之后，我们取源目录项中页表地址
		// from_page_table。为了保存目的目录项对应的页表，需要在住内存区中申请1
		// 页空闲内存页。如果取空闲页面函数get_free_page()返回0，则说明没有申请
		// 到空闲内存页面，可能是内存不够。于是返回-1值退出。
		from_page_table = (unsigned long *)(0xfffff000 & *from_dir);
		if (!(to_page_table = (unsigned long *)get_free_page()))
			return -1; /* Out of memory, see freeing */
		// 否则我们设置目的目录项信息，把最后3位置位，即当前目录的目录项 | 7，
		// 表示对应页表映射的内存页面是用户级的，并且可读写、存在(Usr,R/W,Present).
		// (如果U/S位是0，则R/W就没有作用。如果U/S位是1，而R/W是0，那么运行在用
		// 户层的代码就只能读页面。如果U/S和R/W都置位，则就有读写的权限)。然后
		// 针对当前处理的页目录项对应的页表，设置需要复制的页面项数。如果是在内
		// 核空间，则仅需复制头160页对应的页表项(nr=160),对应于开始640KB物理内存
		// 否则需要复制一个页表中的所有1024个页表项(nr=1024)，可映射4MB物理内存。
		*to_dir = ((unsigned long)to_page_table) | 7;
		nr = (from == 0) ? 0xA0 : 1024;
		// 此时对于当前页表，开始循环复制指定的nr个内存页面表项。先取出源页表的
		// 内容，如果当前源页表没有使用，则不用复制该表项，继续处理下一项。否则
		// 复位表项中R/W标志(位1置0)，即让页表对应的内存页面只读。然后将页表项复制
		// 到目录页表中。
		for (; nr-- > 0; from_page_table++, to_page_table++)
		{
			this_page = *from_page_table;
			if (!(1 & this_page))
				continue;
			this_page &= ~2; //将R/W位置0，让对应内存页只读
			*to_page_table = this_page;
			if (this_page > LOW_MEM)
			{
				*from_page_table = this_page;
				this_page -= LOW_MEM;
				this_page >>= 12;
				mem_map[this_page]++;
			}
		}
	}
	invalidate(); // 刷新页变换高速缓冲。
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */
//// 把一物理内存页面映射到线性地址空间指定处。
// 或者说是把线性地址空间中指定地址address出的页面映射到主内存区页面page上。主
// 要工作是在相关页面目录项和页表项中设置指定页面的信息。若成功则返回物理页面地
// 址。在处理缺页异常的C函数do_no_page()中会调用此函数。对于缺页引起的异常，由于
// 任何缺页缘故而对页表作修改时，并不需要刷新CPU的页变换缓冲(或称Translation Lookaside
// Buffer - TLB),即使页表中标志P被从0修改成1.因为无效叶项不会被缓冲，因此当修改
// 了一个无效的页表项时不需要刷新。在次就表现为不用调用Invalidate()函数。
// 参数page是分配的主内存区中某一页面(页帧，页框)的指针;address是线性地址。
unsigned long put_page(unsigned long page, unsigned long address)
{
	unsigned long tmp, *page_table;

	/* NOTE !!! This uses the fact that _pg_dir=0 */

	// 首先判断参数给定物理内存页面page的有效性。如果该页面位置低于LOW_MEM（1MB）
	// 或超出系统实际含有内存高端HIGH_MEMORY，则发出警告。LOW_MEM是主内存区可能
	// 有的最小起始位置。当系统物理内存小于或等于6MB时，主内存区起始于LOW_MEM处。
	// 再查看一下该page页面是否已经申请的页面，即判断其在内存页面映射字节图mem_map[]
	// 中相应字节是否已经置位。若没有则需发出警告。
	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n", page, address);
	if (mem_map[(page - LOW_MEM) >> 12] != 1)
		printk("mem_map disagrees with %p at %p\n", page, address);
	// 然后根据参数指定的线性地址address计算其在也目录表中对应的目录项指针，并
	// 从中取得二级页表地址。如果该目录项有效(P=1),即指定的页表在内存中，则从中
	// 取得指定页表地址放到page_table 变量中。否则就申请一空闲页面给页表使用，并
	// 在对应目录项中置相应标志(7 - User、U/S、R/W).然后将该页表地址放到page_table
	// 变量中。
	page_table = (unsigned long *)((address >> 20) & 0xffc);
	if ((*page_table) & 1)
		page_table = (unsigned long *)(0xfffff000 & *page_table);
	else
	{
		if (!(tmp = get_free_page()))
			return 0;
		*page_table = tmp | 7;
		page_table = (unsigned long *)tmp;
	}
	// 最后在找到的页表page_table中设置相关页表内容，即把物理页面page的地址填入
	// 表项同时置位3个标志(U/S、W/R、P)。该页表项在页表中索引值等于线性地址位21
	// -- 位12组成的10bit的值。每个页表共可有1024项(0 -- 0x3ff)。
	page_table[(address >> 12) & 0x3ff] = page | 7;
	/* no need for invalidate */
	return page;
}

void un_wp_page(unsigned long *table_entry)
{
	unsigned long old_page, new_page;

	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)] == 1)
	{
		*table_entry |= 2;
		invalidate();
		return;
	}
	if (!(new_page = get_free_page()))
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	*table_entry = new_page | 7;
	invalidate();
	copy_page(old_page, new_page);
}

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */
void do_wp_page(unsigned long error_code, unsigned long address)
{
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
	un_wp_page((unsigned long *)(((address >> 10) & 0xffc) + (0xfffff000 &
															  *((unsigned long *)((address >> 20) & 0xffc)))));
}

void write_verify(unsigned long address)
{
	unsigned long page;

	if (!((page = *((unsigned long *)((address >> 20) & 0xffc))) & 1))
		return;
	page &= 0xfffff000;
	page += ((address >> 10) & 0xffc);
	if ((3 & *(unsigned long *)page) == 1) /* non-writeable, present */
		un_wp_page((unsigned long *)page);
	return;
}

void get_empty_page(unsigned long address)
{
	unsigned long tmp;

	if (!(tmp = get_free_page()) || !put_page(tmp, address))
	{
		free_page(tmp); /* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable.
 */
//为了节约物理内存，不同进程可能会共享同样的物理页面，举个例子：
//同时打开两个notepad，操作系统会同时生成两个进程，但由于运行的是同样的程序，
//最起码代码段是可以共享的，所以这两个进程的代码段是可以设置成一样的！
// linux设置共享物理页的方式如下：
// 因为共享的肯定是物理页面，所以要先根据线性地址算出物理地址；
// 修改目标进程的页表项，让某个页表项保存物理页起始地址（这种物理页的挂载思路在windows下叫shadow walker，可以用来过PG保护的）；

//// 尝试对当前进程指定地址处的页面进行共享处理。
// 当前进程与进程p是同一执行代码，也可以认为当前进程是由p进程执行fork操作产生的
// 进程，因此它们的代码内容一样。如果未对数据段内容做过修改那么数据段内容也应一
// 样。参数address是进程中的逻辑地址，即是当前进程欲与p进程共享页面的逻辑页面地
// 址。进程P是将被共享页面的进程。如果P进程address处的页面存在并且没有被修改过的
// 话，就让当前进程与p进程共享之。同时还需要验证指定地址处是否已经申请了页面，若
// 是则出错，死机。返回：1 - 页面共享处理成功；0 - 失败。
static int try_to_share(unsigned long address, struct task_struct *p)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

	// 首先分别求的指定进程p中和当前进程中逻辑地址address对应的页目录项。为了计
	// 算方便先求出指定逻辑地址address出的'逻辑'页目录项号，即以进程空间(0 - 64 MB)
	// 算出的页目录项号。该'逻辑'页目录项号加上进程p在CPU 4G线性空间中的实际页目
	// 录项from_page。而'逻辑'页目录项号加上当前进程CPU 4G线性空间中起始地址对应
	// 的页目录项，即可最后得到当前进程中地址address处页面所对应的4G线性空间中的
	// 实际页目录项to_page。
	from_page = to_page = ((address >> 20) & 0xffc);
	from_page += ((p->start_code >> 20) & 0xffc);
	to_page += ((current->start_code >> 20) & 0xffc);
	// 在得到p进程和当前进程address对应的目录项后，下面分别对进程p和当前进程进行
	// 处理。下面首先对p进程的表项进行操作。目标是取得p进程中address对应的物理内
	// 存页面地址，并且该物理页面存在，而且干净(没有被修改过)。
	// 方法是先取目录项内容。如果该目录项无效(P=0),表示目录项对应的二级页表不存
	// 在，于是返回。否则取该目录项对应页表地址from，从而计算出逻辑地址address
	// 对应的页表项指针，并取出该页表项内容临时保存在phys_addr中。
	/* is there a page-directory at from? */
	from = *(unsigned long *)from_page; //页表指针
	if (!(from & 1))
		return 0;
	from &= 0xfffff000;							  // 4k对齐
	from_page = from + ((address >> 10) & 0xffc); //其实是先右移12再左移2 但是2位没移动置0
	phys_addr = *(unsigned long *)from_page;	  //物理页地址
	// 接着看看页表项映射的物理页面是否存在并且干净。0x41对应页表项中的D(dirty)
    // 和P（present）标志。如果页面不干净或无效则返回。然后我们从该表项中取出物
    // 理页面地址再保存在phys_addr中。最后我们再检查一下这个物理页面地址的有效性，
    // 即它不应该超过机器最大物理地址值，也不应该小于内存低端(1 MB).
	/* is the page clean and present? */
	if ((phys_addr & 0x41) != 0x01)
		return 0;
	phys_addr &= 0xfffff000;
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
		return 0;
	// 下面首先对当前进程的表项进行操作。目标是取得当前进程中address对应的页表
    // 项地址，并且该页表项还没有映射物理页面，即其P=0。
    // 首先取当前进程页目录项内容->to.如果该目录项无效(P=0),即目录项对应的二级
    // 页表不存在，则申请一空闲页面来存放页表，并更新目录项to_page内容，让其指向
    // 内存页面。
	to = *(unsigned long *)to_page;
	if (!(to & 1))
		if (to = get_free_page())
			*(unsigned long *)to_page = to | 7;
		else
			oom();
	// 否则取目录项中的页表地址->to，加上页表项索引值<<2，即页表项在表中偏移地址，
    // 得到页表地址->to_page.针对页表项，如果我们此时我们检查出其对应的物理页面
    // 已经存在，即页表的存在位P=1，则说明原本我们想共享进程p中对应的物理页面，
    // 但现在我们自己已经占有了(映射有)物理页面。于是说明内核出错，死机。
	to &= 0xfffff000;
	to_page = to + ((address >> 10) & 0xffc);
	if (1 & *(unsigned long *)to_page)
		panic("try_to_share: to_page already exists");
	// 在找到了进程p中逻辑地址address处对应的干净且存在的物理页面，而且也确定了
    // 当前进程中逻辑地址address所对应的耳机页表项地址之后，我们现在对他们进行
    // 共享处理。方法很简单，就是首先对p进程的页表项进行修改，设置其写保护(R/W=0,
    // 只读)标志，然后让当前进程复制p进程的这个页表项。此时当前进程逻辑地址address
    // 处页面即被映射到p进程逻辑地址address处页面映射的物理页面上。
	/* share them: write-protect */
	*(unsigned long *)from_page &= ~2;
	*(unsigned long *)to_page = *(unsigned long *)from_page;
	// 随后刷新页变换高速缓冲。计算所操作屋里页面的页面号，并将对应页面映射字节数
    // 组项中的引用递增1。最后返回1，表示共享处理成功。
	invalidate();
	phys_addr -= LOW_MEM;
	phys_addr >>= 12;
	mem_map[phys_addr]++;
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
//（2）当发生缺页时，首先看看有没有运行同样文件的进程；
// 如果有，先共享一下改进程的物理页面，达到节约内存的目的：注意这里面有个count字段，可以用来检测app多开的！由此也引申出了另一个沙箱的概念：虚拟出另一块内存，但是count就是1，避开检测！

//// 共享页面处理。
// 在发生缺页异常时，首先看看能否与运行同一个执行文件的其他进程进行页面共享处理
// 该函数首先判断系统是否有另一个进程也在运行当前进程一样的执行文件。若有，则在
// 系统当前所有任务中寻找这样的任务。若找到了这样的任务就尝试与其共享指定地址处
// 的页面。若系统中没有其他任务正在运行与当前进程相同的执行文件，那么共享页面操
// 作的前提条件不存在，因此函数立刻退出。判断系统中是否有另一个进程也在执行同一
// 个执行文件的方法是零用进程任务数据结构中的executable字段。该字段指向进程正在
// 执行程序在内存中的i节点。根据该i节点的引用次数i_count我们可以进行这种判断。若
// executable->i_count值大于1，则表明系统中可能有两个进程运行同一个执行文件，于
// 是可以再对task struct数组中所有任务比较是否有相同的executable字段来最后确定多个
// 进程运行着相同执行文件的情况。
// 参数address是进程中的逻辑地址，即是当前进程欲与p进程共享页面的逻辑页面地址。
// 返回：1 - 共享操作成功，0 - 失败。
static int share_page(unsigned long address)
{
	struct task_struct **p;

	// 首先检查一下当前进程的executable字段是否指向某执行文件的i节点，以判断本
	// 进程是否有对应的执行文件。如果没有，则返回0.如果executable的确指向某个i
	// 节点，则检查该i节点引用计数值。如果当前进程运行的执行文件的内存i节点引用
	// 计数等于1(executable->i_count=1),表示当前系统中只有1个进程(即当前进程)在
	// 运行该执行文件。因此无共享可言，直接退出函数。
	if (!current->executable)
		return 0;
	if (current->executable->i_count < 2)
		return 0;
	for (p = &LAST_TASK; p > &FIRST_TASK; --p)
	{
		if (!*p)
			continue;
		if (current == *p)
			continue;
		if ((*p)->executable != current->executable)
			continue;
		if (try_to_share(address, *p))
			return 1;
	}
	return 0;
}

void do_no_page(unsigned long error_code, unsigned long address)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block, i;

	address &= 0xfffff000;
	tmp = address - current->start_code;
	if (!current->executable || tmp >= current->end_data)
	{
		get_empty_page(address);
		return;
	}
	if (share_page(tmp))
		return;
	if (!(page = get_free_page()))
		oom();
	/* remember that 1 block is used for header */
	block = 1 + tmp / BLOCK_SIZE;
	for (i = 0; i < 4; block++, i++)
		nr[i] = bmap(current->executable, block);
	bread_page(page, current->executable->i_dev, nr);
	i = tmp + 4096 - current->end_data;
	tmp = page + 4096;
	while (i-- > 0)
	{
		tmp--;
		*(char *)tmp = 0;
	}
	if (put_page(page, address))
		return;
	free_page(page);
	oom();
}

void mem_init(long start_mem, long end_mem)
{
	int i;

	HIGH_MEMORY = end_mem;
	for (i = 0; i < PAGING_PAGES; i++)
		mem_map[i] = USED;
	i = MAP_NR(start_mem);
	end_mem -= start_mem;
	end_mem >>= 12;
	while (end_mem-- > 0)
		mem_map[i++] = 0;
}

void calc_mem(void)
{
	int i, j, k, free = 0;
	long *pg_tbl;

	for (i = 0; i < PAGING_PAGES; i++)
		if (!mem_map[i])
			free++;
	printk("%d pages free (of %d)\n\r", free, PAGING_PAGES);
	for (i = 2; i < 1024; i++)
	{
		if (1 & pg_dir[i])
		{
			pg_tbl = (long *)(0xfffff000 & pg_dir[i]);
			for (j = k = 0; j < 1024; j++)
				if (pg_tbl[j] & 1)
					k++;
			printk("Pg-dir[%d] uses %d pages\n", i, k);
		}
	}
}
