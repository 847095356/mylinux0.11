/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1 << ((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))
// nr就是pid
//这个函数就是用来打印pid号和state
void show_task(int nr, struct task_struct *p)
{
	int i, j = 4096 - sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, ", nr, p->pid, p->state);
	i = 0;
	while (i < j && !((char *)(p + 1))[i])
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r", i, j);
}

//辅助函数 打印当前所有的进程信息
void show_stat(void)
{
	int i;

	for (i = 0; i < NR_TASKS; i++)
		if (task[i])
			show_task(i, task[i]);
}

#define LATCH (1193180 / HZ)

extern void mem_use(void);

extern int timer_interrupt(void);
extern int system_call(void);

union task_union
{
	struct task_struct task;
	char stack[PAGE_SIZE];
};

static union task_union init_task = {
	INIT_TASK,
};

long volatile jiffies = 0;
long startup_time = 0;
struct task_struct *current = &(init_task.task); //全局变量 指向当前运行的进程
struct task_struct *last_task_used_math = NULL;

struct task_struct *task[NR_TASKS] = {
	&(init_task.task),
};

long user_stack[PAGE_SIZE >> 2];

struct
{
	long *a;
	short b;
} stack_start = {&user_stack[PAGE_SIZE >> 2], 0x10};
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */

//协处理器的恢复函数
//协处理器其实就想象成是CPU，切换进程时，协处理器的一些寄存器栈堆等数据也要进行切换
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");
	if (last_task_used_math)
	{
		__asm__("fnsave %0" ::"m"(last_task_used_math->tss.i387));
	}
	last_task_used_math = current;
	if (current->used_math)
	{
		__asm__("frstor %0" ::"m"(current->tss.i387));
	}
	else
	{
		__asm__("fninit" ::);
		current->used_math = 1;
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
// 时间片分配
void schedule(void)
{
	int i, next, c;
	struct task_struct **p;

	/* check alarm, wake up any interruptible tasks that have got a signal */

	for (p = &LAST_TASK; p > &FIRST_TASK; --p)
		if (*p)
		{											  // alarm是用来设置警告，比如jiffies有1000个可能其中一些需要警告那么就用alarm来实现
			if ((*p)->alarm && (*p)->alarm < jiffies) // alarm存在，并且已经到点
			{
				(*p)->signal |= (1 << (SIGALRM - 1)); //新增一个警告信号量
				(*p)->alarm = 0;					  //警告清空
			}
			//~(_BLOCKABLE & (*p)->blocked
			//用来排除非阻塞信号
			//如果该进程为可中断睡眠状态 则如果该进程有非屏蔽信号出现就将该进程的状态设置为running
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
				(*p)->state == TASK_INTERRUPTIBLE) //并且状态是可中断的
				(*p)->state = TASK_RUNNING;
		}

	/* this is the scheduler proper: */
	// 以下思路，循环task列表 根据counter大小决定进程切换
	while (1)
	{
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		while (--i)
		{
			if (!*--p)
				continue;										  //进程为空就继续循环
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c) //找出c最大的task
				c = (*p)->counter, next = i;
		}
		if (c)
			break; //如果c找到了，就终结循环，说明找到了
		//进行时间片的重新分配
		for (p = &LAST_TASK; p > &FIRST_TASK; --p)
			if (*p) //这里很关键，在低版本内核中，是进行优先级时间片轮转分配，这里搞清楚了优先级和时间片的关系
					// counter = counter/2 + priority
				(*p)->counter = ((*p)->counter >> 1) +
								(*p)->priority;
	}
	//切换到下一个进程 这个功能使用宏定义完成的
	switch_to(next);
}

int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

// 把当前任务置为不可中断的等待状态，并让睡眠队列指针指向当前任务。
// 只有明确的唤醒时才会返回。该函数提供了进程与中断处理程序之间的同步机制。函数参数P是等待
// 任务队列头指针。指针是含有一个变量地址的变量。这里参数p使用了指针的指针形式'**p',这是因为
// C函数参数只能传值，没有直接的方式让被调用函数改变调用该函数程序中变量的值。但是指针'*p'
// 指向的目标(这里是任务结构)会改变，因此为了能修改调用该函数程序中原来就是指针的变量的值，
// 就需要传递指针'*p'的指针，即'**p'.
// 当某个进程想访问CPU资源，但是CPU资源被占用访问不到，就会休眠
// 睡眠了 然后进程调度到别的进程中
void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p) //如果传进来的是空的 就返回
		return;
	// 若指针无效，则退出。(指针所指向的对象可以是NULL，但指针本身不应该为0).另外，如果
    // 当前任务是任务0，则死机。因为任务0的运行不依赖自己的状态，所以内核代码把任务0置为
    // 睡眠状态毫无意义。
	if (current == &(init_task.task))	  //当前进程是0号
		panic("task[0] trying to sleep"); //就打印并且返回
	// 让tmp指向已经在等待队列上的任务(如果有的话)，例如inode->i_wait.并且将睡眠队列头的
	// 等等指针指向当前任务。这样就把当前任务插入到了*p的等待队列中。然后将当前任务置为
	// 不可中断的等待状态，并执行重新调度。
	tmp = *p;
	*p = current; //这两步相当于 给休眠链表添加了一个新node
	// 其实核心就是把state置为TASK_UNINTERRUPTIBLE
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	// 只有当这个等待任务被唤醒时，调度程序才又返回到这里，表示本进程已被明确的唤醒(就
	// 续态)。既然大家都在等待同样的资源，那么在资源可用时，就有必要唤醒所有等待该该资源
	// 的进程。该函数嵌套调用，也会嵌套唤醒所有等待该资源的进程。这里嵌套调用是指一个
	// 进程调用了sleep_on()后就会在该函数中被切换掉，控制权呗转移到其他进程中。此时若有
	// 进程也需要使用同一资源，那么也会使用同一个等待队列头指针作为参数调用sleep_on()函数，
	// 并且也会陷入该函数而不会返回。只有当内核某处代码以队列头指针作为参数wake_up了队列，
	// 那么当系统切换去执行头指针所指的进程A时，该进程才会继续执行下面的代码，把队列后一个
	// 进程B置位就绪状态(唤醒)。而当轮到B进程执行时，它也才可能继续执行下面的代码。若它
	// 后面还有等待的进程C，那它也会把C唤醒等。在这前面还应该添加一行：*p = tmp.

	// 若在其前还有存在的等待的任务，则也将其置为就绪状态(唤醒).
	if (tmp)
		tmp->state = 0;
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;
repeat:
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current)
	{
		(**p).state = 0;
		goto repeat;
	}
	*p = NULL;
	if (tmp)
		tmp->state = 0;
}

