#ifndef XSDK_TIME_PRIVATE_H
#define XSDK_TIME_PRIVATE_H

#include <stdint.h>
#include <time.h>

// NT time counts 100 ns intervals from 1601; POSIX time counts from 1970.
struct timespec xsdk_time_from_nt(uint64_t nt_time);

#endif
