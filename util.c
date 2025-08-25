#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// --------------------------------------------------------------------------------------

void fatal(const char * reason)
{
    printf("FATAL: %s\n", reason);
    exit(-1);
}

// --------------------------------------------------------------------------------------
// Task

struct Task
{
    TaskFunc func;
    pthread_t pthread;
    void * userData;
    int joined;
};

static void * taskThreadProc(void * userData)
{
    struct Task * task = (struct Task *)userData;
    task->func(task->userData);
    pthread_exit(NULL);
}

struct Task * taskCreate(TaskFunc func, void * userData)
{
    struct Task * task = calloc(1, sizeof(struct Task));
    task->func = func;
    task->userData = userData;
    task->joined = 0;
    pthread_create(&task->pthread, NULL, taskThreadProc, task);
    return task;
}

void taskJoin(struct Task * task)
{
    if (!task->joined) {
        pthread_join(task->pthread, NULL);
        task->joined = 1;
    }
}

void taskDestroy(struct Task * task)
{
    taskJoin(task);
    free(task);
}
