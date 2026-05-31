/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * Append-only JSONL action event log for next-click prediction training.
 * See action_log.h and the plan file for schema details.
 */

#include "warpd.h"
#include "action_log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define SCHEMA_VERSION 1
#define EVENT_BUF_SZ (256 * 1024)
#define UUID_STR_SZ 37

static struct {
	int enabled;
	int initialized;

	FILE *fp;
	char *out_buf;

	char session_uuid[UUID_STR_SZ];
	uint64_t seq;

	uint64_t last_event_t_us;
	uint64_t session_idle_us;

	char ext_cmd[512];
	int ext_timeout_ms;

	struct {
		char uuid[64];
		uint64_t sig;
	} hint_sigs[MAX_SCREENS];
	size_t nr_hint_sigs;
} S;

/* -------------------- low-level helpers -------------------- */

static uint64_t wall_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void gen_uuid(char *out)
{
	uint8_t b[16];
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		ssize_t r = read(fd, b, 16);
		(void)r;
		close(fd);
	} else {
		for (int i = 0; i < 16; i++)
			b[i] = (uint8_t)rand();
	}
	b[6] = (b[6] & 0x0F) | 0x40;
	b[8] = (b[8] & 0x3F) | 0x80;
	snprintf(out, UUID_STR_SZ,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
		 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void json_escape(const char *in, char *out, size_t out_sz)
{
	size_t o = 0;
	for (size_t i = 0; in[i] && o + 2 < out_sz; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c == '"' || c == '\\') {
			if (o + 3 >= out_sz) break;
			out[o++] = '\\';
			out[o++] = c;
		} else if (c < 0x20) {
			if (o + 7 >= out_sz) break;
			o += snprintf(out + o, out_sz - o, "\\u%04x", c);
		} else {
			out[o++] = c;
		}
	}
	out[o] = 0;
}

