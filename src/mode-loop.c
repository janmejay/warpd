#include "warpd.h"
#include "action_log.h"

#define DEBUG_PRINT(...) do { \
	extern int warpd_debug_enabled; \
	if (warpd_debug_enabled) { \
		fprintf(stderr, __VA_ARGS__); \
		fflush(stderr); \
	} \
} while(0)

static void trace_append(char *trace, size_t sz, const char *m)
{
	size_t len = strlen(trace);
	if (len) {
		if (len + 1 >= sz) return;
		trace[len++] = ',';
		trace[len] = 0;
	}
	strncat(trace, m, sz - strlen(trace) - 1);
}

int mode_loop(int initial_mode, int oneshot, int record_history)
{
	int mode = initial_mode;
	int rc = 0;
	struct input_event *ev = NULL;
	char selected_hint[32] = "";
	char mode_trace[64] = "";
	int start_x = 0, start_y = 0;
	int clicked = 0;
	(void)start_x; (void)start_y; (void)mode_trace; (void)clicked;

	platform->mouse_get_position(NULL, &start_x, &start_y);
	action_log_mode_enter(initial_mode);

	const char *mode_names[] = {
		[MODE_RESERVED] = "RESERVED",
		[MODE_HISTORY] = "HISTORY",
		[MODE_HINT] = "HINT",
		[MODE_HINT2] = "HINT2",
		[MODE_GRID] = "GRID",
		[MODE_NORMAL] = "NORMAL",
		[MODE_HINTSPEC] = "HINTSPEC",
		[MODE_SCREEN_SELECTION] = "SCREEN_SELECTION"
	};

	DEBUG_PRINT("[MODE] Entering mode_loop: initial_mode=%s oneshot=%d\n", 
		    mode_names[initial_mode], oneshot);

	while (1) {
		int btn = 0;
		config_input_whitelist(NULL, 0);

		DEBUG_PRINT("[MODE] Switching to mode: %s\n", mode_names[mode]);

		switch (mode) {
		case MODE_HISTORY:
			DEBUG_PRINT("[MODE] Executing HISTORY mode\n");
			trace_append(mode_trace, sizeof mode_trace, "history");
			if (history_hint_mode(selected_hint, sizeof selected_hint) < 0)
				goto exit;

			ev = NULL;
			mode = MODE_NORMAL;
			break;
		case MODE_HINTSPEC:
			DEBUG_PRINT("[MODE] Executing HINTSPEC mode\n");
			trace_append(mode_trace, sizeof mode_trace, "hintspec");
			hintspec_mode(selected_hint, sizeof selected_hint);
			break;
		case MODE_NORMAL:
			DEBUG_PRINT("[MODE] Executing NORMAL mode\n");
			trace_append(mode_trace, sizeof mode_trace, "normal");
			ev = normal_mode(ev, oneshot,
					 selected_hint[0] ? selected_hint : NULL);
			selected_hint[0] = 0;

			if (config_input_match(ev, "history")) {
				DEBUG_PRINT("[MODE] Transitioning to HISTORY mode\n");
				mode = MODE_HISTORY;
			}
			else if (config_input_match(ev, "hint")) {
				DEBUG_PRINT("[MODE] Transitioning to HINT mode\n");
				mode = MODE_HINT;
			}
			else if (config_input_match(ev, "hint2")) {
				DEBUG_PRINT("[MODE] Transitioning to HINT2 mode\n");
				mode = MODE_HINT2;
			}
			else if (config_input_match(ev, "grid")) {
				DEBUG_PRINT("[MODE] Transitioning to GRID mode\n");
				mode = MODE_GRID;
			}
			else if (config_input_match(ev, "screen")) {
				DEBUG_PRINT("[MODE] Transitioning to SCREEN_SELECTION mode\n");
				mode = MODE_SCREEN_SELECTION;
			}
			else if ((rc = config_input_match(ev, "oneshot_buttons")) || !ev) {
				DEBUG_PRINT("[MODE] Exiting from NORMAL mode (oneshot_buttons or null event)\n");
				goto exit;
			}
			else if (config_input_match(ev, "exit") || !ev) {
				DEBUG_PRINT("[MODE] Exiting from NORMAL mode (exit)\n");
				rc = 0;
				goto exit;
			}

			break;
		case MODE_HINT2:
		case MODE_HINT:
			DEBUG_PRINT("[MODE] Executing HINT mode (pass=%d)\n", mode == MODE_HINT2 ? 2 : 1);
			trace_append(mode_trace, sizeof mode_trace,
				     mode == MODE_HINT2 ? "hint2" : "hint");
			if (full_hint_mode(mode == MODE_HINT2,
					   selected_hint, sizeof selected_hint) < 0)
				goto exit;

			ev = NULL;
			mode = MODE_NORMAL;
			break;
		case MODE_GRID:
			DEBUG_PRINT("[MODE] Executing GRID mode\n");
			trace_append(mode_trace, sizeof mode_trace, "grid");
			ev = grid_mode();
			if (config_input_match(ev, "grid_exit"))
				ev = NULL;
			mode = MODE_NORMAL;
			break;
		case MODE_SCREEN_SELECTION:
			DEBUG_PRINT("[MODE] Executing SCREEN_SELECTION mode\n");
			trace_append(mode_trace, sizeof mode_trace, "screen");
			screen_selection_mode();
			mode = MODE_NORMAL;
			ev = NULL;
			break;
		}

		if (oneshot && (initial_mode != MODE_NORMAL || (btn = config_input_match(ev, "buttons")))) {
			int x, y;
			screen_t scr;

			platform->mouse_get_position(&scr, NULL, NULL);
			platform->mouse_get_position(NULL, &x, &y);

			if (record_history)
				histfile_add(x, y);

			action_log_click(initial_mode, mode_trace, ev,
					 x, y, start_x, start_y,
					 selected_hint[0] ? selected_hint : NULL,
					 "click");
			clicked = 1;

			if (mode == MODE_HINTSPEC)
				printf("%d %d %s\n", x, y, selected_hint);
			else
				printf("%d %d\n", x, y);

			return btn;
		}
	}

exit:
	if (!clicked)
		action_log_mode_abort(initial_mode, "exit");
	DEBUG_PRINT("[MODE] Exiting mode_loop with rc=%d\n", rc);
	return rc;
}

