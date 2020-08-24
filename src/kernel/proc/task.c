#include "task.h"

#include <kernel/cpu/hal.h>
#include <kernel/cpu/idt.h>
#include <kernel/cpu/pic.h>
#include <kernel/cpu/tss.h>
#include <kernel/fs/vfs.h>
#include <kernel/memory/pmm.h>
#include <kernel/memory/vmm.h>
#include <kernel/proc/elf.h>
#include <kernel/system/time.h>
#include <kernel/utils/hashmap.h>
#include <kernel/utils/printf.h>
#include <kernel/utils/string.h>

extern void enter_usermode(uint32_t eip, uint32_t esp, uint32_t failed_address);
extern void return_usermode(struct interrupt_registers *regs);
extern void irq_schedule_handler(struct interrupt_registers *regs);
extern int32_t thread_page_fault(struct interrupt_registers *regs);

volatile struct thread *current_thread;
volatile struct process *current_process;
static uint32_t next_pid = 0;
static uint32_t next_tid = 0;
volatile struct hashmap *mprocess;

struct process *find_process_by_pid(pid_t pid)
{
	return hashmap_get(mprocess, &pid);
}

struct files_struct *clone_file_descriptor_table(struct process *parent)
{
	struct files_struct *files = kcalloc(1, sizeof(struct files_struct));

	if (parent)
	{
		memcpy(files, parent->files, sizeof(struct files_struct));
		// NOTE: MQ 2019-12-30 Increasing file description usage when forking because child refers to the same one
		for (uint32_t i = 0; i < MAX_FD; ++i)
			if (parent->files->fd[i])
				atomic_inc(&parent->files->fd[i]->f_count);
	}
	sema_init(&files->lock, 1);
	return files;
}

struct mm_struct *clone_mm_struct(struct process *parent)
{
	struct mm_struct *mm = kcalloc(1, sizeof(struct mm_struct));
	memcpy(mm, parent->mm, sizeof(struct mm_struct));
	INIT_LIST_HEAD(&mm->mmap);

	struct vm_area_struct *iter = NULL;
	list_for_each_entry(iter, &parent->mm->mmap, vm_sibling)
	{
		struct vm_area_struct *clone = kcalloc(1, sizeof(struct vm_area_struct));
		clone->vm_start = iter->vm_start;
		clone->vm_end = iter->vm_end;
		clone->vm_file = iter->vm_file;
		clone->vm_flags = iter->vm_flags;
		clone->vm_mm = mm;
		list_add_tail(&clone->vm_sibling, &mm->mmap);
	}

	return mm;
}

void kernel_thread_entry(struct thread *t, void *flow())
{
	flow();
	schedule();
}

void thread_sleep_timer(struct timer_list *timer)
{
	struct thread *t = from_timer(t, timer, sleep_timer);
	list_del(&timer->sibling);
	update_thread(t, THREAD_READY);
}

void thread_sleep(uint32_t ms)
{
	mod_timer(&current_thread->sleep_timer, get_milliseconds(NULL) + ms);
	update_thread(current_thread, THREAD_WAITING);
}

struct thread *create_kernel_thread(struct process *parent, uint32_t eip, enum thread_state state, int priority)
{
	lock_scheduler();

	struct thread *t = kcalloc(1, sizeof(struct thread));
	t->tid = next_tid++;
	t->kernel_stack = (uint32_t)(kcalloc(STACK_SIZE, sizeof(char)) + STACK_SIZE);
	t->parent = parent;
	t->state = state;
	t->esp = t->kernel_stack - sizeof(struct trap_frame);
	plist_node_init(&t->sched_sibling, priority);
	t->sleep_timer = (struct timer_list)TIMER_INITIALIZER(thread_sleep_timer, UINT32_MAX);

	struct trap_frame *frame = (struct trap_frame *)t->esp;
	memset(frame, 0, sizeof(struct trap_frame));

	frame->parameter2 = eip;
	frame->parameter1 = (uint32_t)t;
	frame->return_address = PROCESS_TRAPPED_PAGE_FAULT;
	frame->eip = (uint32_t)kernel_thread_entry;

	frame->eax = 0;
	frame->ecx = 0;
	frame->edx = 0;
	frame->ebx = 0;
	frame->esp = 0;
	frame->ebp = 0;
	frame->esi = 0;
	frame->edi = 0;

	parent->thread = t;

	unlock_scheduler();

	return t;
}

struct process *create_process(struct process *parent, const char *name, struct pdirectory *pdir)
{
	lock_scheduler();

	struct process *p = kcalloc(1, sizeof(struct process));
	p->pid = next_pid++;
	p->name = strdup(name);
	if (pdir)
		p->pdir = vmm_create_address_space(pdir);
	else
		p->pdir = vmm_get_directory();
	p->parent = parent;
	p->files = clone_file_descriptor_table(parent);
	p->fs = kcalloc(1, sizeof(struct fs_struct));
	p->mm = kcalloc(1, sizeof(struct mm_struct));
	INIT_LIST_HEAD(&p->mm->mmap);

	if (parent)
	{
		memcpy(p->fs, parent->fs, sizeof(struct fs_struct));
		list_add_tail(&p->sibling, &parent->children);
	}

	INIT_LIST_HEAD(&p->children);

	hashmap_put(mprocess, &p->pid, p);

	unlock_scheduler();

	return p;
}

void setup_swapper_process()
{
	current_process = create_process(NULL, "swapper", NULL);
	current_thread = create_kernel_thread(current_process, 0, THREAD_RUNNING, 0);
}

struct process *create_kernel_process(const char *pname, void *func, int32_t priority)
{
	struct process *p = create_process(current_process, pname, current_process->pdir);
	create_kernel_thread(p, (uint32_t)func, THREAD_WAITING, priority);

