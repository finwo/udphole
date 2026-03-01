#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#include "cofyc/argparse.h"
#include "rxi/log.h"

#include "infrastructure/config.h"
#include "common/resp.h"
#include "../common.h"
#include "domain/scheduler.h"
#include "domain/config.h"
#include "cluster.h"
#include "interface/api/server.h"
#include "domain/cluster/node.h"

#define HEALTH_CHECK_INTERVAL 5
#define MAX_FAILURES 3

static cluster_node_t *cluster_nodes = NULL;
static const char *cluster_listen = NULL;

typedef struct {
  int64_t last_health_check;
} cluster_udata_t;

static void cluster_config_free(void) {
  cluster_node_t *node = cluster_nodes;
  while (node) {
    cluster_node_t *next = node->next;
    cluster_node_free(node);
    node = next;
  }
  cluster_nodes = NULL;
}

static int cluster_config_parse(void) {
  cluster_config_free();

  if (!global_cfg) return -1;

  resp_object *cluster_sec = resp_map_get(global_cfg, "cluster");
  if (!cluster_sec) {
    log_error("cluster: no [cluster] section in config");
    return -1;
  }

  const char *nodes_str = resp_map_get_string(cluster_sec, "nodes");
  if (!nodes_str) {
    log_error("cluster: no 'nodes' defined in [cluster] section");
    return -1;
  }

  cluster_listen = resp_map_get_string(cluster_sec, "listen");

  char *nodes_copy = strdup(nodes_str);
  char *saveptr;
  char *token = strtok_r(nodes_copy, ",", &saveptr);

  while (token) {
    while (*token == ' ') token++;
    char *end = token + strlen(token) - 1;
    while (end > token && *end == ' ') *end-- = '\0';

    char section_name[64];
    snprintf(section_name, sizeof(section_name), "cluster:node:%s", token);

    resp_object *node_sec = resp_map_get(global_cfg, section_name);
    if (!node_sec) {
      log_error("cluster: no [%s] section found", section_name);
      token = strtok_r(NULL, ",", &saveptr);
      continue;
    }

    const char *url = resp_map_get_string(node_sec, "url");
    const char *weight_str = resp_map_get_string(node_sec, "weight");
    const char *user = resp_map_get_string(node_sec, "user");
    const char *secret = resp_map_get_string(node_sec, "secret");

    if (!url) {
      log_error("cluster: no 'url' for node %s", token);
      token = strtok_r(NULL, ",", &saveptr);
      continue;
    }

    int weight = weight_str ? atoi(weight_str) : 1;
    if (weight <= 0) weight = 1;

    cluster_node_t *node = cluster_node_create(token, url, weight, user, secret);
    if (!node) {
      log_error("cluster: failed to create node %s", token);
      token = strtok_r(NULL, ",", &saveptr);
      continue;
    }

    node->next = cluster_nodes;
    cluster_nodes = node;

    log_info("cluster: added node %s (url=%s, weight=%d)", token, url, weight);

    token = strtok_r(NULL, ",", &saveptr);
  }

  free(nodes_copy);
  return 0;
}

static void cluster_health_check(void) {
  cluster_node_t *node = cluster_nodes;
  while (node) {
    int result = cluster_node_ping(node);
    if (result == 0) {
      if (!node->available) {
        log_info("cluster: node %s is now available", node->name);
        cluster_node_set_available(node, true);
      }
    } else {
      if (node->available || node->consecutive_failures >= MAX_FAILURES) {
        log_warn("cluster: node %s is unavailable (failures: %d)", node->name, node->consecutive_failures);
        cluster_node_set_available(node, false);
      }
    }
    node = node->next;
  }
}

static void cluster_refresh_session_counts(void) {
  cluster_node_t *node = cluster_nodes;
  while (node) {
    if (node->available) {
      int count = cluster_node_get_session_count(node);
      if (count >= 0) {
        node->session_count = count;
      }
    }
    node = node->next;
  }
}

static cluster_node_t *cluster_select_node(void) {
  cluster_node_t *best = NULL;
  cluster_node_t *node = cluster_nodes;
  while (node) {
    if (node->available) {
      if (!best || cluster_node_select_weighted_lowest(node, best)) {
        best = node;
      }
    }
    node = node->next;
  }
  return best;
}

static cluster_node_t *cluster_find_session_node(const char *session_id) {
  cluster_node_t *node = cluster_nodes;
  while (node) {
    if (node->available) {
      resp_object *resp = cluster_node_send_command(node, "session.info", session_id, NULL);
      if (resp && resp->type != RESPT_ERROR) {
        resp_free(resp);
        return node;
      }
      if (resp) resp_free(resp);
    }
    node = node->next;
  }
  return NULL;
}

