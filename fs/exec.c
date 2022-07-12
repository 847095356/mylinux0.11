/*
 *  linux/fs/exec.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * #!-checking implemented by tytso.
 */

/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <a.out.h>

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/segment.h>

extern int sys_exit(int exit_code);
extern int sys_close(int fd);

/*
 * MAX_ARG_PAGES defines the number of pages allocated for arguments
 * and envelope for the new program. 32 should suffice, this gives
 * a maximum env+arg of 128kB !
 */
#define MAX_ARG_PAGES 32

/*
 * create_tables() parses the env- and arg-strings in new user
 * memory and creates the pointer tables from them, and puts their
 * addresses on the "stack", returning the new stack pointer value.
 */
static unsigned long *create_tables(char *p, int argc, int envc)
{
	unsigned long *argv, *envp;
	unsigned long *sp;

	sp = (unsigned long *)(0xfffffffc & (unsigned long)p);
	sp -= envc + 1;
	envp = sp;
	sp -= argc + 1;
	argv = sp;
	put_fs_long((unsigned long)envp, --sp);
	put_fs_long((unsigned long)argv, --sp);
	put_fs_long((unsigned long)argc, --sp);
	while (argc-- > 0)
	{
		put_fs_long((unsigned long)p, argv++);
		while (get_fs_byte(p++)) /* nothing */
			;
	}
	put_fs_long(0, argv);
	while (envc-- > 0)
	{
		put_fs_long((unsigned long)p, envp++);
		while (get_fs_byte(p++)) /* nothing */
			;
	}
	put_fs_long(0, envp);
	return sp;
}

/*
 * count() counts the number of arguments/envelopes
 */
static int count(char **argv)
{
	int i = 0;
	char **tmp;

	if (tmp = argv)
		while (get_fs_long((unsigned long *)(tmp++)))
			i++;

	return i;
}

/*
 * 'copy_string()' copies argument/envelope strings from user
 * memory to free pages in kernel mem. These are in a format ready
 * to be put directly into the top of new user memory.
 *
 * Modified by TYT, 11/24/91 to add the from_kmem argument, which specifies
 * whether the string and the string array are from user or kernel segments:
 *
 * from_kmem     argv *  (参数指针，也就是参数地址)      		argv **		(真正的用户输入参数)
 *    0          user space    								user space
 *    1          kernel space  								user space
 *    2          kernel space  								kernel space
 *
 * We do this by playing games with the fs segment register.  Since it
 * it is expensive to load a segment register, we try to avoid calling
 * set_fs() unless we absolutely have to.
 */
//// 复制指定个数的参数字符串到参数和环境空间中。
// 参数：argc - 欲添加参数个数; argv - 参数指针数组；page - 参数和环境空间页面
// 指针数组。p - 参数表空间中偏移指针，始终指向已复制串的头部；from_kmem - 字符
// 串来源标志。在 do_execve()函数中，p初始化为指向参数表(128kb)空间的最后一个长
// 字处，参数字符串是以堆栈操作方式逆向往其中复制存放的。因此p指针会随着复制信
// 息的增加而逐渐减小，并始终指向参数字符串的头部。字符串来源标志from_kmem应该
// 是TYT为了给execve()增添执行脚本文件的功能而新加的参数。当没有运行脚本文件的
// 功能时，所有参数字符串都在用户数据空间中。
// 返回：参数和环境空间当前头部指针。若出错则返回0.
static unsigned long copy_strings(int argc, char **argv, unsigned long *page,
								  unsigned long p, int from_kmem)
{
	char *tmp, *pag;
	int len, offset = 0;
	unsigned long old_fs, new_fs;

	if (!p)
		return 0; /* bullet-proofing */
	new_fs = get_ds();
	old_fs = get_fs();
	if (from_kmem == 2)
		set_fs(new_fs);
	while (argc-- > 0)
	{
		if (from_kmem == 1)
			set_fs(new_fs);
		if (!(tmp = (char *)get_fs_long(((unsigned long *)argv) + argc)))
			panic("argc is wrong");
		if (from_kmem == 1)
			set_fs(old_fs);
		len = 0; /* remember zero-padding */
		do
		{
			len++;
		} while (get_fs_byte(tmp++));
		if (p - len < 0)
		{ /* this shouldn't happen - 128kB */
			set_fs(old_fs);
			return 0;
		}
		while (len)
		{
			--p;
			--tmp;
			--len;
			if (--offset < 0)
			{
				offset = p % PAGE_SIZE;
				if (from_kmem == 2)
					set_fs(old_fs);
				if (!(pag = (char *)page[p / PAGE_SIZE]) &&
					!(pag = (char *)page[p / PAGE_SIZE] =
						  (unsigned long *)get_free_page()))
					return 0;
				if (from_kmem == 2)
					set_fs(new_fs);
			}
			*(pag + offset) = get_fs_byte(tmp);
		}
	}
	if (from_kmem == 2)
		set_fs(old_fs);
	return p;
}

