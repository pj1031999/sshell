#include <readline/history.h>
#include <readline/readline.h>

#include "shell.h"

sigset_t sigchld_mask;

static sigjmp_buf loop_env;

static void sigint_handler(int sig) { siglongjmp(loop_env, sig); }

static int do_redir(token_t *token, int ntokens, int *inputp, int *outputp) {
  token_t mode = NULL;
  int n = 0;

  for (int i = 0; i < ntokens; i++) {
    mode = token[i];

    switch ((intptr_t)mode) {
    case (intptr_t)T_INPUT: {
      if (*inputp >= 0)
        close(*inputp);

      *inputp = open(token[i + 1], O_RDONLY);
      if (-1 == *inputp)
        msg("shell: open failed '%s': %m", token[i + 1]);
      break;
    }
    case (intptr_t)T_OUTPUT: {
      if (*outputp >= 0)
        close(*outputp);

      *outputp = open(token[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (-1 == *outputp)
        msg("shell: open failed '%s': %m", token[i + 1]);
      break;
    }
    case (intptr_t)T_APPEND: {
      if (*outputp >= 0)
        close(*outputp);

      *outputp = open(token[i + 1], O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (-1 == *outputp)
        msg("shell: open failed '%s': %m", token[i + 1]);
      break;
    }
    default: {
      token[n++] = token[i];
      continue;
      break;
    }
    }

    token[i] = token[i + 1] = NULL;
    ++i;
  }

  token[n] = NULL;
  return n;
}

static int do_job(token_t *token, int ntokens, bool bg) {
  int input = -1, output = -1;
  int exitcode = 0;

  ntokens = do_redir(token, ntokens, &input, &output);

  if ((exitcode = builtin_command(token)) >= 0)
    return exitcode;

  sigset_t mask;
  sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  pid_t pid = fork();

  if (0 == pid) {
    if (-1 != input) {
      dup2(input, STDIN_FILENO);
      close(input);
    }
    if (-1 != output) {
      dup2(output, STDOUT_FILENO);
      close(output);
    }

    signal(SIGCHLD, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    sigprocmask(SIG_SETMASK, &mask, NULL);

    setpgid(0, 0);
    external_command(token);
  }

  if (-1 != input)
    close(input);

  if (-1 != output)
    close(output);

  setpgid(pid, pid);

  int job = addjob(pid, bg);
  addproc(job, pid, token);

  if (bg) {
    msg("[%d] running '%s' %d\n", job, jobcmd(job), pid);
  } else {
    exitcode = monitorjob(&mask);
  }

  sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

static pid_t do_stage(pid_t pgid, sigset_t *mask, int input, int output,
                      token_t *token, int ntokens) {
  ntokens = do_redir(token, ntokens, &input, &output);

  pid_t pid = fork();

  if (0 == pid) {
    if (-1 != input) {
      dup2(input, STDIN_FILENO);
      close(input);
    }
    if (-1 != output) {
      dup2(output, STDOUT_FILENO);
      close(output);
    }

    setpgid(0, pgid);

    signal(SIGCHLD, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    sigprocmask(SIG_SETMASK, mask, NULL);

    int exitcode = 0;
    if ((exitcode = builtin_command(token)) >= 0)
      exit(exitcode);

    external_command(token);
  }

  setpgid(pid, pgid);

  return pid;
}

static void mkpipe(int *readp, int *writep) {
  int fds[2];
  pipe(fds);
  *readp = fds[0];
  *writep = fds[1];
}

static int do_pipeline(token_t *token, int ntokens, bool bg) {
  pid_t pid, pgid = 0;
  int job = -1;
  int exitcode = 0;

  int input = -1, output = -1, next_input = -1;

  sigset_t mask;
  sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  for (int n = 0, p = 0; n < ntokens; n += p + 1) {
    for (p = 0; NULL != token[n + p] && T_PIPE != token[n + p]; ++p)
      continue;
    token[n + p] = NULL;

    if (n + p < ntokens)
      mkpipe(&next_input, &output);

    pid = do_stage(pgid, &mask, input, output, token + n, p);

    if (0 == pgid) {
      pgid = pid;
      job = addjob(pgid, bg);
    }

    if (output >= 0)
      close(output);

    if (input >= 0)
      close(input);

    input = next_input;
    output = -1;

    addproc(job, pid, token + n);
  }

  if (!bg) {
    exitcode = monitorjob(&mask);
  } else {
    msg("[%d] running '%s' %d\n", job, jobcmd(job), pgid);
  }

  sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

static bool is_pipeline(token_t *token, int ntokens) {
  for (int i = 0; i < ntokens; i++)
    if (token[i] == T_PIPE)
      return true;
  return false;
}

static void eval(char *cmdline) {
  bool bg = false;
  int ntokens;
  token_t *token = tokenize(cmdline, &ntokens);

  if (ntokens > 0 && token[ntokens - 1] == T_BGJOB) {
    token[--ntokens] = NULL;
    bg = true;
  }

  if (ntokens > 0) {
    if (is_pipeline(token, ntokens)) {
      do_pipeline(token, ntokens, bg);
    } else {
      do_job(token, ntokens, bg);
    }
  }

  free(token);
}

int main(void) {
  rl_initialize();

  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  initjobs();

  signal(SIGINT, sigint_handler);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);

  msg("[%d] shell (built on " __DATE__ " " __TIME__ ")\n\n", getpid());

  char *line;
  while (true) {
    if (!sigsetjmp(loop_env, 1)) {
      char cwdbuf[4096];
      memset(cwdbuf, 0, 4096);
      if (NULL == getcwd(cwdbuf, 4096)) {
        line = readline("# ");
      } else {
        int _n = (int)strlen(cwdbuf);
        cwdbuf[_n] = ':';
        cwdbuf[_n + 1] = ' ';
        line = readline(cwdbuf);
      }
    } else {
      msg("\n");
      continue;
    }

    if (line == NULL)
      break;

    if (strlen(line)) {
      add_history(line);
      eval(line);
    }
    free(line);
    watchjobs(FINISHED);
  }

  msg("\n");
  shutdownjobs();

  return 0;
}
