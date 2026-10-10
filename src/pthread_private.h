#ifndef XSDK_PTHREAD_PRIVATE_H
#define XSDK_PTHREAD_PRIVATE_H

// Must run before a second thread exists; it also links the whole POSIX threads layer into every title.
void xsdk_pthread_init(void);
// Runs the calling thread's thread-specific data destructors; threads the SDK starts call it as they end.
void xsdk_pthread_thread_end(void);

#endif
