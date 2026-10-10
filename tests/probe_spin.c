#include "probe.h"

// Never calls an import, so a long black screen means the loader accepted the image and ran it.
void xsdk_start(void)
{
	probe_spin();
}
