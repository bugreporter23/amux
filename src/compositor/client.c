#include <assert.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <linux/input-event-codes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "manager-ui.h"
#include "desktop.h"
#include "bindings.h"
#include "presentation-state.h"

struct frontend {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list toplevels;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	int control_fd, width, height, trace_fd;
	const char *alacritty, *config, *entry;
	char *server_path, *terminal_socket;
	const char *control_path;
	char *key, *received;
	size_t received_size;
	struct wl_event_source *control_source, *terminal_timer;
	struct wl_list requests, panes, launches;
	struct wlr_scene_tree *backgrounds, *surfaces;
	int64_t approved_focus, button_pane, active_window, session;
	int64_t reported_focus[2];
	struct { int64_t session, window; } window_history[32];
	int64_t session_history[32];
	size_t window_history_count, session_history_count;
	uint64_t notified_ns;
	uint32_t buttons_down;
	pid_t terminal_pid;
	bool waiting, attach_sent, terminal_ready, quitting, failed, defer_attach;
	bool prefix, resizing, debug, consumed[KEY_MAX + 1];
	unsigned startup_ticks;
	uint64_t request_sequence, applying_request, input_ns, prefix_ns, revision;
	bool trace_frame_pending;
	struct manager_ui ui;
	struct desktop_bridge desktop;
	struct bindings bindings;
};

struct command {
	struct wl_list link;
	char *frame;
	size_t length, sent;
	bool button;
	bool ui;
	bool session_last;
	int64_t pane;
	uint32_t time, code, state;
	double sx, sy;
	uint64_t id;
};

struct pane {
	struct wl_list link;
	int64_t id;
	struct wlr_box rect;
	struct wlr_scene_tree *frame;
	struct wlr_scene_rect *borders[4];
	bool visible, editor;
	enum presentation_state presentation;
	uint64_t birth_request;
};

struct launch {
	struct wl_list link;
	pid_t pid;
	int64_t pane;
};

struct output {
	struct wl_list link;
	struct frontend *client;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
	struct wl_listener present;
};

struct view {
	struct wl_list link;
	struct frontend *client;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener configure, ack_configure;
	int64_t pane;
	bool mapped, closing;
	uint64_t configure_request;
	uint32_t traced_commit;
};

