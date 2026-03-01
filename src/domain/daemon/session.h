#ifndef UDPHOLE_SESSION_H
#define UDPHOLE_SESSION_H

#include <stdint.h>

#include "domain/scheduler.h"
#include "common/resp.h"

PT_THREAD(session_manager_pt(struct pt *pt, int64_t timestamp, struct pt_task *task));

resp_object *domain_session_create(const char *cmd, resp_object *args);
resp_object *domain_session_list(const char *cmd, resp_object *args);
resp_object *domain_session_info(const char *cmd, resp_object *args);
resp_object *domain_session_destroy(const char *cmd, resp_object *args);
resp_object *domain_socket_create_listen(const char *cmd, resp_object *args);
resp_object *domain_socket_create_connect(const char *cmd, resp_object *args);
resp_object *domain_socket_destroy(const char *cmd, resp_object *args);
resp_object *domain_forward_list(const char *cmd, resp_object *args);
resp_object *domain_forward_create(const char *cmd, resp_object *args);
resp_object *domain_forward_destroy(const char *cmd, resp_object *args);
resp_object *domain_session_count(const char *cmd, resp_object *args);
resp_object *domain_system_load(const char *cmd, resp_object *args);

#endif