static resp_object *cluster_forward_to_node(cluster_node_t *node, const char *cmd, resp_object *args) {
  if (!node || node->fd < 0) return NULL;

  resp_object *resp = cluster_node_send_command(node, cmd, NULL);
  if (resp) return resp;

  return NULL;
}

static resp_object *cluster_handle_command(const char *cmd, resp_object *args) {
  if (!cmd || !args) return NULL;

  if (strcmp(cmd, "session.create") == 0) {
    cluster_node_t *node = cluster_select_node();
    if (!node) {
      resp_object *err = resp_array_init();
      resp_array_append_simple(err, "ERR no available nodes");
      return err;
    }

    const char *session_id = NULL;
    const char *idle_expiry = NULL;

    if (args->u.arr.n > 1 && args->u.arr.elem[1].type == RESPT_BULK) {
      session_id = args->u.arr.elem[1].u.s;
    }
    if (args->u.arr.n > 2 && args->u.arr.elem[2].type == RESPT_BULK) {
      idle_expiry = args->u.arr.elem[2].u.s;
    }

    resp_object *resp;
    if (idle_expiry) {
      resp = cluster_node_send_command(node, "session.create", session_id, idle_expiry, NULL);
    } else {
      resp = cluster_node_send_command(node, "session.create", session_id, NULL);
    }

    if (resp && resp->type != RESPT_ERROR) {
      node->session_count++;
    }

    return resp;
  }

  if (strcmp(cmd, "session.list") == 0) {
    resp_object *result = resp_array_init();
    cluster_node_t *node = cluster_nodes;
    while (node) {
      if (node->available) {
        resp_object *resp = cluster_node_send_command(node, "session.list", NULL);
        if (resp && resp->type == RESPT_ARRAY) {
          for (size_t i = 0; i < resp->u.arr.n; i++) {
            if (resp->u.arr.elem[i].type == RESPT_BULK) {
              resp_array_append_bulk(result, resp->u.arr.elem[i].u.s);
            }
          }
        }
        if (resp) resp_free(resp);
      }
      node = node->next;
    }
    return result;
  }

  if (strcmp(cmd, "session.count") == 0) {
    int total = 0;
    cluster_node_t *node = cluster_nodes;
    while (node) {
      if (node->available) {
        int count = cluster_node_get_session_count(node);
        if (count >= 0) {
          total += count;
        }
      }
      node = node->next;
    }
    resp_object *result = malloc(sizeof(resp_object));
    result->type = RESPT_INT;
    result->u.i = total;
    return result;
  }

  if (strcmp(cmd, "session.info") == 0) {
    if (args->u.arr.n < 2 || args->u.arr.elem[1].type != RESPT_BULK) {
      resp_object *err = resp_array_init();
      resp_array_append_simple(err, "ERR wrong number of arguments");
      return err;
    }

    const char *session_id = args->u.arr.elem[1].u.s;
    cluster_node_t *node = cluster_find_session_node(session_id);
    if (!node) {
      resp_object *err = resp_array_init();
      resp_array_append_simple(err, "ERR session not found");
      return err;
    }

    return cluster_node_send_command(node, "session.info", session_id, NULL);
  }

  if (strcmp(cmd, "session.destroy") == 0) {
    if (args->u.arr.n < 2 || args->u.arr.elem[1].type != RESPT_BULK) {
      resp_object *err = resp_array_init();
      resp_array_append_simple(err, "ERR wrong number of arguments");
      return err;
    }

    const char *session_id = args->u.arr.elem[1].u.s;
    cluster_node_t *node = cluster_find_session_node(session_id);
    if (!node) {
      resp_object *err = resp_array_init();
      resp_array_append_simple(err, "ERR session not found");
      return err;
    }

    resp_object *resp = cluster_node_send_command(node, "session.destroy", session_id, NULL);
    if (resp && resp->type != RESPT_ERROR) {
      node->session_count--;
    }
    return resp;
  }

  if (strcmp(cmd, "session.socket.create.listen") == 0 ||
      strcmp(cmd, "session.socket.create.connect") == 0 ||
      strcmp(cmd, "session.socket.destroy") == 0 ||
      strcmp(cmd, "session.forward.create") == 0 ||
      strcmp(cmd, "session.forward.destroy") == 0) {

    if (args->u.arr.n < 2 || args->u.arr.elem[1].type != RESPT_BULK) {
      resp_object *err = resp_array_init();
      resp_array_append_simple(err, "ERR wrong number of arguments");
      return err;
    }

    const char *session_id = args->u.arr.elem[1].u.s;
    cluster_node_t *node = cluster_find_session_node(session_id);
    if (!node) {
      resp_object *err = resp_array_init();
      resp_array_append_simple(err, "ERR session not found");
      return err;
    }

    char **argv = malloc(sizeof(char *) * args->u.arr.n);
    for (size_t i = 0; i < args->u.arr.n; i++) {
      if (args->u.arr.elem[i].type == RESPT_BULK) {
        argv[i] = args->u.arr.elem[i].u.s ? strdup(args->u.arr.elem[i].u.s) : "";
      } else if (args->u.arr.elem[i].type == RESPT_INT) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", args->u.arr.elem[i].u.i);
        argv[i] = strdup(buf);
      } else {
        argv[i] = strdup("");
      }
    }

    resp_object *resp = cluster_node_send_command(node, cmd, NULL);

    for (size_t i = 0; i < args->u.arr.n; i++) {
      free(argv[i]);
    }
    free(argv);

    return resp;
  }

  if (strcmp(cmd, "session.forward.list") == 0) {
    if (args->u.arr.n < 2 || args->u.arr.elem[1].type != RESPT_BULK) {
      resp_object *err = resp_array_init();
      resp_array_append_simple(err, "ERR wrong number of arguments");
      return err;
    }

    const char *session_id = args->u.arr.elem[1].u.s;
    cluster_node_t *node = cluster_find_session_node(session_id);
    if (!node) {
      resp_object *err = resp_array_init();
      resp_array_append_simple(err, "ERR session not found");
      return err;
    }

    return cluster_node_send_command(node, "session.forward.list", session_id, NULL);
  }

  if (strcmp(cmd, "system.load") == 0) {
    resp_object *result = resp_array_init();
    char buf[64];

    resp_array_append_bulk(result, "1min");
    resp_array_append_bulk(result, "0.00");
    resp_array_append_bulk(result, "5min");
    resp_array_append_bulk(result, "0.00");
    resp_array_append_bulk(result, "15min");
    resp_array_append_bulk(result, "0.00");

    return result;
  }

  if (strcmp(cmd, "ping") == 0) {
    resp_object *result = resp_array_init();
    resp_array_append_simple(result, "PONG");
    return result;
  }

  resp_object *err = resp_array_init();
  resp_array_append_simple(err, "ERR unknown command");
  return err;
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

