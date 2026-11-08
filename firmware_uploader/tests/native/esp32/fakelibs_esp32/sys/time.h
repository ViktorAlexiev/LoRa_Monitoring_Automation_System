// Заместител на <sys/time.h>: gettimeofday() върху контролируемия часовник g_fakeTimeUs.
#ifndef FAKE_SYS_TIME_H
#define FAKE_SYS_TIME_H

struct timeval {
  long long tv_sec;
  long long tv_usec;
};
int gettimeofday(struct timeval *tv, void *tz);

#endif
