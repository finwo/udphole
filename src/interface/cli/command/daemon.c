#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "cofyc/argparse.h"
#include "rxi/log.h"

#include "infrastructure/config.h"
#include "common/resp.h"
#include "../common.h"
#include "common/scheduler.h"
#include "domain/config.h"
#include "daemon.h"
#include "interface/api/server.h"
#include "domain/daemon/session.h"

static void register_domain_commands(void) {
  api_register_domain_cmd("session.create", domain_session_create);
  api_register_domain_cmd("session.list", domain_session_list);
  api_register_domain_cmd("session.info", domain_session_info);
  api_register_domain_cmd("session.destroy", domain_session_destroy);
  api_register_domain_cmd("session.socket.create.listen", domain_socket_create_listen);
  api_register_domain_cmd("session.socket.create.connect", domain_socket_create_connect);
  api_register_domain_cmd("session.socket.destroy", domain_socket_destroy);
  api_register_domain_cmd("session.forward.list", domain_forward_list);
  api_register_domain_cmd("session.forward.create", domain_forward_create);
  api_register_domain_cmd("session.forward.destroy", domain_forward_destroy);
  api_register_domain_cmd("session.count", domain_session_count);
  api_register_domain_cmd("system.load", domain_system_load);
  log_info("udphole: registered session.* commands");
}

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
  if (chdir("/") != 0) {}
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

int cli_cmd_daemon(int argc, const char **argv) {
  int daemonize_flag = 0;
  int no_daemonize_flag = 0;

  struct argparse argparse;
  struct argparse_option options[] = {
    OPT_HELP(),
    OPT_BOOLEAN('d', "daemonize", &daemonize_flag, "run in background", NULL, 0, 0),
    OPT_BOOLEAN('D', "no-daemonize", &no_daemonize_flag, "force foreground", NULL, 0, 0),
    OPT_END(),
  };
  argparse_init(&argparse, options, (const char *const[]){"udphole daemon", NULL}, ARGPARSE_STOP_AT_NON_OPTION);
  argparse_parse(&argparse, argc, argv);

  if (!no_daemonize_flag && daemonize_flag) {
    do_daemonize();
  }

  domain_config_init();

  register_domain_commands();

  log_info("udphole: starting daemon");

  session_manager_udata_t *session_udata = calloc(1, sizeof(session_manager_udata_t));

  sched_create(api_server_pt, NULL);
  sched_create(session_manager_pt, session_udata);

  log_info("udphole: daemon started");

  return sched_main();
}