#include <stdlib.h>
#include <string.h>

#include "domain/config.h"

udphole_config_t *domain_cfg = NULL;

void domain_config_init(void) {
  if (domain_cfg) {
    domain_config_free();
  }
  domain_cfg = calloc(1, sizeof(udphole_config_t));
  if (domain_cfg) {
    domain_cfg->port_low = 7000;
    domain_cfg->port_high = 7999;
    domain_cfg->port_cur = 7000;
  }
}

void domain_config_set_ports(int low, int high) {
  if (!domain_cfg) domain_config_init();
  if (!domain_cfg) return;
  domain_cfg->port_low = low > 0 ? low : 7000;
  domain_cfg->port_high = high > domain_cfg->port_low ? high : domain_cfg->port_low + 999;
  domain_cfg->port_cur = domain_cfg->port_low;
}

void domain_config_set_advertise(const char *addr) {
  if (!domain_cfg) domain_config_init();
  if (!domain_cfg) return;
  free(domain_cfg->advertise_addr);
  domain_cfg->advertise_addr = addr ? strdup(addr) : NULL;
}

void domain_config_free(void) {
  if (!domain_cfg) return;
  free(domain_cfg->advertise_addr);
  free(domain_cfg);
  domain_cfg = NULL;
}
