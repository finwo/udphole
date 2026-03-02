#include "domain/cluster/cluster.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common.h"
#include "cofyc/argparse.h"
#include "common/resp.h"
#include "common/scheduler.h"
#include "domain/config.h"
#include "infrastructure/config.h"
#include "interface/api/server.h"
#include "rxi/log.h"

static void register_cluster_commands(void) {
  api_register_domain_cmd("session.create", cluster_session_create);
  api_register_domain_cmd("session.list", cluster_session_list);
  api_register_domain_cmd("session.info", cluster_session_info);
  api_register_domain_cmd("session.destroy", cluster_session_destroy);
  api_register_domain_cmd("session.socket.create.listen", cluster_socket_create_listen);
  api_register_domain_cmd("session.socket.create.connect", cluster_socket_create_connect);
  api_register_domain_cmd("session.socket.destroy", cluster_socket_destroy);
  api_register_domain_cmd("session.forward.list", cluster_forward_list);
  api_register_domain_cmd("session.forward.create", cluster_forward_create);
  api_register_domain_cmd("session.forward.destroy", cluster_forward_destroy);
  api_register_domain_cmd("session.count", cluster_session_count);
  api_register_domain_cmd("system.load", cluster_system_load);
  log_info("cluster: registered session.* commands");
}

static int do_daemonize(void) {
  pid_t pid = fork();
  if (pid < 0) {
    log_fatal("fork: %m");
    return -1;
  }
  if (pid > 0) _exit(0);
  if (setsid() < 0) {
    log_fatal("setsid: %m");
    _exit(1);
  }
  pid = fork();
  if (pid < 0) {
    log_fatal("fork: %m");
    _exit(1);
  }
  if (pid > 0) _exit(0);
  if (chdir("/") != 0) {
  }
  int fd;
  for (fd = 0; fd < 3; fd++) (void)close(fd);
  fd = open("/dev/null", O_RDWR);
  if (fd >= 0) {
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > 2) close(fd);
  }
  return 0;
}

int cli_cmd_cluster(int argc, const char **argv) {
  int daemonize_flag    = 0;
  int no_daemonize_flag = 0;

  struct argparse        argparse;
  struct argparse_option options[] = {
      OPT_HELP(),
      OPT_BOOLEAN('d', "daemonize", &daemonize_flag, "run in background", NULL, 0, 0),
      OPT_BOOLEAN('D', "no-daemonize", &no_daemonize_flag, "force foreground", NULL, 0, 0),
      OPT_END(),
  };
  argparse_init(&argparse, options, (const char *const[]){"udphole cluster", NULL}, ARGPARSE_STOP_AT_NON_OPTION);
  argparse_parse(&argparse, argc, argv);

  if (!no_daemonize_flag && daemonize_flag) {
    do_daemonize();
  }

  cluster_init();

  register_cluster_commands();

  log_info("udphole: starting cluster");

  sched_create(api_server_pt, NULL);

  log_info("udphole: cluster started");

  return sched_main();
}
