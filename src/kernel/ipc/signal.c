#include "signal.h"

#include <include/bitops.h>
#include <include/errno.h>
#include <kernel/proc/task.h>
#include <kernel/utils/hashmap.h>
#include <kernel/utils/printf.h>
#include <kernel/utils/string.h>

// TODO: MQ 2020-08-21 implement signal jump
void sigjump_usermode(struct interrupt_registers *);

struct signal_frame
{
	void (*sigreturn)(struct interrupt_registers *);
	int32_t signum;
	bool signaling;
	sigset_t blocked;
	struct interrupt_registers uregs;
};

#define M(sig) (1UL << ((sig)-1))
#define T(sig, mask) (M(sig) & (mask))

#define SIG_KERNEL_ONLY_MASK (M(SIGKILL) | M(SIGSTOP))

#define SIG_KERNEL_STOP_MASK (M(SIGSTOP) | M(SIGTSTP) | M(SIGTTIN) | M(SIGTTOU))

#define SIG_KERNEL_COREDUMP_MASK (                     \
	M(SIGQUIT) | M(SIGILL) | M(SIGTRAP) | M(SIGABRT) | \
	M(SIGFPE) | M(SIGSEGV) | M(SIGBUS) | M(SIGSYS) |   \
	M(SIGXCPU) | M(SIGXFSZ))

#define SIG_KERNEL_IGNORE_MASK ( \
	M(SIGCHLD) | M(SIGWINCH) | M(SIGURG))

#define sig_kernel_only(sig) \
	(((sig) < SIGRTMIN) && T(sig, SIG_KERNEL_ONLY_MASK))
#define sig_kernel_coredump(sig) \
	(((sig) < SIGRTMIN) && T(sig, SIG_KERNEL_COREDUMP_MASK))
#define sig_kernel_ignore(sig) \
	(((sig) < SIGRTMIN) && T(sig, SIG_KERNEL_IGNORE_MASK))
#define sig_kernel_stop(sig) \
	(((sig) < SIGRTMIN) && T(sig, SIG_KERNEL_STOP_MASK))

#define sig_user_defined(p, signr)                      \
	(((p)->sighand[(signr)-1].sa_handler != SIG_DFL) && \
	 ((p)->sighand[(signr)-1].sa_handler != SIG_IGN))
#define sig_default_action(p, signr) ((p)->sighand[(signr)-1].sa_handler == SIG_DFL)

#define sig_fatal(p, signr)                                      \
	(!T(signr, SIG_KERNEL_IGNORE_MASK | SIG_KERNEL_STOP_MASK) && \
	 (p)->sighand[(signr)-1].sa_handler == SIG_DFL)

int next_signal(sigset_t pending, sigset_t blocked)
{
	sigset_t mask = pending & ~blocked;
	uint32_t signum = 0;

	if (mask & SIG_KERNEL_COREDUMP_MASK)
		signum = ffz(~(mask & SIG_KERNEL_COREDUMP_MASK)) + 1;
	else if (mask & ~M(SIGCONT))
		signum = ffz(~(mask & ~M(SIGCONT))) + 1;
	else if (mask & M(SIGCONT))
		signum = SIGCONT;

	return signum;
}

bool sig_ignored(struct thread *tsk, int sig)
{
	if (sigismember(&tsk->blocked, sig))
		return false;

	__sighandler_t handler = tsk->parent->sighand[sig - 1].sa_handler;
	return handler == SIG_IGN || (handler == SIG_DFL && sig_kernel_ignore(sig));
}

int do_sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
	if (oldset)
		*oldset = current_thread->blocked;

	if (set)
	{
		if (how == SIG_BLOCK)
			current_thread->blocked |= *set;
		else if (how == SIG_UNBLOCK)
			current_thread->blocked &= ~*set;
		else if (how == SIG_SETMASK)
			current_thread->blocked = *set;
		else
			return -EINVAL;

		current_thread->blocked &= ~SIG_KERNEL_ONLY_MASK;
	}

	return 0;
}

int do_sigaction(int signum, const struct sigaction *action, struct sigaction *old_action)
{
	if (!valid_signal(signum) || signum < 1 || sig_kernel_only(signum))
		return -EINVAL;

	if (old_action)
		memcpy(old_action, &current_process->sighand[signum - 1], sizeof(struct sigaction));

	if (action)
		memcpy(&current_process->sighand[signum - 1], action, sizeof(struct sigaction));

	return 0;
}

