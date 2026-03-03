#include "url_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rxi/log.h"

int parse_address_url(const char *addr, struct parsed_url **out) {
  if (!addr || !addr[0]) {
    return -1;
  }

  if (strstr(addr, "://") != NULL) {
    struct parsed_url *purl = parse_url(addr);
    if (!purl) {
      log_error("url_utils: failed to parse URL '%s'", addr);
      return -1;
    }
    *out = purl;
    return 0;
  }

  size_t len        = strlen(addr);
  char  *normalized = NULL;

  if (addr[0] == ':') {
    normalized = malloc(len + 8);
    if (!normalized) return -1;
    snprintf(normalized, len + 8, "tcp://*%s", addr);
  } else if (addr[0] >= '0' && addr[0] <= '9') {
    normalized = malloc(len + 8);
    if (!normalized) return -1;
    snprintf(normalized, len + 8, "tcp://*:%s", addr);
  } else {
    normalized = malloc(len + 8);
    if (!normalized) return -1;
    snprintf(normalized, len + 8, "tcp://%s", addr);
  }

  struct parsed_url *purl = parse_url(normalized);
  free(normalized);

  if (!purl) {
    log_error("url_utils: failed to parse normalized address '%s'", addr);
    return -1;
  }

  *out = purl;
  return 0;
}
