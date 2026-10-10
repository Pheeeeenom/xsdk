#ifndef XSDK_MALLOC_LOCK_H
#define XSDK_MALLOC_LOCK_H

// Must run before a second thread exists.
void xsdk_malloc_lock_init(void);

#endif