void wake_up(struct task_struct **p)
{
	if (p && *p)
	{
		(**p).state = 0;
		*p = NULL;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct *wait_motor[4] = {NULL, NULL, NULL, NULL};
static int mon_timer[4] = {0, 0, 0, 0};
static int moff_timer[4] = {0, 0, 0, 0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr > 3)
		panic("floppy_on: nr>3");
	moff_timer[nr] = 10000; /* 100 s = very big :-) */
	cli();					/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected)
	{
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR)
	{
		outb(mask, FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ / 2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr + wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr] = 3 * HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i = 0; i < 4; i++, mask <<= 1)
	{
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i])
		{
			if (!--mon_timer[i])
				wake_up(i + wait_motor);
		}
		else if (!moff_timer[i])
		{
			current_DOR &= ~mask;
			outb(current_DOR, FD_DOR);
		}
		else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

static struct timer_list
{
	long jiffies;
	void (*fn)();
	struct timer_list *next;
} timer_list[TIME_REQUESTS], *next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list *p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else
	{
		for (p = timer_list; p < timer_list + TIME_REQUESTS; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies)
		{
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

void do_timer(long cpl)
{
	extern int beepcount;
	extern void sysbeepstop(void);

	if (beepcount)
		if (!--beepcount)
			sysbeepstop();

	if (cpl)			  // cpl表示当前被中断的进程是用户态还是内核态
		current->utime++; //给用户程序运行时间+1
	else
		current->stime++; //内核程序运行时间+1

	if (next_timer)
	{ // next_timer 是连接jiffies变量的所有定时器的事件链表
		// 可以这样想象，jiffies是一个时间轴，然后这个时间轴上每个绳结上绑了一个事件，运行到该绳结就触发对应的事件
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0)
		{
			void (*fn)(void);

			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}
	if (current_DOR & 0xf0) //取高四位
		do_floppy_timer();
	if ((--current->counter) > 0)
		return;
	current->counter = 0; // counter进程的时间片为0，task_struct[]是进程的向量表
	// counter在哪里用？ 进程的调度就是task_struct[]中检索，找时间片最大的进程对象来运行 直到时间片为0退出 之后再进行新一轮调用
	// counter在哪里被设置？ 当task_struct[]所有进程的counter都为0，就进行新一轮的时间片分配
	if (!cpl)
		return;
	schedule(); //这个就是进行时间片分配
}

int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds > 0) ? (jiffies + HZ * seconds) : 0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority - increment > 0)
		current->priority -= increment;
	return 0;
}

void sched_init(void)
{
	int i;
	struct desc_struct *p;

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	// gdt是全局描述符（系统级别）和前面所说的ldt（局部描述符）对应
	//内核的代码段
	//内核的数据段
	//进程0...n的数据
	set_tss_desc(gdt + FIRST_TSS_ENTRY, &(init_task.task.tss));
	set_ldt_desc(gdt + FIRST_LDT_ENTRY, &(init_task.task.ldt));
	p = gdt + 2 + FIRST_TSS_ENTRY;
	for (i = 1; i < NR_TASKS; i++)
	{ // 0-64进程进行遍历
		task[i] = NULL;
		p->a = p->b = 0;
		p++;
		p->a = p->b = 0;
		p++;
	} //作用是清空task链表
	  /* Clear NT, so that we won't have troubles with that later on */
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");
	ltr(0);
	lldt(0);
	//以下都是设置一些小的寄存器组
	outb_p(0x36, 0x43);			/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff, 0x40); /* LSB */
	outb(LATCH >> 8, 0x40);		/* MSB */
	set_intr_gate(0x20, &timer_interrupt);
	outb(inb_p(0x21) & ~0x01, 0x21);
	//设置系统中断
	set_system_gate(0x80, &system_call);
}
