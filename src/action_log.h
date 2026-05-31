/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * Action event log: structured JSONL stream of warpd interactions intended
 * as training data for a next-click prediction model.
 *
 * Schema version 1. See plan: ok-let-us-set-sequential-dragon.md.
 */

#ifndef WARPD_ACTION_LOG_H
#define WARPD_ACTION_LOG_H

#include <stddef.h>
#include "platform.h"

void action_log_init(void);
void action_log_reload(void);
void action_log_close(void);

void action_log_mode_enter(int mode);
void action_log_mode_abort(int mode, const char *reason);

void action_log_hint_present(int mode,
			     struct hint *hints, size_t n,
			     int viewport_w, int viewport_h);

void action_log_click(int mode,
		      const char *mode_trace,
		      struct input_event *trigger,
		      int x, int y,
		      int start_x, int start_y,
		      const char *hint_label,
		      const char *event_type);

#endif
