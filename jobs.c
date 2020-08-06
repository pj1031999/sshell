#include "shell.h"

typedef struct proc {
  pid_t pid;
  int state;
  int exitcode;
} proc_t;

typedef struct job {
  pid_t pgid;
  proc_t *proc;
  int nproc;
  int state;
  char *command;
} job_t;

static job_t *jobs = NULL;
static int njobmax = 1;
static int tty_fd = -1;

static int proc_state(int status) {
  if (WIFEXITED(status) || WIFSIGNALED(status))
    return FINISHED;
  if (WIFSTOPPED(status))
    return STOPPED;
  if (WIFCONTINUED(status))
    return RUNNING;
  return RUNNING;
}

static const char *strstate(int state, int exitcode) {
  switch (state) {
  case FINISHED:
    if (WIFEXITED(exitcode))
      return "exited";
    return "killed";
  case STOPPED:
    return "stopped";
  case RUNNING:
    return "running";
  default:
    return "running";
  }
}

static void sigchld_handler(__unused int sig) {
  int old_errno = errno;
  pid_t pid;
  int status;

  while ((pid = waitpid(-1, &status, WNOHANG | WCONTINUED | WUNTRACED)) > 0) {
    for (int i = 0; i < njobmax; ++i) {
      for (int j = 0; j < jobs[i].nproc; ++j) {
        proc_t *const proc = &jobs[i].proc[j];
        if (pid == proc->pid) {
          proc->state = proc_state(status);
          proc->exitcode = status;
        }
      }
    }
  }

  for (int i = 0; i < njobmax; ++i) {
    bool finished = true;
    bool running = false;
    int *const jstate = &jobs[i].state;

    for (int j = 0; j < jobs[i].nproc; ++j) {
      int *const state = &jobs[i].proc[j].state;
      switch (*state) {
      case FINISHED:
        break;
      case STOPPED:
        finished = false;
        break;
      case RUNNING:
        finished = false;
        running = true;
        break;
      }
    }
    if (finished) {
      *jstate = FINISHED;
      continue;
    }
    if (running) {
      *jstate = RUNNING;
      continue;
    }
    *jstate = STOPPED;
  }

  errno = old_errno;
}

static int exitcode(job_t *job) { return job->proc[job->nproc - 1].exitcode; }

static int allocjob(void) {
  for (int j = BG; j < njobmax; j++)
    if (jobs[j].pgid == 0)
      return j;

  jobs = realloc(jobs, sizeof(job_t) * (njobmax + 1));
  return njobmax++;
}

static int allocproc(int j) {
  job_t *job = &jobs[j];
  job->proc = realloc(job->proc, sizeof(proc_t) * (job->nproc + 1));
  return job->nproc++;
}

int addjob(pid_t pgid, int bg) {
  int j = bg ? allocjob() : FG;
  job_t *job = &jobs[j];
  job->pgid = pgid;
  job->state = RUNNING;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
  return j;
}

static void deljob(job_t *job) {
  assert(job->state == FINISHED);
  free(job->command);
  free(job->proc);
  job->pgid = 0;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
}

static void movejob(int from, int to) {
  assert(jobs[to].pgid == 0);
  memcpy(&jobs[to], &jobs[from], sizeof(job_t));
  memset(&jobs[from], 0, sizeof(job_t));
}

static void mkcommand(char **cmdp, char **argv) {
  if (*cmdp)
    strapp(cmdp, " | ");

  for (strapp(cmdp, *argv++); *argv; argv++) {
    strapp(cmdp, " ");
    strapp(cmdp, *argv);
  }
}

void addproc(int j, pid_t pid, char **argv) {
  assert(j < njobmax);
  job_t *job = &jobs[j];

  int p = allocproc(j);
  proc_t *proc = &job->proc[p];
  proc->pid = pid;
  proc->state = RUNNING;
  proc->exitcode = -1;
  mkcommand(&job->command, argv);
}

int jobstate(int j, int *statusp) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  int state = job->state;

  if (FINISHED == state) {
    *statusp = exitcode(job);
    deljob(job);
  }

  return state;
}

char *jobcmd(int j) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  return job->command;
}

bool resumejob(int j, int bg, sigset_t *mask) {
  if (j < 0) {
    for (j = njobmax - 1; j > 0 && jobs[j].state == FINISHED; j--)
      continue;
  }

  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;

  kill(-jobs[j].pgid, SIGCONT);

  msg("[%d] continue '%s'\n", j, jobcmd(j));
  if (!bg) {
    movejob(j, FG);
    int e;
    while (STOPPED == jobstate(FG, &e)) {
      kill(-jobs[FG].pgid, SIGCONT);
      sigsuspend(mask);
    }
    monitorjob(mask);
  }

  return true;
}

bool killjob(int j) {
  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;

  int pgid = jobs[j].pgid;
  kill(-pgid, SIGTERM);

  if (STOPPED == jobs[j].state) {
    kill(-pgid, SIGCONT);
  }

  return true;
}

void watchjobs(int which) {
  for (int j = BG; j < njobmax; j++) {
    job_t *job = &jobs[j];

    if (0 == job->pgid)
      continue;

    if (ALL == which || job->state == which) {
      msg("[%d] %s '%s'", j, strstate(job->state, exitcode(job)), jobcmd(j));
    }

    if (FINISHED == job->state) {
      if (WIFSIGNALED(exitcode(job))) {
        msg(" by signal %d\n", WTERMSIG(exitcode(job)));
      } else {
        msg(", status=%d\n", WEXITSTATUS(exitcode(job)));
      }
      deljob(job);
    } else {
      msg("\n");
    }
  }
}

int monitorjob(sigset_t *mask) {
  int exitcode, state;
  int j;

  exitcode = 0;
  tcsetpgrp(tty_fd, jobs[0].pgid);
  kill(-jobs[0].pgid, SIGCONT);

  for (int loop = 1; loop;) {
    state = jobstate(FG, &exitcode);
    switch (state) {
    case FINISHED: {
      loop = 0;
      break;
    }
    case STOPPED: {
      j = allocjob();
      memset(&jobs[j], 0, sizeof(job_t));
      movejob(FG, j);
      msg("[%d] suspended '%s'\n", j, jobcmd(j));
      loop = 0;
      break;
    }
    default: {
      sigsuspend(mask);
      break;
    }
    }
  }

  tcsetpgrp(tty_fd, getpgrp());

  return exitcode;
}

void initjobs(void) {
  signal(SIGCHLD, sigchld_handler);
  jobs = calloc(sizeof(job_t), 1);

  assert(isatty(STDIN_FILENO));
  tty_fd = dup(STDIN_FILENO);
  fcntl(tty_fd, F_SETFL, O_CLOEXEC);
  tcsetpgrp(tty_fd, getpgrp());
}

void shutdownjobs(void) {
  sigset_t mask;
  sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  for (int i = 0; i < njobmax; ++i) {
    if (jobs[i].pgid)
      killjob(i);

    while (FINISHED != jobs[i].state)
      sigsuspend(&mask);
  }

  watchjobs(FINISHED);

  if (jobs)
    free(jobs);
  sigprocmask(SIG_SETMASK, &mask, NULL);

  close(tty_fd);
}
