/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * © 2019 Raheman Vaiya (see: LICENSE).
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stdlib.h>

#define PLATFORM_MOD_CONTROL 1
#define PLATFORM_MOD_SHIFT 2
#define PLATFORM_MOD_META 4
#define PLATFORM_MOD_ALT 8

#define SCROLL_DOWN 1
#define SCROLL_RIGHT 2
#define SCROLL_LEFT 3
#define SCROLL_UP 4

#define MAX_HINTS 2048
#define MAX_SCREENS 32

struct input_event {
	uint8_t code;
	uint8_t mods;
	uint8_t pressed;
};

struct hint {
	int x;
	int y;

	int w;
	int h;

	char label[16];
};

struct focused_window {
	char bundle_id[128];
	char app_name[128];
};

struct screen_info {
	char uuid[64];
	char name[128];
	int index;
	int total;
	int is_primary;
	int x;
	int y;
	int w;
	int h;
};

struct screen;
typedef struct screen *screen_t;

struct platform {
	/* Input */

	void (*input_grab_keyboard)();
	void (*input_ungrab_keyboard)();

	struct input_event *(*input_next_event)(int timeout);
	uint8_t (*input_lookup_code)(const char *name, int *shifted);
	const char *(*input_lookup_name)(uint8_t code, int shifted);

	/*
	 * Efficiently listen for one or more input events before
	 * grabbing the keyboard (including the event itself)
	 * and returning the matched event.
	 */
	struct input_event *(*input_wait)(struct input_event *events, size_t sz);

	void (*mouse_move)(screen_t scr, int x, int y);
	void (*mouse_down)(int btn);

	void (*mouse_up)(int btn);
	void (*mouse_click)(int btn);

	void (*mouse_get_position)(screen_t *scr, int *x, int *y);
	void (*mouse_show)();
	void (*mouse_hide)();

	void (*screen_get_dimensions)(screen_t scr, int *w, int *h);
	void (*screen_draw_box)(screen_t scr, int x, int y, int w, int h, const char *color);
	void (*screen_clear)(screen_t scr);
	void (*screen_list)(screen_t scr[MAX_SCREENS], size_t *n);

	void (*init_hint)(const char *bg, const char *fg, int border_radius, const char *font_family);

	/* 
	 * Modifications to files passed into this function will interrupt
	 * input_wait (which returns NULL).
	 */
	void (*monitor_file)(const char *path);

	/* Hints are centered around the provided x,y coordinates. */
	void (*hint_draw)(struct screen *scr, struct hint *hints, size_t n);

	void (*scroll)(int direction);

	/*
	 * Populate `out` with information about the frontmost application's
	 * focused window. On platforms where this is not implemented, fills
	 * bundle_id and app_name with "unknown".
	 */
	void (*get_focused_window)(struct focused_window *out);

	/*
	 * Populate `out` with stable identity + geometry for the given screen.
	 * uuid persists across reboots and reconnects (CGDisplayCreateUUIDFromDisplayID
	 * on macOS). On platforms where the stable id is unavailable, uuid and
	 * name are set to "unknown" and geometry is filled from screen_get_dimensions.
	 */
	void (*screen_get_info)(screen_t scr, struct screen_info *out);

	void (*copy_selection)();

	/*
	* Draw operations may (or may not) be queued until this function
	* is called.
	*/
	void (*commit)();
};

void platform_run(int (*main) (struct platform *platform));
#endif
