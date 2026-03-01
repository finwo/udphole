#ifndef UDPHOLE_SESSION_H
#define UDPHOLE_SESSION_H

#include <stdint.h>

#include "domain/scheduler.h"

PT_THREAD(session_manager_pt(struct pt *pt, int64_t timestamp, struct pt_task *task));

#endif // UDPHOLE_SESSION_H