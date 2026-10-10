#include "probe.h"

#include "xsdk/kernel.h"

// Calls one import first, so xbWatson shows the line only if import thunks work.
void xsdk_start(void)
{
	(void)DbgPrint("xsdk probe: entry reached, DbgPrint works\n");
	probe_spin();
	(void)DbgPrint("xsdk probe: exiting\n");
	XamLoaderTerminateTitle();
}
