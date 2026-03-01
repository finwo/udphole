#ifndef UDPHOLE_API_SERVER_H
#define UDPHOLE_API_SERVER_H

#include <stdint.h>
#include <stdbool.h>

#include "domain/scheduler.h"

struct api_client_state;
typedef struct api_client_state api_client_t;

PT_THREAD(api_server_pt(struct pt *pt, int64_t timestamp, struct pt_task *task));

void api_register_cmd(const char *name, char (*func)(api_client_t *, char **, int));

bool api_write_ok(api_client_t *c);
bool api_write_err(api_client_t *c, const char *msg);
bool api_write_array(api_client_t *c, size_t nitems);
bool api_write_bulk_cstr(api_client_t *c, const char *s);
bool api_write_bulk_int(api_client_t *c, int val);
bool api_write_int(api_client_t *c, int val);

#endif // UDPHOLE_API_SERVER_H