static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {

	struct keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	if (!keyboard->wlr_keyboard->keymap) return;

	wlr_seat_set_keyboard(keyboard->client->seat, keyboard->wlr_keyboard);

	wlr_seat_keyboard_notify_modifiers(keyboard->client->seat,
		&keyboard->wlr_keyboard->modifiers);
	if (keyboard->client->trace_fd >= 0)
		trace_event(keyboard->client, "keyboard_modifiers", 0, keyboard_focus_pane(keyboard->client),
			",\"approved_focus\":%" PRId64 ",\"modifiers\":%u",
			keyboard->client->approved_focus, wlr_keyboard_get_modifiers(keyboard->wlr_keyboard));
}

#include "bindings.c"

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct frontend *client = keyboard->client;
	struct wlr_keyboard_key_event *event = data;
	if (!keyboard->wlr_keyboard->xkb_state) return;
	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED && event->keycode <= KEY_MAX) {
		handled = client->consumed[event->keycode];
		client->consumed[event->keycode] = false;
		if (event->keycode == client->bindings.repeat_code) bindings_stop(client);
	} else if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		client->input_ns = client->trace_fd >= 0 ? monotonic_ns() : 0;
		const xkb_keysym_t *syms;
		int count = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, event->keycode + 8, &syms);
		uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
		for (int i = 0; i < count && !handled; i++) handled = handle_keybinding(client, syms[i], modifiers);
		if (handled && event->keycode <= KEY_MAX) client->consumed[event->keycode] = true;
		if (client->bindings.repeating || client->bindings.ui_repeat_sym != XKB_KEY_NoSymbol)
			client->bindings.repeat_code = event->keycode;
		ui_draw(client);
		client->input_ns = 0;
	}
	if (!handled) {
		wlr_seat_set_keyboard(client->seat, keyboard->wlr_keyboard);
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) check_keyboard_focus(client);
		if (client->trace_fd >= 0 && (wlr_keyboard_get_modifiers(keyboard->wlr_keyboard) & WLR_MODIFIER_CTRL))
			trace_event(client, "keyboard_control_key", 0, keyboard_focus_pane(client),
				",\"approved_focus\":%" PRId64 ",\"keycode\":%u,\"state\":%u,\"grabbed\":%s",
				client->approved_focus, event->keycode, event->state,
				client->seat->keyboard_state.grab != client->seat->keyboard_state.default_grab ? "true" : "false");
		wlr_seat_keyboard_notify_key(client->seat, event->time_msec, event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {

	struct keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	bindings_stop(keyboard->client);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void frontend_new_keyboard(struct frontend *client,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->client = client;
	keyboard->wlr_keyboard = wlr_keyboard;

	if (client->desktop.keymap) wlr_keyboard_set_keymap(wlr_keyboard, client->desktop.keymap);
	wlr_keyboard_set_repeat_info(wlr_keyboard, client->desktop.rate, client->desktop.delay);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	if (client->desktop.keymap) wlr_seat_set_keyboard(client->seat, keyboard->wlr_keyboard);

	wl_list_insert(&client->keyboards, &keyboard->link);
	apply_focus(client);
}

static void frontend_new_pointer(struct frontend *client,
		struct wlr_input_device *device) {

	wlr_cursor_attach_input_device(client->cursor, device);
}

static void frontend_new_input(struct wl_listener *listener, void *data) {

	struct frontend *client =
		wl_container_of(listener, client, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		frontend_new_keyboard(client, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		frontend_new_pointer(client, device);
		break;
	default:
		break;
	}

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&client->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(client->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct frontend *client = wl_container_of(
			listener, client, request_cursor);

	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		client->seat->pointer_state.focused_client;

	if (focused_client == event->seat_client) {

		wlr_cursor_set_surface(client->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct frontend *client = wl_container_of(
			listener, client, pointer_focus_change);

	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL) {
		wlr_cursor_set_xcursor(client->cursor, client->cursor_mgr, "default");
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {

	struct frontend *client = wl_container_of(
			listener, client, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	if (!client->desktop.focused || client->ui.model.mode) return;
	wlr_seat_set_selection(client->seat, event->source, event->serial);
	desktop_export(client, event->source);
}

static struct view *desktop_toplevel_at(
		struct frontend *client, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {

	struct wlr_scene_node *node = wlr_scene_node_at(
		&client->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;

	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	return tree ? tree->node.data : NULL;
}

static void process_cursor_motion(struct frontend *client, uint32_t time) {
	if ((client->ui.model.mode || client->cursor->y < STATUS_HEIGHT) && !client->seat->pointer_state.button_count) {
		wlr_seat_pointer_clear_focus(client->seat);
		return;
	}
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct view *view = desktop_toplevel_at(client, client->cursor->x, client->cursor->y, &surface, &sx, &sy);
	if (view && surface) {
		wlr_seat_pointer_notify_enter(client->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(client->seat, time, sx, sy);
	} else {
		wlr_cursor_set_xcursor(client->cursor, client->cursor_mgr, "default");
		wlr_seat_pointer_clear_focus(client->seat);
	}
}

static void frontend_cursor_motion(struct wl_listener *listener, void *data) {

	struct frontend *client =
		wl_container_of(listener, client, cursor_motion);
	struct wlr_pointer_motion_event *event = data;

	wlr_cursor_move(client->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);
	process_cursor_motion(client, event->time_msec);
}

static void frontend_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {

	struct frontend *client =
		wl_container_of(listener, client, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(client->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(client, event->time_msec);
}

static void frontend_cursor_button(struct wl_listener *listener, void *data) {
	struct frontend *client = wl_container_of(listener, client, cursor_button);
	uint64_t input_ns = client->trace_fd >= 0 ? monotonic_ns() : 0;
	struct wlr_pointer_button_event *event = data;
	if (client->ui.button && event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		client->ui.button = false;
		return;
	}
	if (!client->buttons_down && event->state == WL_POINTER_BUTTON_STATE_PRESSED && ui_pointer(client, client->cursor->x, client->cursor->y)) {
		client->ui.button = true;
		return;
	}
	double sx = 0, sy = 0;
	struct wlr_surface *surface = NULL;
	struct view *view = desktop_toplevel_at(client, client->cursor->x, client->cursor->y, &surface, &sx, &sy);
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (!client->buttons_down) client->button_pane = view ? view->pane : 0;
		client->buttons_down++;
	} else if (client->buttons_down) client->buttons_down--;
	int64_t pane = client->button_pane;
	if (!pane) return;
	struct pane *geometry = find_pane(client, pane);
	if (!geometry) {
		if (event->state == WL_POINTER_BUTTON_STATE_RELEASED)
			wlr_seat_pointer_notify_button(client->seat, event->time_msec, event->button, event->state);
		return;
	}
	json_object *message = operation(event->state == WL_POINTER_BUTTON_STATE_PRESSED ? "focus" : "state");
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
		json_object_object_add(message, "pane", json_object_new_int64(pane));
	client->input_ns = input_ns;
	struct command *command = enqueue(client, message);
	client->input_ns = 0;
	if (!command) return;
	command->button = true;
	command->pane = pane;
	command->time = event->time_msec;
	command->code = event->button;
	command->state = event->state;
	command->sx = client->cursor->x - geometry->rect.x - 1;
	command->sy = client->cursor->y - STATUS_HEIGHT - geometry->rect.y - 1;
	trace_event(client, "pointer_command", command->id, pane, NULL);
}

static void frontend_cursor_axis(struct wl_listener *listener, void *data) {

	struct frontend *client =
		wl_container_of(listener, client, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	if (client->ui.model.mode) return;

	wlr_seat_pointer_notify_axis(client->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

static void frontend_cursor_frame(struct wl_listener *listener, void *data) {

	struct frontend *client =
		wl_container_of(listener, client, cursor_frame);

	wlr_seat_pointer_notify_frame(client->seat);
}
