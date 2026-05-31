/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * © 2019 Raheman Vaiya (see: LICENSE).
 */

#include "macos.h"

struct screen screens[32];
size_t nr_screens;

static void draw_hook(void *arg, NSView *view)
{
	struct box *b = arg;
	macos_draw_box(b->scr, b->color, b->x, b->y, b->w, b->h, 0);
}

void osx_screen_draw_box(struct screen *scr, int x, int y, int w, int h, const char *color)
{
	assert(scr->nr_boxes < MAX_BOXES);
	struct box *b = &scr->boxes[scr->nr_boxes++];

	b->x = x;
	b->y = y;
	b->w = w;
	b->h = h;
	b->scr = scr;
	b->color = nscolor_from_hex(color);

	window_register_draw_hook(scr->overlay, draw_hook, b);
}

void osx_screen_list(struct screen *rscreens[MAX_SCREENS], size_t *n)
{
	size_t i;

	for (i = 0; i < nr_screens; i++)
		rscreens[i] = &screens[i];

	*n = nr_screens;
}

void osx_screen_clear(struct screen *scr)
{
	scr->nr_boxes = 0;
	scr->overlay->nr_hooks = 0;
}

void osx_screen_get_dimensions(struct screen *scr, int *w, int *h)
{
	*w = scr->w;
	*h = scr->h;
}

void macos_init_screen()
{
	for (NSScreen *screen in NSScreen.screens) {
		struct screen *scr = &screens[nr_screens++];

		scr->x = screen.frame.origin.x;
		scr->y = screen.frame.origin.y;
		scr->w = screen.frame.size.width;
		scr->h = screen.frame.size.height;

		NSNumber *num = [screen.deviceDescription objectForKey:@"NSScreenNumber"];
		scr->display_id = num ? [num unsignedIntValue] : 0;

		scr->overlay = create_overlay_window(scr->x, scr->y, scr->w, scr->h);
	}
}

void osx_screen_get_info(struct screen *scr, struct screen_info *out)
{
	memset(out, 0, sizeof(*out));

	out->x = scr->x;
	out->y = scr->y;
	out->w = scr->w;
	out->h = scr->h;

	out->index = 0;
	out->total = (int)nr_screens;
	out->is_primary = 0;
	strncpy(out->uuid, "unknown", sizeof(out->uuid) - 1);
	strncpy(out->name, "unknown", sizeof(out->name) - 1);

	if (scr->display_id == 0)
		return;

	CGDirectDisplayID active[MAX_SCREENS];
	uint32_t n_active = 0;
	if (CGGetActiveDisplayList(MAX_SCREENS, active, &n_active) == kCGErrorSuccess) {
		out->total = (int)n_active;
		for (uint32_t i = 0; i < n_active; i++) {
			if (active[i] == scr->display_id) {
				out->index = (int)i;
				break;
			}
		}
	}

	out->is_primary = CGDisplayIsMain(scr->display_id) ? 1 : 0;

	CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(scr->display_id);
	if (uuid) {
		CFStringRef cfs = CFUUIDCreateString(NULL, uuid);
		if (cfs) {
			CFStringGetCString(cfs, out->uuid, sizeof(out->uuid),
					   kCFStringEncodingUTF8);
			CFRelease(cfs);
		}
		CFRelease(uuid);
	}

	for (NSScreen *ns in NSScreen.screens) {
		NSNumber *num = [ns.deviceDescription objectForKey:@"NSScreenNumber"];
		if (num && [num unsignedIntValue] == scr->display_id) {
			NSString *nm = [ns localizedName];
			if (nm) {
				strncpy(out->name, nm.UTF8String,
					sizeof(out->name) - 1);
			}
			break;
		}
	}
}
