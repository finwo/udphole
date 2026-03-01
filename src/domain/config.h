#ifndef UDPHOLE_DOMAIN_CONFIG_H
#define UDPHOLE_DOMAIN_CONFIG_H

#include <stdint.h>

typedef struct {
  int port_low;
  int port_high;
  int port_cur;
  char *advertise_addr;
} udphole_config_t;

extern udphole_config_t *domain_cfg;

void domain_config_init(void);

void domain_config_set_ports(int low, int high);

void domain_config_set_advertise(const char *addr);

void domain_config_free(void);

#endif
