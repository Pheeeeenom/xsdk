#ifndef XSDK_REENT_PRIVATE_H
#define XSDK_REENT_PRIVATE_H

// Must run after xsdk_pthread_init and before a second thread exists.
void xsdk_reent_init(void);

#endif