static uint64_t fnv1a64(const void *data, size_t n, uint64_t seed)
{
	const uint8_t *p = data;
	uint64_t h = seed ? seed : 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

/* -------------------- external context (posix_spawn) -------------------- */

static int set_nonblock(int fd)
{
	int f = fcntl(fd, F_GETFL, 0);
	if (f < 0) return -1;
	return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static void resolve_external_context(char *out, size_t out_sz)
{
	out[0] = 0;
	if (!S.ext_cmd[0]) return;

	int p[2];
	if (pipe(p) != 0) return;

	posix_spawn_file_actions_t fa;
	if (posix_spawn_file_actions_init(&fa) != 0) {
		close(p[0]); close(p[1]);
		return;
	}
	posix_spawn_file_actions_addclose(&fa, p[0]);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_addclose(&fa, p[1]);

	char *argv[] = {"/bin/sh", "-c", S.ext_cmd, NULL};
	pid_t pid;
	int rc = posix_spawn(&pid, "/bin/sh", &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(p[1]);

	if (rc != 0) {
		close(p[0]);
		return;
	}

	set_nonblock(p[0]);

	char buf[256];
	size_t got = 0;
	uint64_t deadline_us = get_time_us() + (uint64_t)S.ext_timeout_ms * 1000;

	while (got + 1 < sizeof buf) {
		uint64_t now = get_time_us();
		if (now >= deadline_us) break;

		ssize_t n = read(p[0], buf + got, sizeof buf - 1 - got);
		if (n > 0) {
			got += (size_t)n;
			if (memchr(buf, '\n', got)) break;
		} else if (n == 0) {
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			int wait_ms = (int)((deadline_us - now) / 1000);
			if (wait_ms < 1) wait_ms = 1;
			struct pollfd pfd = { .fd = p[0], .events = POLLIN };
			poll(&pfd, 1, wait_ms);
		} else {
			break;
		}
	}
	close(p[0]);

	int status;
	pid_t w = waitpid(pid, &status, WNOHANG);
	if (w == 0) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		return;
	}

	buf[got] = 0;
	char *nl = memchr(buf, '\n', got);
	if (nl) *nl = 0;

	char *start = buf;
	while (*start == ' ' || *start == '\t') start++;
	size_t len = strlen(start);
	while (len && (start[len-1] == ' ' || start[len-1] == '\t' || start[len-1] == '\r'))
		start[--len] = 0;

	json_escape(start, out, out_sz);
}

/* -------------------- session / time -------------------- */

static uint64_t session_check_and_bump(void)
{
	uint64_t now = get_time_us();
	if (S.last_event_t_us &&
	    S.session_idle_us &&
	    now - S.last_event_t_us > S.session_idle_us) {
		gen_uuid(S.session_uuid);
		S.seq = 0;
	}
	S.last_event_t_us = now;
	return now;
}

/* -------------------- mode / mods rendering -------------------- */

static const char *mode_name(int mode)
{
	switch (mode) {
	case MODE_RESERVED: return "reserved";
	case MODE_HISTORY: return "history";
	case MODE_HINT: return "hint";
	case MODE_HINT2: return "hint2";
	case MODE_GRID: return "grid";
	case MODE_NORMAL: return "normal";
	case MODE_HINTSPEC: return "hintspec";
	case MODE_SCREEN_SELECTION: return "screen_selection";
	default: return "unknown";
	}
}

static void render_mods(uint8_t mods, char *out, size_t out_sz)
{
	out[0] = 0;
	size_t o = 0;
	const char *parts[4] = {NULL, NULL, NULL, NULL};
	int n = 0;
	if (mods & PLATFORM_MOD_CONTROL) parts[n++] = "control";
	if (mods & PLATFORM_MOD_SHIFT) parts[n++] = "shift";
	if (mods & PLATFORM_MOD_META) parts[n++] = "meta";
	if (mods & PLATFORM_MOD_ALT) parts[n++] = "alt";
	for (int i = 0; i < n && o + 16 < out_sz; i++) {
		o += snprintf(out + o, out_sz - o, "%s%s", i ? "|" : "", parts[i]);
	}
}

/* -------------------- screen/window snapshot -------------------- */

static void snapshot_screen(struct screen_info *out)
{
	screen_t scr = NULL;
	platform->mouse_get_position(&scr, NULL, NULL);
	memset(out, 0, sizeof *out);
	if (scr && platform->screen_get_info) {
		platform->screen_get_info(scr, out);
	} else {
		strcpy(out->uuid, "unknown");
		strcpy(out->name, "unknown");
		if (scr) platform->screen_get_dimensions(scr, &out->w, &out->h);
	}
}

static void snapshot_focused(struct focused_window *out)
{
	memset(out, 0, sizeof *out);
	if (platform->get_focused_window) {
		platform->get_focused_window(out);
	} else {
		strcpy(out->bundle_id, "unknown");
		strcpy(out->app_name, "unknown");
	}
}

/* -------------------- JSON building -------------------- */

static int append_common(char *buf, int cap, int *pos, const char *type,
			 uint64_t t_us)
{
	char ses[UUID_STR_SZ];
	memcpy(ses, S.session_uuid, UUID_STR_SZ);
	int w = snprintf(buf + *pos, cap - *pos,
		"{\"schema\":%d,\"t_us\":%llu,\"wall_ms\":%llu,"
		"\"session\":\"%s\",\"seq\":%llu,\"type\":\"%s\"",
		SCHEMA_VERSION,
		(unsigned long long)t_us,
		(unsigned long long)wall_ms(),
		ses,
		(unsigned long long)S.seq,
		type);
	if (w < 0 || w >= cap - *pos) return -1;
	*pos += w;
	return 0;
}

static int append_screen(char *buf, int cap, int *pos,
			 const struct screen_info *s)
{
	char uuid_esc[128], name_esc[256];
	json_escape(s->uuid, uuid_esc, sizeof uuid_esc);
	json_escape(s->name, name_esc, sizeof name_esc);
	int w = snprintf(buf + *pos, cap - *pos,
		",\"screen\":{\"uuid\":\"%s\",\"name\":\"%s\","
		"\"index\":%d,\"total\":%d,\"is_primary\":%d,"
		"\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
		uuid_esc, name_esc,
		s->index, s->total, s->is_primary,
		s->x, s->y, s->w, s->h);
	if (w < 0 || w >= cap - *pos) return -1;
	*pos += w;
	return 0;
}

static int append_window(char *buf, int cap, int *pos,
			 const struct focused_window *fw)
{
	char bid[256], aname[256];
	json_escape(fw->bundle_id, bid, sizeof bid);
	json_escape(fw->app_name, aname, sizeof aname);
	int w = snprintf(buf + *pos, cap - *pos,
		",\"window\":{\"bundle_id\":\"%s\",\"app_name\":\"%s\"}",
		bid, aname);
	if (w < 0 || w >= cap - *pos) return -1;
	*pos += w;
	return 0;
}

static int append_context(char *buf, int cap, int *pos)
{
	char ctx[256] = {0};
	resolve_external_context(ctx, sizeof ctx);
	if (!ctx[0]) return 0;
	int w = snprintf(buf + *pos, cap - *pos, ",\"context\":\"%s\"", ctx);
	if (w < 0 || w >= cap - *pos) return -1;
	*pos += w;
	return 0;
}

static void emit(char *buf, int pos)
{
	if (!S.enabled || !S.fp) return;
	if (pos >= EVENT_BUF_SZ - 2) return;
	buf[pos++] = '}';
	buf[pos++] = '\n';
	fwrite(buf, 1, pos, S.fp);
	fflush(S.fp);
	S.seq++;
}

/* -------------------- public API -------------------- */

static void open_log_file(void)
{
	const char *path = config_get("action_log_path");
	if (!path[0]) path = get_data_path("events.jsonl");

	if (S.fp) { fclose(S.fp); S.fp = NULL; }
	S.fp = fopen(path, "a");
	if (!S.fp) {
		fprintf(stderr, "WARN: action_log: cannot open %s: %s\n",
			path, strerror(errno));
		S.enabled = 0;
		return;
	}
	if (!S.out_buf) {
		S.out_buf = malloc(EVENT_BUF_SZ);
	}
	setvbuf(S.fp, NULL, _IONBF, 0);
}

void action_log_init(void)
{
	if (S.initialized) return;
	S.initialized = 1;

	S.enabled = config_get_int("enable_action_log");
	S.session_idle_us = (uint64_t)config_get_int("action_log_session_idle_s") * 1000000ULL;
	S.ext_timeout_ms = config_get_int("external_context_timeout_ms");
	if (S.ext_timeout_ms < 1) S.ext_timeout_ms = 1;
	strncpy(S.ext_cmd, config_get("external_context_cmd"), sizeof S.ext_cmd - 1);
	S.ext_cmd[sizeof S.ext_cmd - 1] = 0;

	if (!S.enabled) return;

	gen_uuid(S.session_uuid);
	S.seq = 0;
	S.last_event_t_us = 0;
	open_log_file();
}

void action_log_reload(void)
{
	int was_enabled = S.enabled;
	S.enabled = config_get_int("enable_action_log");
	S.session_idle_us = (uint64_t)config_get_int("action_log_session_idle_s") * 1000000ULL;
	S.ext_timeout_ms = config_get_int("external_context_timeout_ms");
	if (S.ext_timeout_ms < 1) S.ext_timeout_ms = 1;
	strncpy(S.ext_cmd, config_get("external_context_cmd"), sizeof S.ext_cmd - 1);
	S.ext_cmd[sizeof S.ext_cmd - 1] = 0;

	if (S.enabled && !was_enabled) {
		gen_uuid(S.session_uuid);
		S.seq = 0;
		S.last_event_t_us = 0;
		open_log_file();
	} else if (!S.enabled && was_enabled) {
		if (S.fp) { fclose(S.fp); S.fp = NULL; }
	}
}

void action_log_close(void)
{
	if (S.fp) { fflush(S.fp); fclose(S.fp); S.fp = NULL; }
}

void action_log_mode_enter(int mode)
{
	if (!S.enabled || !S.fp) return;
	uint64_t now = session_check_and_bump();

	struct screen_info si;
	struct focused_window fw;
	snapshot_screen(&si);
	snapshot_focused(&fw);

	int px = 0, py = 0;
	platform->mouse_get_position(NULL, &px, &py);

	int pos = 0;
	char *buf = S.out_buf;
	if (append_common(buf, EVENT_BUF_SZ, &pos, "mode_enter", now)) return;
	int w = snprintf(buf + pos, EVENT_BUF_SZ - pos,
			 ",\"mode\":\"%s\",\"pointer_x\":%d,\"pointer_y\":%d",
			 mode_name(mode), px, py);
	if (w < 0) return;
	pos += w;
	if (append_screen(buf, EVENT_BUF_SZ, &pos, &si)) return;
	if (append_window(buf, EVENT_BUF_SZ, &pos, &fw)) return;
	if (append_context(buf, EVENT_BUF_SZ, &pos)) return;
	emit(buf, pos);
}

void action_log_mode_abort(int mode, const char *reason)
{
	if (!S.enabled || !S.fp) return;
	uint64_t now = session_check_and_bump();

	struct screen_info si;
	struct focused_window fw;
	snapshot_screen(&si);
	snapshot_focused(&fw);

	char reason_esc[64];
	json_escape(reason ? reason : "", reason_esc, sizeof reason_esc);

	int pos = 0;
	char *buf = S.out_buf;
	if (append_common(buf, EVENT_BUF_SZ, &pos, "mode_abort", now)) return;
	int w = snprintf(buf + pos, EVENT_BUF_SZ - pos,
			 ",\"mode\":\"%s\",\"reason\":\"%s\"",
			 mode_name(mode), reason_esc);
	if (w < 0) return;
	pos += w;
	if (append_screen(buf, EVENT_BUF_SZ, &pos, &si)) return;
	if (append_window(buf, EVENT_BUF_SZ, &pos, &fw)) return;
	if (append_context(buf, EVENT_BUF_SZ, &pos)) return;
	emit(buf, pos);
}

static uint64_t compute_hint_sig(const struct screen_info *si,
				 int vw, int vh,
				 struct hint *hints, size_t n)
{
	uint64_t h = fnv1a64(si->uuid, strlen(si->uuid), 0);
	int meta[4] = { vw, vh, (int)n, 0 };
	h = fnv1a64(meta, sizeof meta, h);
	if (n > 0) {
		struct hint sample[2];
		sample[0] = hints[0];
		sample[1] = hints[n - 1];
		h = fnv1a64(sample, sizeof sample, h);
	}
	return h;
}

static int hint_sig_changed(const struct screen_info *si, uint64_t sig)
{
	for (size_t i = 0; i < S.nr_hint_sigs; i++) {
		if (!strcmp(S.hint_sigs[i].uuid, si->uuid)) {
			if (S.hint_sigs[i].sig == sig) return 0;
			S.hint_sigs[i].sig = sig;
			return 1;
		}
	}
	if (S.nr_hint_sigs < MAX_SCREENS) {
		strncpy(S.hint_sigs[S.nr_hint_sigs].uuid, si->uuid,
			sizeof S.hint_sigs[0].uuid - 1);
		S.hint_sigs[S.nr_hint_sigs].sig = sig;
		S.nr_hint_sigs++;
	}
	return 1;
}

void action_log_hint_present(int mode,
			     struct hint *hints, size_t n,
			     int viewport_w, int viewport_h)
{
	if (!S.enabled || !S.fp) return;

	struct screen_info si;
	struct focused_window fw;
	snapshot_screen(&si);
	snapshot_focused(&fw);

	uint64_t sig = compute_hint_sig(&si, viewport_w, viewport_h, hints, n);
	if (!hint_sig_changed(&si, sig)) return;

	uint64_t now = session_check_and_bump();

	int pos = 0;
	char *buf = S.out_buf;
	if (append_common(buf, EVENT_BUF_SZ, &pos, "hint_present", now)) return;
	int w = snprintf(buf + pos, EVENT_BUF_SZ - pos,
			 ",\"mode\":\"%s\",\"signature\":\"%016llx\","
			 "\"viewport_w\":%d,\"viewport_h\":%d",
			 mode_name(mode),
			 (unsigned long long)sig,
			 viewport_w, viewport_h);
	if (w < 0) return;
	pos += w;
	if (append_screen(buf, EVENT_BUF_SZ, &pos, &si)) return;
	if (append_window(buf, EVENT_BUF_SZ, &pos, &fw)) return;
	if (append_context(buf, EVENT_BUF_SZ, &pos)) return;

	w = snprintf(buf + pos, EVENT_BUF_SZ - pos, ",\"hints\":[");
	if (w < 0) return;
	pos += w;
	for (size_t i = 0; i < n; i++) {
		char label[32];
		json_escape(hints[i].label, label, sizeof label);
		w = snprintf(buf + pos, EVENT_BUF_SZ - pos,
			"%s{\"label\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
			i ? "," : "", label,
			hints[i].x, hints[i].y, hints[i].w, hints[i].h);
		if (w < 0 || pos + w >= EVENT_BUF_SZ - 4) return;
		pos += w;
	}
	w = snprintf(buf + pos, EVENT_BUF_SZ - pos, "]");
	if (w < 0) return;
	pos += w;

	emit(buf, pos);
}

void action_log_click(int mode,
		      const char *mode_trace,
		      struct input_event *trigger,
		      int x, int y,
		      int start_x, int start_y,
		      const char *hint_label,
		      const char *event_type)
{
	if (!S.enabled || !S.fp) return;
	uint64_t now = session_check_and_bump();

	struct screen_info si;
	struct focused_window fw;
	snapshot_screen(&si);
	snapshot_focused(&fw);

	int button = 0;
	uint8_t mods = 0;
	if (trigger) {
		button = trigger->code;
		mods = trigger->mods;
	}
	char mods_str[64];
	render_mods(mods, mods_str, sizeof mods_str);
	char trace_esc[128];
	json_escape(mode_trace ? mode_trace : "", trace_esc, sizeof trace_esc);

	int pos = 0;
	char *buf = S.out_buf;
	if (append_common(buf, EVENT_BUF_SZ, &pos, event_type, now)) return;
	int w = snprintf(buf + pos, EVENT_BUF_SZ - pos,
			 ",\"mode\":\"%s\",\"mode_trace\":\"%s\","
			 "\"button\":%d,\"mods\":\"%s\","
			 "\"x\":%d,\"y\":%d,\"start_x\":%d,\"start_y\":%d",
			 mode_name(mode), trace_esc,
			 button, mods_str,
			 x, y, start_x, start_y);
	if (w < 0) return;
	pos += w;
	if (append_screen(buf, EVENT_BUF_SZ, &pos, &si)) return;
	if (append_window(buf, EVENT_BUF_SZ, &pos, &fw)) return;
	if (hint_label && hint_label[0]) {
		char hl[64];
		json_escape(hint_label, hl, sizeof hl);
		w = snprintf(buf + pos, EVENT_BUF_SZ - pos,
			     ",\"hint_label\":\"%s\"", hl);
		if (w < 0) return;
		pos += w;
	}
	if (append_context(buf, EVENT_BUF_SZ, &pos)) return;
	emit(buf, pos);
}
