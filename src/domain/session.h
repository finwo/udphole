#ifndef UDPHOLE_SESSION_H
#define UDPHOLE_SESSION_H

#include <stdint.h>

#include "domain/scheduler.h"
#include "common/resp.h"

PT_THREAD(session_manager_pt(struct pt *pt, int64_t timestamp, struct pt_task *task));

resp_object *domain_session_create(resp_object *args);
resp_object *domain_session_list(resp_object *args);
resp_object *domain_session_info(resp_object *args);
resp_object *domain_session_destroy(resp_object *args);
resp_object *domain_socket_create_listen(resp_object *args);
resp_object *domain_socket_create_connect(resp_object *args);
resp_object *domain_socket_destroy(resp_object *args);
resp_object *domain_forward_list(resp_object *args);
resp_object *domain_forward_create(resp_object *args);
resp_object *domain_forward_destroy(resp_object *args);
resp_object *domain_session_count(resp_object *args);
resp_object *domain_system_load(resp_object *args);

#endif