	return p;
}

void task_init(void *func)
{
	DEBUG &&debug_println(DEBUG_INFO, "[task] - Initializing");

	mprocess = kcalloc(1, sizeof(struct hashmap));
	hashmap_init(mprocess, hashmap_hash_uint32, hashmap_compare_uint32, 0);
	sched_init();
	// register_interrupt_handler(IRQ0, irq_schedule_handler);
	register_interrupt_handler(14, thread_page_fault);

	DEBUG &&debug_println(DEBUG_INFO, "\tSetup swapper process");
	setup_swapper_process();

	DEBUG &&debug_println(DEBUG_INFO, "\tSetup init process");
	struct process *init = create_process(current_process, "init", current_process->pdir);
	struct thread *nt = create_kernel_thread(init, (uint32_t)func, THREAD_WAITING, 1);

	update_thread(current_thread, THREAD_TERMINATED);
	update_thread(nt, THREAD_READY);
	DEBUG &&debug_println(DEBUG_INFO, "[task] - Done");
	schedule();
}

void user_thread_entry(struct thread *t)
{
	tss_set_stack(0x10, t->kernel_stack);
	return_usermode(&t->uregs);
}

void user_thread_elf_entry(struct thread *t, const char *path, void (*setup)(struct Elf32_Layout *))
{
	// explain in kernel_init#unlock_scheduler
	unlock_scheduler();

	char *buf = vfs_read(path);
	struct Elf32_Layout *elf_layout = elf_load(buf);
	t->user_stack = elf_layout->stack;
	tss_set_stack(0x10, t->kernel_stack);
	if (setup)
		setup(elf_layout);
	enter_usermode(elf_layout->stack, elf_layout->entry, PROCESS_TRAPPED_PAGE_FAULT);
}

struct thread *create_user_thread(struct process *parent, const char *path, enum thread_state state, enum thread_policy policy, int priority, void (*setup)(struct Elf32_Layout *))
{
	lock_scheduler();

	struct thread *t = kcalloc(1, sizeof(struct thread));
	t->tid = next_tid++;
	t->parent = parent;
	t->state = state;
	t->policy = policy;
	t->kernel_stack = (uint32_t)(kcalloc(STACK_SIZE, sizeof(char)) + STACK_SIZE);
	t->esp = t->kernel_stack - sizeof(struct trap_frame);
	plist_node_init(&t->sched_sibling, priority);
	t->sleep_timer = (struct timer_list)TIMER_INITIALIZER(thread_sleep_timer, UINT32_MAX);

	struct trap_frame *frame = (struct trap_frame *)t->esp;
	memset(frame, 0, sizeof(struct trap_frame));

	frame->parameter3 = (uint32_t)setup;
	frame->parameter2 = (uint32_t)strdup(path);
	frame->parameter1 = (uint32_t)t;
	frame->return_address = PROCESS_TRAPPED_PAGE_FAULT;
	frame->eip = (uint32_t)user_thread_elf_entry;

	frame->eax = 0;
	frame->ecx = 0;
	frame->edx = 0;
	frame->ebx = 0;
	frame->esp = 0;
	frame->ebp = 0;
	frame->esi = 0;
	frame->edi = 0;

	parent->thread = t;

	unlock_scheduler();

	return t;
}

void process_load(const char *pname, const char *path, int priority, void (*setup)(struct Elf32_Layout *))
{
	struct process *p = create_process(current_process, pname, current_process->pdir);
	struct thread *t = create_user_thread(p, path, THREAD_READY, THREAD_SYSTEM_POLICY, priority, setup);
	queue_thread(t);
}

struct process *process_fork(struct process *parent)
{
	lock_scheduler();

	// fork process
	struct process *p = kcalloc(1, sizeof(struct process));
	p->pid = next_pid++;
	p->gid = parent->pid;
	p->sid = parent->sid;
	p->name = strdup(parent->name);
	p->parent = parent;
	p->mm = clone_mm_struct(parent);
	memcpy(&p->sighand, &parent->sighand, sizeof(parent->sighand));

	INIT_LIST_HEAD(&p->children);

	list_add_tail(&p->sibling, &parent->children);

	p->fs = kcalloc(1, sizeof(struct fs_struct));
	memcpy(p->fs, parent->fs, sizeof(struct fs_struct));

	p->files = clone_file_descriptor_table(parent);
	p->pdir = vmm_fork(parent->pdir);

	// copy active parent's thread
	struct thread *parent_thread = parent->thread;
	struct thread *t = kcalloc(1, sizeof(struct thread));
	t->tid = next_tid++;
	t->state = THREAD_READY;
	t->policy = parent_thread->policy;
	t->time_slice = 0;
	t->parent = p;
	t->kernel_stack = (uint32_t)(kcalloc(STACK_SIZE, sizeof(char)) + STACK_SIZE);
	t->user_stack = parent_thread->user_stack;
	// NOTE: MQ 2019-12-18 Setup trap frame
	t->esp = t->kernel_stack - sizeof(struct trap_frame);
	plist_node_init(&t->sched_sibling, parent_thread->sched_sibling.prio);

	memcpy(&t->uregs, &parent_thread->uregs, sizeof(struct interrupt_registers));
	t->uregs.eax = 0;

	struct trap_frame *frame = (struct trap_frame *)t->esp;
	frame->parameter1 = (uint32_t)t;
	frame->return_address = PROCESS_TRAPPED_PAGE_FAULT;
	frame->eip = (uint32_t)user_thread_entry;

	frame->eax = 0;
	frame->ecx = 0;
	frame->edx = 0;
	frame->ebx = 0;
	frame->esp = 0;
	frame->ebp = 0;
	frame->esi = 0;
	frame->edi = 0;

	parent->thread = t;

	unlock_scheduler();

	return p;
}
