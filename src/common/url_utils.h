#ifndef UDPHOLE_URL_UTILS_H
#define UDPHOLE_URL_UTILS_H

#include "finwo/url-parser.h"

int parse_address_url(const char *addr, struct parsed_url **out);

#endif