static unsigned long change_ldt(unsigned long text_size, unsigned long *page)
{
	unsigned long code_limit, data_limit, code_base, data_base;
	int i;

	code_limit = text_size + PAGE_SIZE - 1;
	code_limit &= 0xFFFFF000;
	data_limit = 0x4000000;
	code_base = get_base(current->ldt[1]);
	data_base = code_base;
	set_base(current->ldt[1], code_base);
	set_limit(current->ldt[1], code_limit);
	set_base(current->ldt[2], data_base);
	set_limit(current->ldt[2], data_limit);
	/* make sure fs points to the NEW data segment */
	__asm__("pushl $0x17\n\tpop %%fs" ::);
	data_base += data_limit;
	for (i = MAX_ARG_PAGES - 1; i >= 0; i--)
	{
		data_base -= PAGE_SIZE;
		if (page[i])
			put_page(page[i], data_base);
	}
	return data_limit;
}

/*
 * 'do_execve()' executes a new program.
 */
//// execve()系统中断调用函数。加载并执行子进程
// 该函数是系统中断调用（int 0x80）功能号__NR_execve调用的函数。函数的参数是进
// 入系统调用处理过程后直接到调用本系统嗲用处理过程和调用本函数之前逐步压入栈中
// 的值。
// eip - 调用系统中断的程序代码指针。
// tmp - 系统中断中在调用_sys_execve时的返回地址，无用；
// filename - 被执行程序文件名指针；
// argv - 命令行参数指针数组的指针；
// envp - 环境变量指针数组的指针。
// 返回：如果调用成功，则不返回；否则设置出错号，并返回-1.
/*
    ./add  1  2  3  
    ./run.sh  helloworld
*/
int do_execve(unsigned long *eip, long tmp, char *filename,
			  char **argv, char **envp)
{
	struct m_inode *inode;
	struct buffer_head *bh;
	struct exec ex;
	unsigned long page[MAX_ARG_PAGES];
	int i, argc, envc; //循环索引，参数个数，环境变量个数
	int e_uid, e_gid;
	int retval;
	int sh_bang = 0; // 控制是否需要执行的脚本程序
	unsigned long p = PAGE_SIZE * MAX_ARG_PAGES - 4;

	// eip[1]是调用本次系统调用的原用户程序代码段寄存器cs值。其中的段选择符，当然必须是当前任务的代码段选择符(0x000f)若不是该值，那么cs只能是内核代码段的选择符(0x0008)。但这是绝对不允许的，因为内核代码常驻内存是不能被替换掉的
	if ((0xffff & eip[1]) != 0x000f)
		panic("execve called from supervisor mode");
	for (i = 0; i < MAX_ARG_PAGES; i++) /*清空page-table 用于存放命令行参数和环境变量 clear page-table */
		page[i] = 0;
	if (!(inode = namei(filename))) /* get executables inode */
		return -ENOENT;
	argc = count(argv); // numbers of arguments
	envc = count(envp); // numbers of envelopes

restart_interp:
	if (!S_ISREG(inode->i_mode)) // file type inspect
	{							 /* must be regular file */
		retval = -EACCES;
		goto exec_error2;
	}
	i = inode->i_mode; // file permission inspect
	e_uid = (i & S_ISUID) ? inode->i_uid : current->euid;
	e_gid = (i & S_ISGID) ? inode->i_gid : current->egid;
	if (current->euid == inode->i_uid)
		i >>= 6;
	else if (current->egid == inode->i_gid)
		i >>= 3;
	if (!(i & 1) &&
		!((inode->i_mode & 0111) && suser()))
	{
		retval = -ENOEXEC;
		goto exec_error2;
	}
	if (!(bh = bread(inode->i_dev, inode->i_zone[0])))
	{
		retval = -EACCES;
		goto exec_error2;
	}
	//赋值高速缓冲区块的数据到ex中
	ex = *((struct exec *)bh->b_data); /* read exec-header */
	//认定为shell脚本
	//如果(执行文件开始的两个字节为#!)说明是一个脚本执行文件
	//想要运行一个脚本文件，就需要执行脚本文件的解释程序 （如shell程序）
	//通常脚本程序的的第一行文本是 #!/bin/bash。它指明了要运行的解释程序
	//运行方式是从脚本的第一行(带字符'#!')中取出的解释程序名以及后面的参数(若存在)
	//然后将这些参数和脚本文件名放进执行文件(此时为解释程序)的命令行参数空间中
	//在这之前我们当然需要把函数指定的原有命令行参数和环境字符串放到128kb空间中，而这里建立的命令行参数则放到他们前面的位置（逆向放入的）
	//最后让内核执行脚本文件的解释程序
	//下面就是在设置好解释程序的脚本文件名等参数后，取出解释程序的i接地那并跳转restart_interp:去执行解释程序
	//由于需要跳转执行，因此在下面确认并处理脚本文件之后需要设置一个进制再次执行下面脚本处理的标志位sh_bang
	//后面的代码标志中该标志也用来表示我们已经设置好执行文件命令行参数，不需要重复设置
	if ((bh->b_data[0] == '#') && (bh->b_data[1] == '!') && (!sh_bang))
	{
		/*
		 * This section does the #! interpretation.
		 * Sorta complicated, but hopefully it will work.  -TYT
		 */

		char buf[1023], *cp, *interp, *i_name, *i_arg;
		unsigned long old_fs;

		strncpy(buf, bh->b_data + 2, 1022);
		brelse(bh);
		iput(inode);
		buf[1022] = '\0';
		if (cp = strchr(buf, '\n'))
		{
			*cp = '\0';
			for (cp = buf; (*cp == ' ') || (*cp == '\t'); cp++)
				;
		}
		if (!cp || *cp == '\0')
		{
			retval = -ENOEXEC; /* No interpreter name found */
			goto exec_error1;
		}
		//此时我们得到脚本解释程序的第一行内容  例如/bin/bash
		//首先取第一个字符串，它应该是解释程序名，此时i_name指向该名称
		//若解释程序名后还有其他字符，则他们应该是解释程序的参数，用i_arg指向该串
		interp = i_name = cp;
		i_arg = 0;
		//遍历字符串 cp不为空格并且不是制表符
		for (; *cp && (*cp != ' ') && (*cp != '\t'); cp++)
		{
			if (*cp == '/') //如果遇到"/"就更新i_name的指向
				i_name = cp + 1;
		}
		//当前*cp不是NULL，则为空格或者是\t，说明后面是有参数
		if (*cp)
		{
			*cp++ = '\0'; //让*cp (空格或制表符) = NULL , 然后cp++，这是为了cp指向了参数的首地址
			i_arg = cp;
		}
		/*
		 * OK, we've parsed out the interpreter name and
		 * (optional) argument.
		 */
		//现在把解析出来的程序名i_name 和 i_arg和脚本文件名作为解释程序参数放到环境和参数中
		//不过需要把函数提供的原来的参数和环境字符串先放进去，然后再放解析出来的
		//例如，对于命令行参数来说，如果原来的参数“-arg1 -arg2”，脚本名为example.sh  解释程序名为"bash" 其参数为"-iarg1, -iarg2"
		//那么在放入这里参数后，新的命令行类似于 "bash -iarg1, -iarg2 example.sh -arg1 -arg2"
		//用sh_bang来作为是否第一次执行shell的状态
		if (sh_bang++ == 0)
		{
			p = copy_strings(envc, envp, page, p, 0);
			p = copy_strings(--argc, argv + 1, page, p, 0);
		}
		/*
		 * Splice in (1) the interpreter's name for argv[0]
		 *           (2) (optional) argument to interpreter
		 *           (3) filename of shell script
		 *
		 * This is done in reverse order, because of how the
		 * user environment and arguments are stored.
		 */
		p = copy_strings(1, &filename, page, p, 1);
		argc++;
		if (i_arg)
		{
			p = copy_strings(1, &i_arg, page, p, 2);
			argc++;
		}
		p = copy_strings(1, &i_name, page, p, 2);
		argc++;
		if (!p)
		{
			retval = -ENOMEM;
			goto exec_error1;
		}
		/*
		 * OK, now restart the process with the interpreter's inode.
		 */
		old_fs = get_fs();
		set_fs(get_ds());
		if (!(inode = namei(interp)))
		{ /* get executables inode */
			set_fs(old_fs);
			retval = -ENOENT;
			goto exec_error1;
		}
		set_fs(old_fs);
		goto restart_interp;
	}
	brelse(bh);
	if (N_MAGIC(ex) != ZMAGIC || ex.a_trsize || ex.a_drsize ||
		ex.a_text + ex.a_data + ex.a_bss > 0x3000000 ||
		inode->i_size < ex.a_text + ex.a_data + ex.a_syms + N_TXTOFF(ex))
	{
		retval = -ENOEXEC;
		goto exec_error2;
	}
	if (N_TXTOFF(ex) != BLOCK_SIZE)
	{
		printk("%s: N_TXTOFF != BLOCK_SIZE. See a.out.h.", filename);
		retval = -ENOEXEC;
		goto exec_error2;
	}
	if (!sh_bang)
	{
		p = copy_strings(envc, envp, page, p, 0);
		p = copy_strings(argc, argv, page, p, 0);
		if (!p)
		{
			retval = -ENOMEM;
			goto exec_error2;
		}
	}
	/* OK, This is the point of no return */
	if (current->executable)
		iput(current->executable);
	current->executable = inode;
	for (i = 0; i < 32; i++) //清空所有的信号handler
		current->sigaction[i].sa_handler = NULL;
	for (i = 0; i < NR_OPEN; i++) //关闭所有打开的文件
		if ((current->close_on_exec >> i) & 1)
			sys_close(i);
	current->close_on_exec = 0;
	free_page_tables(get_base(current->ldt[1]), get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]), get_limit(0x17));
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	current->used_math = 0;
	p += change_ldt(ex.a_text, page) - MAX_ARG_PAGES * PAGE_SIZE;
	p = (unsigned long)create_tables((char *)p, argc, envc);
	current->brk = ex.a_bss +
				   (current->end_data = ex.a_data +
										(current->end_code = ex.a_text));
	current->start_stack = p & 0xfffff000;
	current->euid = e_uid;
	current->egid = e_gid;
	i = ex.a_text + ex.a_data;
	while (i & 0xfff)
		put_fs_byte(0, (char *)(i++));
	eip[0] = ex.a_entry; /* eip, magic happens :-) */
	eip[3] = p;			 /* stack pointer */
	return 0;
exec_error2:
	iput(inode);
exec_error1:
	for (i = 0; i < MAX_ARG_PAGES; i++)
		free_page(page[i]);
	return (retval);
}