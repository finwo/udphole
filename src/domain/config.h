#ifndef UDPHOLE_DOMAIN_CONFIG_H
#define UDPHOLE_DOMAIN_CONFIG_H

#include <stdint.h>

#include "common/resp.h"

extern resp_object *domain_cfg;

void domain_config_init(void);

void domain_config_free(void);

resp_object *domain_config_get_cluster_nodes(void);

#endif
