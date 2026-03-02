#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "common/scheduler.h"

typedef struct {
  int id;
  int count;
  int64_t last_run;
} task_data_t;

int countdown_pt(int64_t timestamp, struct pt_task *task) {
  task_data_t *data = task->udata;

  if (timestamp - data->last_run < 100) {
    return SCHED_RUNNING;
  }
  data->last_run = timestamp;

  printf("Task %d: %d\n", data->id, data->count);
  data->count--;

  if (data->count < 0) {
    printf("Task %d: DONE\n", data->id);
    return SCHED_DONE;
  }

  return SCHED_RUNNING;
}

int main(void) {
  task_data_t tasks[3] = {
    { .id = 1, .count = 3, .last_run = 0 },
    { .id = 2, .count = 3, .last_run = 0 },
    { .id = 3, .count = 3, .last_run = 0 },
  };

  for (int i = 0; i < 3; i++) {
    sched_create(countdown_pt, &tasks[i]);
  }

  printf("Starting countdown test with 3 parallel tasks...\n");
  sched_main();
  printf("Test complete!\n");
  return 0;
}
