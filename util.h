#ifndef VAAT_UTIL_H
#define VAAT_UTIL_H

void fatal(const char * reason);

typedef void (*TaskFunc)(void * userData);

struct Task * taskCreate(TaskFunc func, void * userData);
void taskJoin(struct Task * task);
void taskDestroy(struct Task * task);

#endif