struct popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct keyboard {
	struct wl_list link;
	struct frontend *client;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

static uint64_t monotonic_ns(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
}

static void __attribute__((format(printf, 5, 6))) trace_event(struct frontend *client, const char *event, uint64_t request,
		int64_t pane, const char *format, ...) {
	if (client->trace_fd < 0) return;
	uint64_t at = monotonic_ns();
	char extra[512] = "", identity[64] = "null", line[1024];
	if (request) snprintf(identity, sizeof(identity), "\"%ld:%" PRIu64 "\"", (long)getpid(), request);
	if (format) {
		va_list args;
		va_start(args, format);
		vsnprintf(extra, sizeof(extra), format, args);
		va_end(args);
	}
	int length = snprintf(line, sizeof(line),
		"{\"ts_ns\":%" PRIu64 ",\"component\":\"frontend\",\"pid\":%ld,"
		"\"event\":\"%s\",\"trace_id\":%s,\"pane\":%" PRId64 "%s}\n",
		at, (long)getpid(), event, identity, pane, extra);
	if (length < 0 || (size_t)length >= sizeof(line)) return;
	size_t written = 0;
	while (written < (size_t)length) {
		ssize_t size = write(client->trace_fd, line + written, (size_t)length - written);
		if (size < 0 && errno == EINTR) continue;
		if (size <= 0) {
			perror("trace disabled"); close(client->trace_fd); client->trace_fd = -1; return;
		}
		written += (size_t)size;
	}
}

static json_object *field(json_object *object, const char *name) {
	json_object *value = NULL;
	if (!object) return NULL;
	json_object_object_get_ex(object, name, &value);
	return value;
}

static json_object *operation(const char *name) {
	json_object *message = json_object_new_object();
	json_object_object_add(message, "op", json_object_new_string(name));
	return message;
}

static struct pane *find_pane(struct frontend *client, int64_t id) {
	struct pane *pane;
	wl_list_for_each(pane, &client->panes, link) {
		if (pane->id == id) return pane;
	}
	return NULL;
}

static struct view *find_view(struct frontend *client, int64_t id) {
	struct view *view;
	wl_list_for_each(view, &client->toplevels, link) {
		if (view->pane == id && view->mapped) return view;
	}
	return NULL;
}

static int64_t keyboard_focus_pane(struct frontend *client) {
	struct view *view;
	wl_list_for_each(view, &client->toplevels, link) {
		if (view->xdg_toplevel->base->surface == client->seat->keyboard_state.focused_surface)
			return view->pane;
	}
	return 0;
}

static void check_keyboard_focus(struct frontend *client) {
	int64_t actual = client->seat->keyboard_state.focused_surface ? keyboard_focus_pane(client) : 0;
	if (!client->seat->keyboard_state.focused_surface || actual == client->approved_focus) {
		client->reported_focus[0] = client->reported_focus[1] = 0;
		return;
	}
	if (client->reported_focus[0] == actual && client->reported_focus[1] == client->approved_focus) return;
	client->reported_focus[0] = actual;
	client->reported_focus[1] = client->approved_focus;
	fprintf(stderr, "amux client: keyboard input for pane %" PRId64 " reaches pane %" PRId64 " (grab %s)\n",
		client->approved_focus, actual,
		client->seat->keyboard_state.grab != client->seat->keyboard_state.default_grab ? "active" : "inactive");
}

static void fail(struct frontend *client, const char *reason) {
	fprintf(stderr, "amux client: %s\n", reason);
	client->failed = client->quitting = true;
	wl_display_terminate(client->wl_display);
}

static void apply_focus(struct frontend *client) {
	struct view *view, *focused = find_view(client, client->approved_focus);
	wl_list_for_each(view, &client->toplevels, link) {
		bool active = view == focused && !client->ui.model.mode && client->desktop.focused;
		if (view->xdg_toplevel->scheduled.activated != active) {
			if (client->applying_request) view->configure_request = client->applying_request;
			uint32_t serial = wlr_xdg_toplevel_set_activated(view->xdg_toplevel, active);
			trace_event(client, "activation_requested", view->configure_request, view->pane,
				",\"serial\":%u,\"active\":%s", serial, active ? "true" : "false");
		}
	}
	if (!focused || client->ui.model.mode || !client->desktop.focused) {
		if (client->seat->keyboard_state.focused_surface)
			trace_event(client, "keyboard_focus_clear", 0, keyboard_focus_pane(client),
				",\"approved_focus\":%" PRId64 ",\"outer_focused\":%s,\"ui_mode\":%d",
				client->approved_focus, client->desktop.focused ? "true" : "false", client->ui.model.mode);
		wlr_seat_keyboard_clear_focus(client->seat);
		return;
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(client->seat);
	if (keyboard && client->seat->keyboard_state.focused_surface != focused->xdg_toplevel->base->surface) {
		uint32_t keys[WLR_KEYBOARD_KEYS_CAP], count = 0;
		for (size_t i = 0; i < keyboard->num_keycodes; i++) {
			uint32_t code = keyboard->keycodes[i];
			if (code > KEY_MAX || !client->consumed[code]) keys[count++] = code;
		}
		wlr_seat_keyboard_notify_enter(client->seat, focused->xdg_toplevel->base->surface,
			keys, count, &keyboard->modifiers);
		trace_event(client, "keyboard_focus_enter", client->applying_request, keyboard_focus_pane(client),
			",\"approved_focus\":%" PRId64 ",\"grabbed\":%s",
			client->approved_focus, client->seat->keyboard_state.grab != client->seat->keyboard_state.default_grab ? "true" : "false");
	}
}

static void update_view(struct view *view) {
	struct pane *pane = find_pane(view->client, view->pane);
	if (!pane) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
		if (view->pane && !view->closing) {
			view->closing = true;
			wlr_xdg_toplevel_send_close(view->xdg_toplevel);
		}
		return;
	}
	int width = pane->rect.width > 2 ? pane->rect.width - 2 : 1;
	int height = pane->rect.height > 2 ? pane->rect.height - 2 : 1;
	wlr_scene_node_set_position(&view->scene_tree->node, pane->rect.x + 1, pane->rect.y + 1);
	struct wlr_box clip = { .x = view->xdg_toplevel->base->geometry.x,
		.y = view->xdg_toplevel->base->geometry.y, .width = width, .height = height };
	wlr_scene_subsurface_tree_set_clip(&view->scene_tree->node, &clip);
	wlr_scene_node_set_enabled(&view->scene_tree->node, view->mapped && pane->visible && pane->rect.width > 2 && pane->rect.height > 2);
	if (view->xdg_toplevel->base->initial_commit || view->xdg_toplevel->scheduled.width != width || view->xdg_toplevel->scheduled.height != height) {
		view->configure_request = view->client->applying_request ? view->client->applying_request : pane->birth_request;
		uint32_t serial = wlr_xdg_toplevel_set_size(view->xdg_toplevel, width, height);
		trace_event(view->client, "configure_requested", view->configure_request, view->pane,
			",\"serial\":%u,\"width\":%d,\"height\":%d", serial, width, height);
	}
}

static void presentation_error(struct frontend *client, const char *message);

static void spawn_views(struct frontend *client) {
	if (!client->key || client->quitting) return;
	struct pane *pane;
	wl_list_for_each(pane, &client->panes, link) {
		bool surface_exists = false, launch_exists = false;
		struct view *view;
		wl_list_for_each(view, &client->toplevels, link)
			if (view->pane == pane->id) surface_exists = true;
		struct launch *pending;
		wl_list_for_each(pending, &client->launches, link)
			if (pending->pane == pane->id) launch_exists = true;
		if (!presentation_can_launch(pane->presentation, pane->visible,
			pane->editor || client->terminal_ready, surface_exists, launch_exists)) continue;
		trace_event(client, "window_launch_begin", pane->birth_request, pane->id, NULL);
		char id[32], class[80];
		snprintf(id, sizeof(id), "%" PRId64, pane->id);
		snprintf(class, sizeof(class), "amux-pane-%" PRId64, pane->id);
		pid_t pid = fork();
		if (pid == 0) {
			close(client->control_fd);
			char *key_argument;
			if (asprintf(&key_argument, "--key=%s", client->key) < 0) _exit(127);
			if (pane->editor)
				execl(client->entry, client->entry, "editor-view", "--socket", client->server_path,
					key_argument, "--pane", id, (char *)NULL);
			else
				execl(client->alacritty, client->alacritty, "msg", "--socket", client->terminal_socket,
					"create-window", "--class", class, "-e", client->entry, "relay",
					"--socket", client->server_path, key_argument, "--pane", id, (char *)NULL);
			_exit(127);
		}
		if (pid < 0) {
			pane->presentation = VIEW_FAILED;
			presentation_error(client, "Cannot launch pane view.");
			continue;
		}
		struct launch *launch = calloc(1, sizeof(*launch));
		launch->pid = pid;
		launch->pane = pane->id;
		wl_list_insert(client->launches.prev, &launch->link);
		pane->presentation = VIEW_STARTING;
		trace_event(client, "window_launch_end", pane->birth_request, pane->id,
			",\"child_pid\":%ld", (long)pid);
	}
}

static void touch_history(struct frontend *client) {
	size_t i = 0;
	while (i < client->session_history_count && client->session_history[i] != client->session) i++;
	if (i == client->session_history_count && i < 32) client->session_history_count++;
	if (i == 32) i = 31;
	memmove(&client->session_history[1], &client->session_history[0], i * sizeof(*client->session_history));
	client->session_history[0] = client->session;
	if (!client->active_window) return;
	for (i = 0; i < client->window_history_count; i++)
		if (client->window_history[i].session == client->session && client->window_history[i].window == client->active_window) break;
	if (i == client->window_history_count && i < 32) client->window_history_count++;
	if (i == 32) i = 31;
	memmove(&client->window_history[1], &client->window_history[0], i * sizeof(*client->window_history));
	client->window_history[0].session = client->session;
	client->window_history[0].window = client->active_window;
}

static void apply_state(struct frontend *client, json_object *state) {
	client->session = json_object_get_int64(field(state, "session"));
	int64_t previous = client->active_window;
	client->active_window = json_object_get_int64(field(state, "active_window"));
	touch_history(client);
	bool presentation_hidden = previous != client->active_window;
	client->approved_focus = 0;
	int active_number = 0;
	struct wl_list old;
	wl_list_init(&old);
	wl_list_insert_list(&old, &client->panes);
	wl_list_init(&client->panes);
	json_object *windows = field(state, "windows");
	for (size_t w = 0; w < json_object_array_length(windows); w++) {
		json_object *window = json_object_array_get_idx(windows, w);
		bool visible = json_object_get_int64(field(window, "window")) == client->active_window;
		if (visible) {
			client->approved_focus = json_object_get_int64(field(window, "focus"));
			active_number = json_object_get_int(field(window, "number"));
		}
		json_object *panes = field(window, "panes");
		for (size_t i = 0; i < json_object_array_length(panes); i++) {
			json_object *description = json_object_array_get_idx(panes, i);
			int64_t id = json_object_get_int64(field(description, "id"));
			struct pane *pane = NULL, *candidate;
			wl_list_for_each(candidate, &old, link) {
				if (candidate->id == id) { pane = candidate; break; }
			}
			if (pane) wl_list_remove(&pane->link);
			else {
				pane = calloc(1, sizeof(*pane));
				pane->id = id;
				pane->birth_request = client->applying_request;
				pane->frame = wlr_scene_tree_create(client->backgrounds);
				const float border[] = {.20f, .24f, .28f, 1.f};
				for (size_t edge = 0; edge < 4; edge++)
					pane->borders[edge] = wlr_scene_rect_create(pane->frame, 1, 1, border);
			}
			const char *kind = json_object_get_string(field(description, "kind"));
			pane->editor = kind && !strcmp(kind, "editor");
			bool pane_visible = visible && json_object_get_boolean(field(description, "visible"));
			if (pane->visible && !pane_visible) presentation_hidden = true;
			pane->visible = pane_visible;
			json_object *rect = field(description, "rect");
			pane->rect = (struct wlr_box) {
				.x = json_object_get_int(field(rect, "x")), .y = json_object_get_int(field(rect, "y")),
				.width = json_object_get_int(field(rect, "width")), .height = json_object_get_int(field(rect, "height")) };
			wlr_scene_node_set_position(&pane->frame->node, pane->rect.x, pane->rect.y);
			wlr_scene_node_set_enabled(&pane->frame->node, pane->visible);
			const float active[] = {.53f, .71f, .82f, 1.f};
			const float inactive[] = {.20f, .24f, .28f, 1.f};
			const struct wlr_box borders[] = {
				{0, 0, pane->rect.width, 1}, {0, pane->rect.height - 1, pane->rect.width, 1},
				{0, 0, 1, pane->rect.height}, {pane->rect.width - 1, 0, 1, pane->rect.height},
			};
			for (size_t edge = 0; edge < 4; edge++) {
				wlr_scene_node_set_position(&pane->borders[edge]->node, borders[edge].x, borders[edge].y);
				wlr_scene_rect_set_size(pane->borders[edge], borders[edge].width, borders[edge].height);
				wlr_scene_rect_set_color(pane->borders[edge], id == client->approved_focus ? active : inactive);
			}
			wl_list_insert(client->panes.prev, &pane->link);
		}
	}
	struct pane *pane, *temporary;
	wl_list_for_each_safe(pane, temporary, &old, link) {
		if (pane->visible) presentation_hidden = true;
		wl_list_remove(&pane->link);
		wlr_scene_node_destroy(&pane->frame->node);
		free(pane);
	}
	struct view *view;
	wl_list_for_each(view, &client->toplevels, link) update_view(view);
	apply_focus(client);
	{
		char title[96];
		if (active_number) snprintf(title, sizeof(title), "amux | %s | window %d", json_object_get_string(field(state, "name")), active_number);
		else snprintf(title, sizeof(title), "amux | session %" PRId64 " | no window", client->session);
		struct output *output;
		wl_list_for_each(output, &client->outputs, link) wlr_wl_output_set_title(output->wlr_output, title);
	}
	if (presentation_hidden && !client->seat->pointer_state.button_count)
		wlr_seat_pointer_clear_focus(client->seat);
	spawn_views(client);
}

static void update_interest(struct frontend *client) {
	uint32_t mask = WL_EVENT_READABLE;
	if (!client->waiting && !wl_list_empty(&client->requests)) mask |= WL_EVENT_WRITABLE;
	wl_event_source_fd_update(client->control_source, mask);
}

static int terminal_ready(void *data);

static bool start_terminal(struct frontend *client) {
	char runtime[PATH_MAX], socket_path[PATH_MAX];
	if (snprintf(runtime, sizeof(runtime), "%s/terminal-XXXXXX", getenv("XDG_RUNTIME_DIR")) >= (int)sizeof(runtime) || !mkdtemp(runtime)) return false;
	if (snprintf(socket_path, sizeof(socket_path), "%s/alacritty.sock", runtime) >= (int)sizeof(socket_path)) return false;
	free(client->terminal_socket);
	client->terminal_socket = strdup(socket_path);
	client->terminal_ready = false;
	client->startup_ticks = 0;
	client->terminal_pid = fork();
	if (client->terminal_pid == 0) {
		close(client->control_fd);
		setenv("XDG_RUNTIME_DIR", runtime, true);
		setenv("AMUX_STATE_STREAM", "1", true);
		char *args[] = {(char *)client->alacritty, "--daemon", "--socket", client->terminal_socket,
			"--config-file", (char *)client->config, NULL, NULL};
		if (getenv("AMUX_GRAPHICS_DEBUG") && strcmp(getenv("AMUX_GRAPHICS_DEBUG"), "1") == 0) args[6] = "-v";
		if (client->trace_fd >= 0) {
			char path[PATH_MAX];
			if (snprintf(path, sizeof(path), "%s/alacritty-%ld.log", getenv("AMUX_TRACE_DIR"), (long)getpid()) >= (int)sizeof(path)) _exit(127);
			int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
			if (fd < 0 || dup2(fd, STDERR_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0) _exit(127);
			if (fd > STDERR_FILENO) close(fd);
			setenv("AMUX_TRACE_TIMINGS", "1", true);
			args[6] = "-v";
		}
		execv(client->alacritty, args);
		_exit(127);
	}
	if (client->terminal_pid < 0) { client->terminal_pid = 0; return false; }
	trace_event(client, "daemon_launched", 0, 0, ",\"child_pid\":%ld", (long)client->terminal_pid);
	if (!client->terminal_timer) client->terminal_timer = wl_event_loop_add_timer(wl_display_get_event_loop(client->wl_display), terminal_ready, client);
	wl_event_source_timer_update(client->terminal_timer, 10);
	return true;
}

static void clear_presentation(struct frontend *client) {
	bindings_stop(client);
	client->prefix = client->resizing = false;
	client->buttons_down = 0;
	client->button_pane = 0;
	wlr_seat_keyboard_notify_clear_focus(client->seat);
	wlr_seat_pointer_clear_focus(client->seat);
	struct pane *pane, *next;
	wl_list_for_each_safe(pane, next, &client->panes, link) {
		wl_list_remove(&pane->link);
		wlr_scene_node_destroy(&pane->frame->node);
		free(pane);
	}
	struct launch *launch, *next_launch;
	wl_list_for_each_safe(launch, next_launch, &client->launches, link) {
		kill(launch->pid, SIGKILL);
		waitpid(launch->pid, NULL, 0);
		wl_list_remove(&launch->link);
		free(launch);
	}
	if (client->terminal_pid) {
		kill(client->terminal_pid, SIGKILL);
		waitpid(client->terminal_pid, NULL, 0);
		client->terminal_pid = 0;
	}
	wl_display_destroy_clients(client->wl_display);
	if (client->terminal_timer) wl_event_source_timer_update(client->terminal_timer, 0);
	client->terminal_ready = client->attach_sent = false;
	client->defer_attach = true;
	client->approved_focus = client->active_window = client->session = 0;
	free(client->key); client->key = NULL;
	json_object_put(client->ui.model.state); client->ui.model.state = NULL;
	json_object_put(client->ui.model.sessions); client->ui.model.sessions = NULL;
}

static struct command *enqueue(struct frontend *client, json_object *message) {
	json_object_object_add(message, "protocol", json_object_new_int(atoi(getenv("AMUX_PROTOCOL_VERSION"))));
	json_object_object_add(message, "version", json_object_new_string(getenv("AMUX_VERSION")));
	if (client->quitting) { json_object_put(message); return NULL; }
	struct command *command = calloc(1, sizeof(*command));
	if (client->trace_fd >= 0) {
		command->id = ++client->request_sequence;
		char identity[64];
		snprintf(identity, sizeof(identity), "%ld:%" PRIu64, (long)getpid(), command->id);
		json_object_object_add(message, "_trace", json_object_new_string(identity));
		trace_event(client, "command_enqueued", command->id, 0,
			",\"op\":\"%s\",\"input_ns\":%" PRIu64 ",\"prefix_ns\":%" PRIu64,
			json_object_get_string(field(message, "op")), client->input_ns, client->prefix ? client->prefix_ns : 0);
	}
	const char *json = json_object_to_json_string_ext(message, JSON_C_TO_STRING_PLAIN);
	command->length = strlen(json) + 1;
	command->frame = malloc(command->length);
	memcpy(command->frame, json, command->length - 1);
	command->frame[command->length - 1] = '\n';
	json_object_put(message);
	wl_list_insert(client->requests.prev, &command->link);
	update_interest(client);
	return command;
}

#include "manager-ui.c"
#include "desktop.c"

static void presentation_error(struct frontend *client, const char *message) {
	if (client->quitting) return;
	ui_open(client, UI_COMMAND);
	snprintf(client->ui.model.error, sizeof(client->ui.model.error), "%s Use view retry [pane] to retry presentation.", message);
	ui_draw(client);
}

static void retry_view(struct frontend *client, int64_t id) {
	struct pane *pane = find_pane(client, id);
	if (!pane || find_view(client, id)) return;
	pane->presentation = presentation_retry(pane->presentation);
	struct launch *launch;
	wl_list_for_each(launch, &client->launches, link)
		if (launch->pane == id) kill(launch->pid, SIGTERM);
	struct view *view;
	wl_list_for_each(view, &client->toplevels, link) {
		if (view->pane == id && !view->closing) {
			view->closing = true;
			wlr_xdg_toplevel_send_close(view->xdg_toplevel);
		}
	}
	if (!pane->editor && !client->terminal_pid) {
		if (!start_terminal(client)) {
			pane->presentation = VIEW_FAILED;
			presentation_error(client, "Cannot launch terminal views.");
			return;
		}
		struct pane *other;
		wl_list_for_each(other, &client->panes, link)
			if (!other->editor) other->presentation = presentation_retry(other->presentation);
	}
	spawn_views(client);
}

static void pane_gone(struct frontend *client, int64_t pane) {
	if (!find_pane(client, pane)) return;
	json_object *message = operation("surface_gone");
	json_object_object_add(message, "pane", json_object_new_int64(pane));
	enqueue(client, message);
}

static void deliver_button(struct frontend *client, struct command *command) {
	struct view *view = find_view(client, command->pane);
	if (!view) {
		if (command->state == WL_POINTER_BUTTON_STATE_RELEASED)
			wlr_seat_pointer_notify_button(client->seat, command->time, command->code, command->state);
		return;
	}
	if (command->state == WL_POINTER_BUTTON_STATE_PRESSED && client->approved_focus != command->pane) return;
	wlr_seat_pointer_notify_enter(client->seat, view->xdg_toplevel->base->surface, command->sx, command->sy);
	wlr_seat_pointer_notify_button(client->seat, command->time, command->code, command->state);
	wlr_seat_pointer_notify_frame(client->seat);
	struct pane *pane = find_pane(client, command->pane);
	if (pane && !pane->visible && !client->seat->pointer_state.button_count)
		wlr_seat_pointer_clear_focus(client->seat);
}

static int64_t previous_session(struct frontend *client, json_object *sessions);

static void run_notify(struct frontend *client, const char *title, const char *body) {
	json_object *command = field(client->bindings.config, "notify");
	size_t count = command ? json_object_array_length(command) : 0;
	if (!count) return;
	pid_t pid = fork();
	if (pid != 0) return;
	setsid();
	char **argv = calloc(count + 3, sizeof(*argv));
	for (size_t i = 0; i < count; i++) argv[i] = (char *)json_object_get_string(json_object_array_get_idx(command, i));
	argv[count] = (char *)title;
	argv[count + 1] = (char *)body;
	execvp(argv[0], argv);
	_exit(127);
}

static void attention_event(struct frontend *client, json_object *message) {
	int64_t pane = json_object_get_int64(field(message, "pane"));
	bool viewed = client->desktop.focused && !client->ui.model.mode && pane == client->approved_focus;
	trace_event(client, "pane_attention", 0, pane, ",\"viewed\":%s", viewed ? "true" : "false");
	if (viewed) return;
	uint64_t now = monotonic_ns();
	if (client->notified_ns && now - client->notified_ns < 1000000000ull) return;
	client->notified_ns = now;
	desktop_attention(client);
	const char *place = NULL;
	json_object *windows = client->ui.model.state ? field(client->ui.model.state, "windows") : NULL;
	for (size_t w = 0; windows && !place && w < json_object_array_length(windows); w++) {
		json_object *window = json_object_array_get_idx(windows, w), *panes = field(window, "panes");
		for (size_t p = 0; panes && p < json_object_array_length(panes); p++)
			if (json_object_get_int64(field(json_object_array_get_idx(panes, p), "id")) == pane)
				place = json_object_get_string(field(window, "name"));
	}
	json_object *notifications = field(message, "notifications");
	size_t count = notifications ? json_object_array_length(notifications) : 0;
	for (size_t i = 0; i < count; i++) {
		json_object *item = json_object_array_get_idx(notifications, i);
		const char *title = json_object_get_string(json_object_array_get_idx(item, 0));
		run_notify(client, *title ? title : place ? place : "Amux", json_object_get_string(json_object_array_get_idx(item, 1)));
	}
	if (!count && json_object_get_boolean(field(message, "bell"))) {
		char body[256];
		snprintf(body, sizeof(body), "Bell in %s", place ? place : "a pane");
		run_notify(client, "Amux", body);
	}
}

static void reply(struct frontend *client, const char *frame) {
	enum json_tokener_error error;
	json_object *message = json_tokener_parse_verbose(frame, &error);
	if (!message || error != json_tokener_success) { fail(client, "invalid server JSON"); return; }
	const char *event = json_object_get_string(field(message, "event"));
	if (event && !strcmp(event, "attention")) {
		attention_event(client, message);
		json_object_put(message);
		return;
	}
	if (event && !strcmp(event, "connection_lost")) {
		clear_presentation(client);
		free(client->server_path); client->server_path = strdup("");
		struct command *pending, *next;
		wl_list_for_each_safe(pending, next, &client->requests, link) {
			wl_list_remove(&pending->link); free(pending->frame); free(pending);
		}
		client->waiting = client->ui.model.pending = false;
		ui_open(client, UI_CONNECTIONS);
		snprintf(client->ui.model.error, sizeof(client->ui.model.error), "%s", json_object_get_string(field(message, "error")));
		ui_draw(client);
		json_object_put(message);
		return;
	}
	struct command *command = NULL;
	if (field(message, "ok")) {
		if (!client->waiting || wl_list_empty(&client->requests)) {
			json_object_put(message); fail(client, "unsolicited command reply"); return;
		}
		command = wl_container_of(client->requests.next, command, link);
		if (command->id) {
			char expected[64];
			snprintf(expected, sizeof(expected), "%ld:%" PRIu64, (long)getpid(), command->id);
			const char *received = json_object_get_string(field(message, "_trace"));
			if (!received || strcmp(expected, received)) {
				json_object_put(message); fail(client, "trace reply identifier mismatch"); return;
			}
		}
		trace_event(client, "reply_received", command->id, 0, NULL);
	}
	json_object *connection = field(message, "connection_path");
	if (connection) {
		clear_presentation(client);
		free(client->server_path);
		client->server_path = strdup(json_object_get_string(connection));
		if (*client->server_path && !start_terminal(client)) presentation_error(client, "Cannot launch terminal views.");
	}
	json_object *key = field(message, "key");
	if (key) { free(client->key); client->key = strdup(json_object_get_string(key)); client->defer_attach = false; }
	json_object *state = field(message, "state");
	client->applying_request = command ? command->id : 0;
	if (state) {
		client->revision++;
		trace_event(client, "state_apply_begin", client->applying_request, 0,
			",\"revision\":%" PRIu64, client->revision);
		apply_state(client, state);
		trace_event(client, "state_applied", client->applying_request, 0,
			",\"revision\":%" PRIu64 ",\"window\":%" PRId64, client->revision, client->active_window);
		client->trace_frame_pending = true;
	}
	client->applying_request = 0;
	if (field(message, "detached") || (field(message, "destroyed") && !state) || (event && !strcmp(event, "destroyed"))) {
		client->quitting = true;
		wl_display_terminate(client->wl_display);
	}
	if (command) {
		bool ok = json_object_get_boolean(field(message, "ok"));
		if (!ok) fprintf(stderr, "manager: %s\n", json_object_get_string(field(message, "error")));
		if (ok && command->button) deliver_button(client, command);
		json_object *sessions = field(message, "sessions");
		int64_t target = ok && command->session_last && sessions ? previous_session(client, sessions) : 0;
		if (target) {
			json_object *select = operation("session_select");
			json_object_object_add(select, "session", json_object_new_int64(target));
			enqueue(client, select);
		}
		ui_reply(client, message, command->ui);
		if (ok && field(message, "retry_view")) retry_view(client, json_object_get_int64(field(message, "retry_view")));
		wl_list_remove(&command->link);
		free(command->frame);
		free(command);
		client->waiting = false;
		if (!client->key && !client->defer_attach) fail(client, "window attachment failed");
		update_interest(client);
	}
	else ui_reply(client, message, false);
	ui_draw(client);
	json_object_put(message);
}

static int control_ready(int fd, uint32_t mask, void *data) {
	struct frontend *client = data;
	if (mask & WL_EVENT_ERROR) { fail(client, "server disconnected"); return 0; }
	if ((mask & WL_EVENT_WRITABLE) && !(mask & WL_EVENT_HANGUP) && !client->waiting && !wl_list_empty(&client->requests)) {
		struct command *command = wl_container_of(client->requests.next, command, link);
		ssize_t size = send(fd, command->frame + command->sent, command->length - command->sent, MSG_NOSIGNAL);
		if (size > 0) command->sent += (size_t)size;
		else if (size < 0 && errno != EAGAIN && errno != EINTR) { fail(client, "command write failed"); return 0; }
		if (command->sent == command->length) {
			trace_event(client, "command_written", command->id, 0, NULL);
			client->waiting = true; update_interest(client);
		}
	}
	if (mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP)) {
		char buffer[8192];
		ssize_t size;
		while ((size = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
			client->received = realloc(client->received, client->received_size + (size_t)size + 1);
			memcpy(client->received + client->received_size, buffer, (size_t)size);
			client->received_size += (size_t)size;
			client->received[client->received_size] = '\0';
		}
		if (size < 0 && errno != EAGAIN && errno != EINTR) { fail(client, "reply read failed"); return 0; }
		char *end;
		while (client->received && (end = memchr(client->received, '\n', client->received_size))) {
			*end = '\0';
			reply(client, client->received);
			size_t used = (size_t)(end - client->received) + 1;
			client->received_size -= used;
			memmove(client->received, end + 1, client->received_size);
			if (client->quitting) return 0;
		}
		if (size == 0) { fail(client, "server disconnected"); return 0; }
	}
	return 0;
}

static void report_viewport(struct frontend *client) {
	if (wl_list_empty(&client->outputs)) return;
	struct output *output = wl_container_of(client->outputs.next, output, link);
	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);
	client->width = width;
	client->height = height;
	if (client->defer_attach && !client->key) { ui_draw(client); return; }
	json_object *message = operation(client->attach_sent ? "viewport" : "attach");
	json_object_object_add(message, "width", json_object_new_int(width));
	json_object_object_add(message, "height", json_object_new_int(height > STATUS_HEIGHT ? height - STATUS_HEIGHT : 1));
	if (!client->attach_sent) {
		json_object_object_add(message, "events", json_object_new_boolean(true));
		if (client->session) json_object_object_add(message, "session", json_object_new_int64(client->session));
	}
	client->attach_sent = true;
	enqueue(client, message);
	ui_draw(client);
}

static int children_ready(int signum, void *data) {
	struct frontend *client = data;
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (pid == client->terminal_pid) {
			client->terminal_pid = 0;
			client->terminal_ready = false;
			if (client->terminal_timer) wl_event_source_timer_update(client->terminal_timer, 0);
			if (!client->quitting) {
				char message[96];
				if (WIFEXITED(status)) snprintf(message, sizeof(message), "Alacritty daemon exited with status %d", WEXITSTATUS(status));
				else snprintf(message, sizeof(message), "Alacritty daemon terminated by signal %d", WTERMSIG(status));
				struct pane *pane;
				wl_list_for_each(pane, &client->panes, link)
					if (!pane->editor) pane->presentation = VIEW_FAILED;
				presentation_error(client, message);
			}
		}
		struct launch *launch, *temporary;
		wl_list_for_each_safe(launch, temporary, &client->launches, link) {
			if (launch->pid != pid) continue;
			struct pane *pane = find_pane(client, launch->pane);
			if (pane && pane->presentation != VIEW_IDLE &&
				(!WIFEXITED(status) || WEXITSTATUS(status) != 0) && !find_view(client, pane->id)) {
				pane->presentation = VIEW_FAILED;
				trace_event(client, pane->editor ? "editor_view_failed" : "terminal_view_failed", 0, pane->id, NULL);
				presentation_error(client, pane->editor ?
					"Editor view exited. Check Neovide on the client PATH or AMUX_NEOVIDE." : "Terminal view exited.");
			}
			wl_list_remove(&launch->link);
			free(launch);
		}
	}
	spawn_views(client);
	return 0;
}