static void register_domain_commands(void) {
  api_register_domain_cmd("session.create", cluster_handle_command);
  api_register_domain_cmd("session.list", cluster_handle_command);
  api_register_domain_cmd("session.info", cluster_handle_command);
  api_register_domain_cmd("session.destroy", cluster_handle_command);
  api_register_domain_cmd("session.socket.create.listen", cluster_handle_command);
  api_register_domain_cmd("session.socket.create.connect", cluster_handle_command);
  api_register_domain_cmd("session.socket.destroy", cluster_handle_command);
  api_register_domain_cmd("session.forward.list", cluster_handle_command);
  api_register_domain_cmd("session.forward.create", cluster_handle_command);
  api_register_domain_cmd("session.forward.destroy", cluster_handle_command);
  api_register_domain_cmd("session.count", cluster_handle_command);
  api_register_domain_cmd("system.load", cluster_handle_command);
  log_info("cluster: registered session.* commands");
}

int cli_cmd_cluster(int argc, const char **argv) {
  int daemonize_flag = 0;
  int no_daemonize_flag = 0;

  struct argparse argparse;
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

  domain_config_init();

  if (global_cfg) {
    resp_object *cfg_sec = resp_map_get(global_cfg, "udphole");
    if (cfg_sec) {
      const char *ports_str = resp_map_get_string(cfg_sec, "ports");
      if (ports_str) {
        int port_low = 7000, port_high = 7999;
        sscanf(ports_str, "%d-%d", &port_low, &port_high);
        domain_config_set_ports(port_low, port_high);
      }
      const char *advertise = resp_map_get_string(cfg_sec, "advertise");
      if (advertise) {
        domain_config_set_advertise(advertise);
      }
    }
  }

  if (cluster_config_parse() != 0) {
    log_error("cluster: failed to parse cluster config");
    return 1;
  }

  register_domain_commands();

  log_info("udphole: starting cluster daemon");

  cluster_udata_t *udata = calloc(1, sizeof(cluster_udata_t));
  udata->last_health_check = 0;

  domain_schedmod_pt_create(api_server_pt, NULL);

  log_info("udphole: cluster daemon started");

  return domain_schedmod_main();
}
