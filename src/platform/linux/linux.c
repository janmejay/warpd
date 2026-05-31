#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../platform.h"

void x_init();
void wayland_init();

void warpd_warn_once(const char *fn)
{
	static const char *seen[32];
	static int n = 0;
	for (int i = 0; i < n; i++)
		if (seen[i] == fn) return;
	if (n < 32) seen[n++] = fn;
	fprintf(stderr, "WARN: %s not implemented on this platform\n", fn);
}

#ifndef WARPD_X
void x_init()
{
	fprintf(stderr, "ERROR: warpd compiled without X support\n");
	exit(-1);
}
#endif

#ifndef WARPD_WAYLAND
void wayland_init()
{
	fprintf(stderr, "ERROR: warpd compiled without wayland support\n");
	exit(-1);
}
#endif

void platform_run(int (*main) (struct platform *platform))
{
	struct platform platform;

	if (getenv("WAYLAND_DISPLAY"))
		wayland_init(&platform);
	else
		x_init(&platform);

	exit(main(&platform));
}
