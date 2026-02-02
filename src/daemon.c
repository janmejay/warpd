#include "warpd.h"

#define DEBUG_PRINT(...) do { \
	extern int warpd_debug_enabled; \
	if (warpd_debug_enabled) { \
		fprintf(stderr, __VA_ARGS__); \
		fflush(stderr); \
	} \
} while(0)

static const char *activation_keys[] = {
	"activation_key",
	"hint_activation_key",
	"grid_activation_key",
	"hint_oneshot_key",
	"screen_activation_key",
	"hint2_activation_key",
	"hint2_oneshot_key",
	"history_activation_key",
};

static struct input_event activation_events[sizeof activation_keys / sizeof activation_keys[0]];

static void reload_config(const char *path)
{
	int i;

	parse_config(path);

	init_hints();
	init_mouse();

	for (i = 0; i < sizeof activation_keys / sizeof activation_keys[0]; i++)
		input_parse_string(&activation_events[i], config_get(activation_keys[i]));

}

void daemon_loop(const char *config_path)
{
	size_t i;


	platform->monitor_file(config_path);
	reload_config(config_path);

	DEBUG_PRINT("[DAEMON] Daemon loop started, waiting for activation keys\n");

	while (1) {
		int mode = 0;
		DEBUG_PRINT("[DAEMON] Waiting for activation event...\n");
		struct input_event *ev = platform->input_wait(activation_events,
							     sizeof(activation_events) /
							     sizeof(activation_events[0]));

		if (!ev) {
			DEBUG_PRINT("[DAEMON] Config reload triggered\n");
			reload_config(config_path);
			continue;
		}

		DEBUG_PRINT("[DAEMON] Activation event received: code=%d mods=%d\n", ev->code, ev->mods);

		config_input_whitelist(activation_keys, sizeof activation_keys / sizeof activation_keys[0]);

		if (config_input_match(ev, "activation_key")) {
			DEBUG_PRINT("[DAEMON] Activation key -> MODE_NORMAL\n");
			mode = MODE_NORMAL;
		}
		else if (config_input_match(ev, "grid_activation_key")) {
			DEBUG_PRINT("[DAEMON] Grid activation key -> MODE_GRID\n");
			mode = MODE_GRID;
		}
		else if (config_input_match(ev, "hint_activation_key")) {
			DEBUG_PRINT("[DAEMON] Hint activation key -> MODE_HINT\n");
			mode = MODE_HINT;
		}
		else if (config_input_match(ev, "hint2_activation_key")) {
			DEBUG_PRINT("[DAEMON] Hint2 activation key -> MODE_HINT2\n");
			mode = MODE_HINT2;
		}
		else if (config_input_match(ev, "screen_activation_key")) {
			DEBUG_PRINT("[DAEMON] Screen activation key -> MODE_SCREEN_SELECTION\n");
			mode = MODE_SCREEN_SELECTION;
		}
		else if (config_input_match(ev, "history_activation_key")) {
			DEBUG_PRINT("[DAEMON] History activation key -> MODE_HISTORY\n");
			mode = MODE_HISTORY;
		}
		else if (config_input_match(ev, "hint2_oneshot_key")) {
			DEBUG_PRINT("[DAEMON] Hint2 oneshot key\n");
			full_hint_mode(1);
			continue;
		} else if (config_input_match(ev, "hint_oneshot_key")) {
			DEBUG_PRINT("[DAEMON] Hint oneshot key\n");
			full_hint_mode(0);
			continue;
		} else if (config_input_match(ev, "history_oneshot_key")) {
			DEBUG_PRINT("[DAEMON] History oneshot key\n");
			history_hint_mode();
			continue;
		}

		DEBUG_PRINT("[DAEMON] Starting mode_loop with mode=%d\n", mode);
		mode_loop(mode, 0, 1);
		DEBUG_PRINT("[DAEMON] Returned from mode_loop\n");
	}
}

