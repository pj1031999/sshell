#include "shell.h"

typedef int (*func_t)(char **argv);

typedef struct {
  const char *name;
  func_t func;
} command_t;

static int do_quit(__unused char **argv) {
  shutdownjobs();
  exit(EXIT_SUCCESS);
}

static char pathbuf[4096];
static int do_chdir(char **argv) {
  char *path = argv[0];
  if (path == NULL)
    path = getenv("HOME");

  memset(pathbuf, 0, 4096);
  if (0 == strncmp(path, "~/", 2)) {
    strcat(pathbuf, getenv("HOME"));
    strcat(pathbuf, "/");
    strcat(pathbuf, path + 2);
    path = pathbuf;
  }

  int rc = chdir(path);
  if (rc < 0) {
    msg("cd: %s: %s\n", strerror(errno), path);
    return 1;
  }
  return 0;
}

static int do_jobs(__unused char **argv) {
  watchjobs(ALL);
  return 0;
}

static int do_fg(char **argv) {
  int j = argv[0] ? atoi(argv[0]) : -1;

  sigset_t mask;
  sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);
  if (!resumejob(j, FG, &mask))
    msg("fg: job not found: %s\n", argv[0]);
  sigprocmask(SIG_SETMASK, &mask, NULL);
  return 0;
}

static int do_bg(char **argv) {
  int j = argv[0] ? atoi(argv[0]) : -1;

  sigset_t mask;
  sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);
  if (!resumejob(j, BG, &mask))
    msg("bg: job not found: %s\n", argv[0]);
  sigprocmask(SIG_SETMASK, &mask, NULL);
  return 0;
}

static int do_kill(char **argv) {
  if (!argv[0])
    return -1;
  if (*argv[0] != '%')
    return -1;

  int j = atoi(argv[0] + 1);

  sigset_t mask;
  sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);
  if (!killjob(j))
    msg("kill: job not found: %s\n", argv[0]);
  sigprocmask(SIG_SETMASK, &mask, NULL);

  return 0;
}

static command_t builtins[] = {
    {"quit", do_quit}, {"cd", do_chdir},  {"jobs", do_jobs}, {"fg", do_fg},
    {"bg", do_bg},     {"kill", do_kill}, {NULL, NULL},
};

int builtin_command(char **argv) {
  for (command_t *cmd = builtins; cmd->name; cmd++) {
    if (strcmp(argv[0], cmd->name))
      continue;
    return cmd->func(&argv[1]);
  }

  errno = ENOENT;
  return -1;
}

noreturn void external_command(char **argv) {
  const char *path = getenv("PATH");

  if (!index(argv[0], '/') && path) {
    size_t n = strlen(path);
    for (const char *colon = path; colon < path + n;) {
      size_t offset = strcspn(colon, ":");
      char *cmd = strndup(colon, offset);
      strapp(&cmd, "/");
      strapp(&cmd, argv[0]);
      (void)execve(cmd, argv, environ);
      free(cmd);
      colon += offset + 1;
    }
  } else {
    (void)execve(argv[0], argv, environ);
  }

  msg("%s: %s\n", argv[0], strerror(errno));
  exit(EXIT_FAILURE);
}