static int terminal_ready(void *data) {
	struct frontend *client = data;
	struct stat info;
	if (stat(client->terminal_socket, &info) == 0 && S_ISSOCK(info.st_mode)) {
		client->terminal_ready = true;
		spawn_views(client);
	} else if (++client->startup_ticks >= 500) {
		if (client->terminal_pid) kill(client->terminal_pid, SIGKILL);
		presentation_error(client, "Alacritty socket not ready within five seconds.");
	} else {
		wl_event_source_timer_update(client->terminal_timer, 10);
	}
	return 0;
}

#include "input.c"
#include "surfaces.c"

static int terminate(int signum, void *data) {
	struct frontend *client = data;
	client->quitting = true;
	wl_display_terminate(client->wl_display);
	return 0;
}

static int dimension(const char *text) {
	char *end;
	errno = 0;
	long value = strtol(text, &end, 10);
	if (errno || *end || value < 1 || value > INT_MAX) {
		fprintf(stderr, "invalid viewport dimension: %s\n", text);
		exit(1);
	}
	return (int)value;
}

int main(int argc, char **argv) {
	struct frontend client = {.control_fd = -1, .trace_fd = -1, .width = 1200, .height = 800};
	enum ui_mode initial_ui = UI_CONNECTIONS;
	static const struct option options[] = {
		{"socket", required_argument, NULL, 's'}, {"width", required_argument, NULL, 'w'},
		{"control", required_argument, NULL, 'c'}, {"pick-connection", no_argument, NULL, 'p'},
		{"pick-session", no_argument, NULL, 'S'},
		{"session", required_argument, NULL, 'i'},
		{"height", required_argument, NULL, 'h'}, {"help", no_argument, NULL, 'H'}, {0}
	};
	int option;
	while ((option = getopt_long(argc, argv, "s:w:h:i:c:pSH", options, NULL)) != -1) {
		if (option == 's') client.server_path = strdup(optarg);
		else if (option == 'c') client.control_path = optarg;
		else if (option == 'p') client.defer_attach = true;
		else if (option == 'S') { client.defer_attach = true; initial_ui = UI_SESSIONS; }
		else if (option == 'w') client.width = dimension(optarg);
		else if (option == 'h') client.height = dimension(optarg);
		else if (option == 'i') client.session = dimension(optarg);
		else {
			printf("Usage: %s --socket <server socket> [--session N --width N --height N]\n", argv[0]);
			return option == 'H' ? 0 : 1;
		}
	}
	client.alacritty = getenv("AMUX_ALACRITTY");
	client.terminal_socket = strdup(getenv("AMUX_AL_SOCKET") ? getenv("AMUX_AL_SOCKET") : "");
	client.config = getenv("AMUX_ALACRITTY_CONFIG");
	client.entry = getenv("AMUX_ENTRY");
	client.debug = getenv("AMUX_DEBUG") && !strcmp(getenv("AMUX_DEBUG"), "1");
	if (!client.server_path || !client.control_path || optind != argc || !client.alacritty || !client.config || !client.entry || !getenv("WAYLAND_DISPLAY") || !getenv("AMUX_VERSION") || !getenv("AMUX_PROTOCOL_VERSION")) {
		fprintf(stderr, "Run through amux in a Wayland desktop\n");
		return 1;
	}
	const char *trace_dir = getenv("AMUX_TRACE_DIR");
	if (trace_dir && *trace_dir) {
		char path[PATH_MAX];
		int length = snprintf(path, sizeof(path), "%s/frontend-%ld.jsonl", trace_dir, (long)getpid());
		if (length < 0 || (size_t)length >= sizeof(path)) { fprintf(stderr, "trace path too long\n"); return 1; }
		client.trace_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
		if (client.trace_fd < 0) { perror("open trace"); return 1; }
		trace_event(&client, "process_start", 0, 0, NULL);
	}
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	if (strlen(client.control_path) >= sizeof(address.sun_path)) { fprintf(stderr, "control socket path too long\n"); return 1; }
	strcpy(address.sun_path, client.control_path);
	client.control_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (client.control_fd < 0 || connect(client.control_fd, (struct sockaddr *)&address, sizeof(address)) < 0) { perror("connect server"); return 1; }
	if (fcntl(client.control_fd, F_SETFL, O_NONBLOCK) < 0) { perror("nonblocking socket"); return 1; }
	setenv("WLR_BACKENDS", "wayland", true);
	setenv("WLR_WL_OUTPUTS", "1", true);
	bool graphics_debug = getenv("AMUX_GRAPHICS_DEBUG") && strcmp(getenv("AMUX_GRAPHICS_DEBUG"), "1") == 0;
	wlr_log_init(graphics_debug ? WLR_DEBUG : WLR_INFO, NULL);
	client.wl_display = wl_display_create();
	struct wl_event_loop *loop = wl_display_get_event_loop(client.wl_display);
	if (!bindings_init(&client)) { fprintf(stderr, "Invalid manager configuration\n"); return 1; }
	wl_list_init(&client.requests);
	wl_list_init(&client.panes);
	wl_list_init(&client.launches);
	wl_list_init(&client.outputs);
	wl_list_init(&client.toplevels);
	wl_list_init(&client.keyboards);
	client.control_source = wl_event_loop_add_fd(loop, client.control_fd, WL_EVENT_READABLE, control_ready, &client);
	wl_event_loop_add_signal(loop, SIGINT, terminate, &client);
	wl_event_loop_add_signal(loop, SIGTERM, terminate, &client);
	wl_event_loop_add_signal(loop, SIGCHLD, children_ready, &client);
	client.backend = wlr_backend_autocreate(loop, NULL);
	if (!client.backend) { wlr_log(WLR_ERROR, "Initialization failed: outer Wayland backend"); return 1; }
	client.renderer = wlr_renderer_autocreate(client.backend);
	if (!client.renderer) { wlr_log(WLR_ERROR, "Graphics initialization failed: renderer creation"); return 1; }
	if (!wlr_renderer_init_wl_display(client.renderer, client.wl_display)) {
		wlr_log(WLR_ERROR, "Graphics initialization failed: renderer Wayland globals"); return 1;
	}
	client.allocator = wlr_allocator_autocreate(client.backend, client.renderer);
	if (!client.allocator) { wlr_log(WLR_ERROR, "Graphics initialization failed: buffer allocator"); return 1; }
	wlr_compositor_create(client.wl_display, 5, client.renderer);
	wlr_subcompositor_create(client.wl_display);
	wlr_data_device_manager_create(client.wl_display);
	client.output_layout = wlr_output_layout_create(client.wl_display);
	client.new_output.notify = frontend_new_output;
	wl_signal_add(&client.backend->events.new_output, &client.new_output);
	client.scene = wlr_scene_create();
	client.scene_layout = wlr_scene_attach_output_layout(client.scene, client.output_layout);
	client.backgrounds = wlr_scene_tree_create(&client.scene->tree);
	client.surfaces = wlr_scene_tree_create(&client.scene->tree);
	wlr_scene_node_set_position(&client.backgrounds->node, 0, STATUS_HEIGHT);
	wlr_scene_node_set_position(&client.surfaces->node, 0, STATUS_HEIGHT);
	client.ui.tree = wlr_scene_tree_create(&client.scene->tree);
	client.xdg_shell = wlr_xdg_shell_create(client.wl_display, 3);
	client.new_xdg_toplevel.notify = frontend_new_xdg_toplevel;
	wl_signal_add(&client.xdg_shell->events.new_toplevel, &client.new_xdg_toplevel);
	client.new_xdg_popup.notify = frontend_new_xdg_popup;
	wl_signal_add(&client.xdg_shell->events.new_popup, &client.new_xdg_popup);
	client.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(client.cursor, client.output_layout);
	client.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	client.cursor_motion.notify = frontend_cursor_motion;
	wl_signal_add(&client.cursor->events.motion, &client.cursor_motion);
	client.cursor_motion_absolute.notify = frontend_cursor_motion_absolute;
	wl_signal_add(&client.cursor->events.motion_absolute, &client.cursor_motion_absolute);
	client.cursor_button.notify = frontend_cursor_button;
	wl_signal_add(&client.cursor->events.button, &client.cursor_button);
	client.cursor_axis.notify = frontend_cursor_axis;
	wl_signal_add(&client.cursor->events.axis, &client.cursor_axis);
	client.cursor_frame.notify = frontend_cursor_frame;
	wl_signal_add(&client.cursor->events.frame, &client.cursor_frame);
	client.seat = wlr_seat_create(client.wl_display, "seat0");
	client.new_input.notify = frontend_new_input;
	wl_signal_add(&client.backend->events.new_input, &client.new_input);
	client.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&client.seat->events.request_set_cursor, &client.request_cursor);
	client.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&client.seat->pointer_state.events.focus_change, &client.pointer_focus_change);
	client.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&client.seat->events.request_set_selection, &client.request_set_selection);
	const char *display = wl_display_add_socket_auto(client.wl_display);
	if (!display || !desktop_init(&client)) {
		wlr_log(WLR_ERROR, "Outer desktop needs a seat and wl_data_device_manager");
		return 1;
	}
	if (!wlr_backend_start(client.backend)) { wlr_log(WLR_ERROR, "Initialization failed: starting outer Wayland backend"); return 1; }
	char nested_display[PATH_MAX];
	if (snprintf(nested_display, sizeof(nested_display), "%s/%s", getenv("XDG_RUNTIME_DIR"), display) >= (int)sizeof(nested_display)) return 1;
	setenv("WAYLAND_DISPLAY", nested_display, true);
	if ((!client.defer_attach || initial_ui == UI_SESSIONS) && !start_terminal(&client))
		presentation_error(&client, "Cannot launch terminal views.");
	if (client.defer_attach) ui_open(&client, initial_ui);
	wl_display_run(client.wl_display);
	client.quitting = true;
	wl_event_source_remove(client.control_source);
	close(client.control_fd);
	bindings_finish(&client);
	desktop_finish(&client);
	wl_display_destroy_clients(client.wl_display);
	if (client.terminal_pid) { kill(client.terminal_pid, SIGKILL); waitpid(client.terminal_pid, NULL, 0); }
	struct launch *launch, *next_launch;
	wl_list_for_each_safe(launch, next_launch, &client.launches, link) {
		waitpid(launch->pid, NULL, 0);
		wl_list_remove(&launch->link);
		free(launch);
	}
	struct command *command, *next_command;
	wl_list_for_each_safe(command, next_command, &client.requests, link) {
		wl_list_remove(&command->link); free(command->frame); free(command);
	}
	struct pane *pane, *next_pane;
	wl_list_for_each_safe(pane, next_pane, &client.panes, link) { wl_list_remove(&pane->link); free(pane); }
	wl_list_remove(&client.new_xdg_toplevel.link);
	wl_list_remove(&client.new_xdg_popup.link);
	wl_list_remove(&client.cursor_motion.link);
	wl_list_remove(&client.cursor_motion_absolute.link);
	wl_list_remove(&client.cursor_button.link);
	wl_list_remove(&client.cursor_axis.link);
	wl_list_remove(&client.cursor_frame.link);
	wl_list_remove(&client.new_input.link);
	wl_list_remove(&client.request_cursor.link);
	wl_list_remove(&client.pointer_focus_change.link);
	wl_list_remove(&client.request_set_selection.link);
	wl_list_remove(&client.new_output.link);
	wlr_scene_node_destroy(&client.scene->tree.node);
	wlr_xcursor_manager_destroy(client.cursor_mgr);
	wlr_cursor_destroy(client.cursor);
	wlr_allocator_destroy(client.allocator);
	wlr_renderer_destroy(client.renderer);
	wlr_backend_destroy(client.backend);
	wl_display_destroy(client.wl_display);
	if (client.trace_fd >= 0) close(client.trace_fd);
	free(client.received);
	free(client.key);
	ui_finish(&client);
	free(client.server_path);
	free(client.terminal_socket);
	return client.failed ? 1 : 0;
}
