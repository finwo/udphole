#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "cofyc/argparse.h"
#include "rxi/log.h"

#include "config.h"
#include "CliModule/common.h"
#include "SchedulerModule/scheduler.h"
#include "AppModule/command/daemon.h"
#include "AppModule/udphole.h"
#include "ApiModule/server.h"

static const char *const daemon_usages[] = {
  "udphole daemon [options]",
  NULL,
};

static int do_daemonize(void) {
  pid_t pid = fork();
  if (pid < 0) {
    log_fatal("fork: %m");
    return -1;
  }
  if (pid > 0)
    _exit(0);
  if (setsid() < 0) {
    log_fatal("setsid: %m");
    _exit(1);
  }
  pid = fork();
  if (pid < 0) {
    log_fatal("fork: %m");
    _exit(1);
  }
  if (pid > 0)
    _exit(0);
  if (chdir("/") != 0) {
  }
  int fd;
  for (fd = 0; fd < 3; fd++)
    (void)close(fd);
  fd = open("/dev/null", O_RDWR);
  if (fd >= 0) {
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > 2)
      close(fd);
  }
  return 0;
}

int appmodule_cmd_daemon(int argc, const char **argv) {
  int daemonize_flag = 0;
  int no_daemonize_flag = 0;

  struct argparse argparse;
  struct argparse_option options[] = {
    OPT_HELP(),
    OPT_BOOLEAN('d', "daemonize", &daemonize_flag, "run in background", NULL, 0, 0),
    OPT_BOOLEAN('D', "no-daemonize", &no_daemonize_flag, "force foreground (overrides config daemonize=1)", NULL, 0, 0),
    OPT_END(),
  };
  argparse_init(&argparse, options, daemon_usages, ARGPARSE_STOP_AT_NON_OPTION);
  argc = argparse_parse(&argparse, argc, argv);

  if ((!no_daemonize_flag) && (daemonize_flag || 0)) {
    do_daemonize();
  }

  schedmod_pt_create(api_server_pt, NULL);
  schedmod_pt_create(udphole_manager_pt, NULL);

  return schedmod_main();
}
