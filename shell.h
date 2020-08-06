#ifndef _SHELL_H_
#define _SHELL_H_

#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __unused
#define __unused __attribute__((unused))
#endif

#define msg(...) dprintf(STDERR_FILENO, __VA_ARGS__)

typedef char *token_t;

#define T_NULL ((token_t)0)
#define T_AND ((token_t)1)
#define T_OR ((token_t)2)
#define T_PIPE ((token_t)3)
#define T_BGJOB ((token_t)4)
#define T_COLON ((token_t)5)
#define T_OUTPUT ((token_t)6)
#define T_INPUT ((token_t)7)
#define T_APPEND ((token_t)8)
#define T_BANG ((token_t)9)
#define separator_p(t) ((t) <= T_COLON)
#define string_p(t) ((t) > T_BANG)

void strapp(char **dstp, const char *src);
token_t *tokenize(char *s, int *tokc_p);

enum {
  FG = 0,
  BG = 1,
};

enum {
  ALL = -1,
  FINISHED = 0,
  RUNNING = 1,
  STOPPED = 2,
};

void initjobs(void);
void shutdownjobs(void);

int addjob(pid_t pgid, int bg);
void addproc(int job, pid_t pid, char **argv);
bool killjob(int job);
void watchjobs(int state);
int jobstate(int job, int *exitcodep);
char *jobcmd(int job);
bool resumejob(int job, int bg, sigset_t *mask);
int monitorjob(sigset_t *mask);

int builtin_command(char **argv);
noreturn void external_command(char **argv);

extern sigset_t sigchld_mask;

#endif /* !_SHELL_H_ */
