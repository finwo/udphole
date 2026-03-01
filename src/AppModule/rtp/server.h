#ifndef __APPMODULE_RTP_SERVER_H__
#define __APPMODULE_RTP_SERVER_H__

#include <stdint.h>

#include "SchedulerModule/scheduler.h"

PT_THREAD(udphole_manager_pt(struct pt *pt, int64_t timestamp, struct pt_task *task));

#endif // __APPMODULE_RTP_SERVER_H__