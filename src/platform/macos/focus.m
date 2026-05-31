/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * © 2019 Raheman Vaiya (see: LICENSE).
 */

#include "macos.h"

void osx_get_focused_window(struct focused_window *out)
{
	memset(out, 0, sizeof(*out));
	strncpy(out->bundle_id, "unknown", sizeof(out->bundle_id) - 1);
	strncpy(out->app_name, "unknown", sizeof(out->app_name) - 1);

	NSRunningApplication *app =
	    [[NSWorkspace sharedWorkspace] frontmostApplication];
	if (!app)
		return;

	NSString *bid = app.bundleIdentifier;
	if (bid) {
		strncpy(out->bundle_id, bid.UTF8String,
			sizeof(out->bundle_id) - 1);
		out->bundle_id[sizeof(out->bundle_id) - 1] = '\0';
	}

	NSString *nm = app.localizedName;
	if (nm) {
		strncpy(out->app_name, nm.UTF8String,
			sizeof(out->app_name) - 1);
		out->app_name[sizeof(out->app_name) - 1] = '\0';
	}
}