int do_kill(pid_t pid, int32_t signum)
{
	if (!valid_signal(signum) || signum < 0)
		return -EINVAL;

	if (!signum)
		return 0;

	if (pid > 0)
	{
		struct process *proc = find_process_by_pid(pid);
		struct thread *tsk = proc->thread;

		if (sig_ignored(tsk, signum))
			return 0;

		if (signum == SIGCONT)
		{
			sigdelsetmask(&current_thread->pending, SIG_KERNEL_STOP_MASK);
			current_process->flags |= SIGNAL_CONTINUED;
			current_process->flags &= !SIGNAL_STOPED;
			do_kill(current_process->parent->pid, SIGCHLD);
			wake_up(&current_process->parent->wait_chld);
		}
		else if (sig_kernel_stop(signum))
		{
			sigdelset(&current_thread->pending, SIGCONT);
			current_process->flags |= SIGNAL_STOPED;
			current_process->flags &= !SIGNAL_CONTINUED;
			do_kill(current_process->parent->pid, SIGCHLD);
			wake_up(&current_process->parent->wait_chld);
		}

		tsk->pending |= M(signum);
		if ((signum == SIGCONT || signum == SIGKILL) && tsk != current_thread)
			update_thread(tsk, THREAD_READY);
	}
	else if (pid == 0)
	{
		struct process *proc;
		struct hashmap_iter *iter;
		for_each_process(proc, iter)
		{
			if (proc->gid == current_process->gid)
				do_kill(proc->gid, signum);
		}
	}
	else if (pid == -1)
	{
		struct process *proc;
		struct hashmap_iter *iter;
		for_each_process(proc, iter)
		{
			// TODO: MQ 2020-08-20 Make sure calling process has permission to send signals
			if (proc->pid > 1)
				do_kill(proc->gid, signum);
		}
	}
	else
	{
		struct process *proc;
		struct hashmap_iter *iter;
		for_each_process(proc, iter)
		{
			if (proc->gid == -pid)
				do_kill(proc->gid, signum);
		}
	}

	return 0;
}

void signal_handler(struct interrupt_registers *regs)
{
	if (!current_thread->pending || current_thread->signaling ||
		((uint32_t)regs + sizeof(struct interrupt_registers) != current_thread->kernel_stack))
		return;

	handle_signal(regs);
}

void handle_signal(struct interrupt_registers *regs)
{
	bool from_syscall = false;
	bool prev_signaling = current_thread->signaling;

	if (!current_thread->pending)
		return;

	if (regs->int_no == 0x7F)
	{
		from_syscall = true;
		regs->eax = -EINTR;
	}
	current_thread->signaling = true;
	memcpy(&current_thread->uregs, regs, sizeof(struct interrupt_registers));

	int signum = next_signal(current_thread->pending, current_thread->blocked);
	sigdelset(&current_thread->pending, signum);

	assert(!sig_ignored(current_thread, signum));
	if (sig_default_action(current_process, signum))
	{
		assert(sig_kernel_coredump(signum));
		current_process->caused_signal = signum;
		current_process->flags |= SIGNAL_TERMINATED;
		current_process->flags &= !(SIGNAL_CONTINUED | SIGNAL_STOPED);
		current_thread->signaling = false;
		sigemptyset(&current_thread->pending);
		do_exit(signum);
	}
	else if (sig_user_defined(current_process, signum))
	{
		regs->useresp -= sizeof(struct signal_frame);
		struct signal_frame *frame = (struct signal_frame *)regs->useresp;
		frame->sigreturn = sigreturn;
		frame->signum = signum;
		frame->signaling = prev_signaling;
		frame->blocked = current_thread->blocked;
		frame->uregs = *regs;

		struct sigaction *sigaction = &current_process->sighand[signum - 1];
		regs->eip = (uint32_t)sigaction->sa_handler;
		current_thread->blocked |= M(signum) | sigaction->sa_mask;
		if (from_syscall)
			sigjump_usermode(&current_thread->uregs);
	}
}

void sigreturn(struct interrupt_registers *regs)
{
	struct signal_frame *frame = (struct signal_frame *)regs->useresp;
	current_thread->uregs = frame->uregs;
	current_thread->signaling = frame->signaling;
	current_thread->blocked = frame->blocked;
	memcpy(regs, &frame->uregs, sizeof(struct interrupt_registers));
}