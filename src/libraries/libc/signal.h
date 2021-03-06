#ifndef _LIBC_SIGNAL_H
#define _LIBC_SIGNAL_H 1

#include <stdint.h>
#include <sys/types.h>

#define NSIG 32

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGIOT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPOLL SIGIO
/*
#define SIGLOST		29
*/
#define SIGPWR 30
#define SIGSYS 31
#define SIGUNUSED 31

/* These should not be considered constants from userland.  */
#define SIGRTMIN 32
#define SIGRTMAX NSIG

#define SA_RESTART 0x10000000u
#define SA_NODEFER 0x40000000u
#define SA_RESETHAND 0x80000000u

#define SIG_BLOCK 0	  /* for blocking signals */
#define SIG_UNBLOCK 1 /* for unblocking signals */
#define SIG_SETMASK 2 /* for setting the signal mask */

typedef void (*sighandler_t)(int);
typedef int sig_atomic_t;

#define SIG_DFL ((sighandler_t)0)  /* default signal handling */
#define SIG_IGN ((sighandler_t)1)  /* ignore signal */
#define SIG_ERR ((sighandler_t)-1) /* error return from signal */

#define sigmask(sig) (1UL << ((sig)-1))

/* Bits in `sa_flags'.  */
#define SA_NOCLDSTOP 1 /* Don't send SIGCHLD when children stop.  */
#define SA_NOCLDWAIT 2 /* Don't create zombie on child death.  */
#define SA_SIGINFO 4   /* Invoke signal-catching function with */

struct sigaction
{
	sighandler_t sa_handler;
	uint32_t sa_flags;
	sigset_t sa_mask;
};

int kill(pid_t pid, int sig);
int raise(int32_t sig);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigaddset(sigset_t *set, int signo);
int sigdelset(sigset_t *set, int sig);
void sigemptyset(sigset_t *set);
void sigfillset(sigset_t *set);
int sigismember(sigset_t *set, int sig);
int sigsuspend(const sigset_t *mask);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int signal(int signum, sighandler_t handler);

#endif
