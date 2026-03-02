#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "domain/config.h"

resp_object *domain_cfg = NULL;

void domain_config_init(void) {
  if (domain_cfg) return;
  domain_cfg = resp_array_init();
}

void domain_config_free(void) {
  if (domain_cfg) {
    resp_free(domain_cfg);
    domain_cfg = NULL;
  }
}

resp_object *domain_config_get_cluster_nodes(void) {
  if (!domain_cfg) return NULL;
  resp_object *udphole_sec = resp_map_get(domain_cfg, "udphole");
  if (!udphole_sec) return NULL;
  return resp_map_get(udphole_sec, "cluster");
}
