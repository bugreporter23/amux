#include <sys/mman.h>
#include <wlr/backend/multi.h>

struct desktop_offer {
	struct wlr_data_source base;
	struct desktop_bridge *bridge;
	struct wl_data_offer *proxy;
	struct wl_list link;
	bool own;
};

static void desktop_offer_send(struct wlr_data_source *base, const char *mime, int32_t fd) {
	struct desktop_offer *offer = wl_container_of(base, offer, base);
	trace_event(offer->bridge->client, "clipboard_import_transfer", 0, 0, NULL);
	wl_data_offer_receive(offer->proxy, mime, fd);
	close(fd);
	wl_display_flush(offer->bridge->display);
}

static void desktop_offer_destroy(struct wlr_data_source *base) {
	struct desktop_offer *offer = wl_container_of(base, offer, base);
	wl_data_offer_destroy(offer->proxy);
	wl_list_remove(&offer->link);
	free(offer);
}

static const struct wlr_data_source_impl desktop_offer_impl = {
	.send = desktop_offer_send, .destroy = desktop_offer_destroy,
};

static void desktop_mime(void *data, struct wl_data_offer *proxy, const char *mime) {
	struct desktop_offer *offer = data;
	struct desktop_bridge *bridge = offer->bridge;
	char **slot = wl_array_add(&offer->base.mime_types, sizeof(char *));
	if (slot) *slot = strdup(mime);
	if (!strcmp(mime, bridge->marker)) offer->own = true;
}

static void desktop_offer_action(void *data, struct wl_data_offer *proxy, uint32_t action) {}
static const struct wl_data_offer_listener desktop_offer_listener = {
	.offer = desktop_mime, .source_actions = desktop_offer_action, .action = desktop_offer_action,
};

static void desktop_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *proxy) {
	struct desktop_bridge *bridge = data;
	struct desktop_offer *offer = calloc(1, sizeof(*offer));
	wlr_data_source_init(&offer->base, &desktop_offer_impl);
	offer->bridge = bridge;
	offer->proxy = proxy;
	wl_list_insert(&bridge->offers, &offer->link);
	wl_data_offer_add_listener(proxy, &desktop_offer_listener, offer);
}

static void desktop_selection(void *data, struct wl_data_device *device, struct wl_data_offer *proxy) {
	struct desktop_bridge *bridge = data;
	struct desktop_offer *offer = proxy ? wl_data_offer_get_user_data(proxy) : NULL;
	trace_event(bridge->client, "clipboard_offer", 0, 0,
		",\"own\":%s,\"present\":%s", offer && offer->own ? "true" : "false", offer ? "true" : "false");
	if (offer && offer->own) { wlr_data_source_destroy(&offer->base); return; }
	if (offer) { wl_list_remove(&offer->link); wl_list_init(&offer->link); }
	wlr_seat_set_selection(bridge->client->seat, offer ? &offer->base : NULL,
		wl_display_next_serial(bridge->client->wl_display));
}

static void desktop_drag_enter(void *data, struct wl_data_device *device, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *proxy) {
	if (proxy) {
		struct desktop_offer *offer = wl_data_offer_get_user_data(proxy);
		wlr_data_source_destroy(&offer->base);
	}
}
static void desktop_drag_leave(void *data, struct wl_data_device *device) {}
static void desktop_drag_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y) {}
static void desktop_drag_drop(void *data, struct wl_data_device *device) {}
static const struct wl_data_device_listener desktop_device_listener = {
	.data_offer = desktop_data_offer, .enter = desktop_drag_enter, .leave = desktop_drag_leave,
	.motion = desktop_drag_motion, .drop = desktop_drag_drop, .selection = desktop_selection,
};

static void desktop_clear_export(struct desktop_bridge *bridge) {
	if (bridge->outgoing) { wl_data_source_destroy(bridge->outgoing); bridge->outgoing = NULL; }
	if (bridge->exported) {
		wl_list_remove(&bridge->source_destroy.link);
		bridge->exported = NULL;
	}
}

static void desktop_source_destroy(struct wl_listener *listener, void *data) {
	struct desktop_bridge *bridge = wl_container_of(listener, bridge, source_destroy);
	wl_list_remove(&bridge->source_destroy.link);
	bridge->exported = NULL;
}

static void desktop_source_send(void *data, struct wl_data_source *proxy, const char *mime, int32_t fd) {
	struct desktop_bridge *bridge = data;
	if (proxy != bridge->outgoing || !bridge->exported || !strcmp(mime, bridge->marker)) close(fd);
	else {
		trace_event(bridge->client, "clipboard_export_transfer", 0, 0, NULL);
		wlr_data_source_send(bridge->exported, mime, fd);
	}
}
static void desktop_source_target(void *data, struct wl_data_source *proxy, const char *mime) {}
static void desktop_source_cancel(void *data, struct wl_data_source *proxy) {
	struct desktop_bridge *bridge = data;
	if (proxy == bridge->outgoing) desktop_clear_export(bridge);
}
static void desktop_source_done(void *data, struct wl_data_source *proxy) {}
static void desktop_source_action(void *data, struct wl_data_source *proxy, uint32_t action) {}
static const struct wl_data_source_listener desktop_source_listener = {
	.target = desktop_source_target, .send = desktop_source_send, .cancelled = desktop_source_cancel,
	.dnd_drop_performed = desktop_source_done, .dnd_finished = desktop_source_done,
	.action = desktop_source_action,
};

static void desktop_export(struct frontend *client, struct wlr_data_source *source) {
	struct desktop_bridge *bridge = &client->desktop;
	trace_event(client, "clipboard_export", 0, 0, ",\"present\":%s", source ? "true" : "false");
	struct wl_data_source *previous = bridge->outgoing;
	bridge->outgoing = NULL;
	if (bridge->exported) {
		wl_list_remove(&bridge->source_destroy.link);
		bridge->exported = NULL;
	}
	if (source) {
		bridge->outgoing = wl_data_device_manager_create_data_source(bridge->manager);
		bridge->exported = source;
		bridge->source_destroy.notify = desktop_source_destroy;
		wl_signal_add(&source->events.destroy, &bridge->source_destroy);
		wl_data_source_add_listener(bridge->outgoing, &desktop_source_listener, bridge);
		char **mime;
		wl_array_for_each(mime, &source->mime_types) wl_data_source_offer(bridge->outgoing, *mime);
		wl_data_source_offer(bridge->outgoing, bridge->marker);
	}
	if (bridge->serial) wl_data_device_set_selection(bridge->device, bridge->outgoing, bridge->serial);
	if (previous) wl_data_source_destroy(previous);
	wl_display_flush(bridge->display);
}

static void desktop_keymap(void *data, struct wl_keyboard *proxy, uint32_t format, int32_t fd, uint32_t size) {
	struct desktop_bridge *bridge = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !size) { close(fd); return; }
	char *text = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (text == MAP_FAILED) return;
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(context, text, size - 1,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_context_unref(context);
	munmap(text, size);
	if (!keymap) { wlr_log(WLR_ERROR, "Invalid outer desktop keymap"); return; }
	xkb_keymap_unref(bridge->keymap);
	bridge->keymap = keymap;
	struct keyboard *keyboard;
	wl_list_for_each(keyboard, &bridge->client->keyboards, link) {
		wlr_keyboard_set_keymap(keyboard->wlr_keyboard, keymap);
		wlr_seat_set_keyboard(bridge->client->seat, keyboard->wlr_keyboard);
	}
	apply_focus(bridge->client);
}

static void desktop_serial(struct desktop_bridge *bridge, uint32_t serial) {
	bool first = !bridge->serial;
	bridge->serial = serial;
	if (first && bridge->outgoing) {
		wl_data_device_set_selection(bridge->device, bridge->outgoing, serial);
		wl_display_flush(bridge->display);
	}
}
static void desktop_key_enter(void *data, struct wl_keyboard *proxy, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	desktop_serial(data, serial);
	struct desktop_bridge *bridge = data;
	bridge->focused = true;
	trace_event(bridge->client, "outer_keyboard_focus", 0, 0, ",\"focused\":true");
	apply_focus(bridge->client);
}
static void desktop_key_leave(void *data, struct wl_keyboard *proxy, uint32_t serial, struct wl_surface *surface) {
	struct desktop_bridge *bridge = data;
	bridge->focused = false;
	trace_event(bridge->client, "outer_keyboard_focus", 0, 0, ",\"focused\":false");
	bindings_stop(bridge->client);
	apply_focus(bridge->client);
}
static void desktop_key(void *data, struct wl_keyboard *proxy, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	desktop_serial(data, serial);
	struct desktop_bridge *bridge = data;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		bridge->input_serial = serial;
		bridge->input_ns = monotonic_ns();
	}
}
static void desktop_modifiers(void *data, struct wl_keyboard *proxy, uint32_t serial, uint32_t depressed,
		uint32_t latched, uint32_t locked, uint32_t group) { desktop_serial(data, serial); }
static void desktop_repeat(void *data, struct wl_keyboard *proxy, int32_t rate, int32_t delay) {
	struct desktop_bridge *bridge = data;
	bridge->rate = rate;
	bridge->delay = delay;
	struct keyboard *keyboard;
	wl_list_for_each(keyboard, &bridge->client->keyboards, link)
		wlr_keyboard_set_repeat_info(keyboard->wlr_keyboard, rate, delay);
}
static const struct wl_keyboard_listener desktop_keyboard_listener = {
	.keymap = desktop_keymap, .enter = desktop_key_enter, .leave = desktop_key_leave,
	.key = desktop_key, .modifiers = desktop_modifiers, .repeat_info = desktop_repeat,
};

static void desktop_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	struct desktop_bridge *bridge = data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !bridge->keyboard) {
		bridge->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(bridge->keyboard, &desktop_keyboard_listener, bridge);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && bridge->keyboard) {
		wl_keyboard_release(bridge->keyboard);
		bridge->keyboard = NULL;
		bridge->focused = false;
		bindings_stop(bridge->client);
		apply_focus(bridge->client);
	}
}
static void desktop_seat_name(void *data, struct wl_seat *seat, const char *name) {}
static const struct wl_seat_listener desktop_seat_listener = {
	.capabilities = desktop_capabilities, .name = desktop_seat_name,
};

static void desktop_attention_done(void *data, struct xdg_activation_token_v1 *token, const char *value) {
	struct desktop_bridge *bridge = data;
	if (!bridge->focused) {
		struct output *output;
		wl_list_for_each(output, &bridge->client->outputs, link) {
			if (!wlr_output_is_wl(output->wlr_output)) continue;
			xdg_activation_v1_activate(bridge->activation, value, wlr_wl_output_get_surface(output->wlr_output));
			trace_event(bridge->client, "host_attention_requested", 0, 0, NULL);
			break;
		}
	}
	xdg_activation_token_v1_destroy(token);
	bridge->attention_token = NULL;
	wl_display_flush(bridge->display);
}
static const struct xdg_activation_token_v1_listener desktop_attention_listener = {
	.done = desktop_attention_done,
};

static void desktop_attention(struct frontend *client) {
	struct desktop_bridge *bridge = &client->desktop;
	if (bridge->focused || !bridge->activation || bridge->attention_token) return;
	bridge->attention_token = xdg_activation_v1_get_activation_token(bridge->activation);
	xdg_activation_token_v1_add_listener(bridge->attention_token, &desktop_attention_listener, bridge);
	xdg_activation_token_v1_set_app_id(bridge->attention_token, "amux");
	xdg_activation_token_v1_commit(bridge->attention_token);
	wl_display_flush(bridge->display);
}

static void desktop_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
	struct desktop_bridge *bridge = data;
	if (!strcmp(interface, wl_seat_interface.name) && version >= 5 && !bridge->seat) {
		bridge->seat_name = name;
		bridge->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7);
		wl_seat_add_listener(bridge->seat, &desktop_seat_listener, bridge);
	} else if (!strcmp(interface, wl_data_device_manager_interface.name) && version >= 3) {
		bridge->manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
	} else if (!strcmp(interface, xdg_activation_v1_interface.name)) {
		bridge->activation = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
	}
}
static void desktop_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	struct desktop_bridge *bridge = data;
	if (name == bridge->seat_name) {
		wlr_log(WLR_ERROR, "Outer desktop seat removed");
		bridge->client->failed = true;
		wl_display_terminate(bridge->client->wl_display);
	}
}
static const struct wl_registry_listener desktop_registry_listener = {
	.global = desktop_global, .global_remove = desktop_global_remove,
};

static void desktop_find_backend(struct wlr_backend *backend, void *data) {
	struct desktop_bridge *bridge = data;
	if (wlr_backend_is_wl(backend)) bridge->display = wlr_wl_backend_get_remote_display(backend);
}

#include "host-open.c"

static bool desktop_init(struct frontend *client) {
	struct desktop_bridge *bridge = &client->desktop;
	bridge->client = client;
	wl_list_init(&bridge->offers);
	snprintf(bridge->marker, sizeof(bridge->marker), "application/x-amux-clipboard-%ld", (long)getpid());
	if (wlr_backend_is_multi(client->backend)) wlr_multi_for_each_backend(client->backend, desktop_find_backend, bridge);
	else desktop_find_backend(client->backend, bridge);
	if (!bridge->display) return false;
	bridge->registry = wl_display_get_registry(bridge->display);
	wl_registry_add_listener(bridge->registry, &desktop_registry_listener, bridge);
	if (wl_display_roundtrip(bridge->display) < 0 || !bridge->seat || !bridge->manager) return false;
	bridge->device = wl_data_device_manager_get_data_device(bridge->manager, bridge->seat);
	wl_data_device_add_listener(bridge->device, &desktop_device_listener, bridge);
	return wl_display_roundtrip(bridge->display) >= 0 && wl_display_roundtrip(bridge->display) >= 0 && desktop_open_init(client);
}

static void desktop_finish(struct frontend *client) {
	struct desktop_bridge *bridge = &client->desktop;
	desktop_open_finish(bridge);
	if (bridge->attention_token) xdg_activation_token_v1_destroy(bridge->attention_token);
	if (bridge->activation) xdg_activation_v1_destroy(bridge->activation);
	desktop_clear_export(bridge);
	if (client->seat->selection_source && client->seat->selection_source->impl == &desktop_offer_impl)
		wlr_seat_set_selection(client->seat, NULL, wl_display_next_serial(client->wl_display));
	struct desktop_offer *offer, *next;
	wl_list_for_each_safe(offer, next, &bridge->offers, link) wlr_data_source_destroy(&offer->base);
	if (bridge->keyboard) wl_keyboard_release(bridge->keyboard);
	wl_data_device_release(bridge->device);
	wl_data_device_manager_destroy(bridge->manager);
	wl_seat_release(bridge->seat);
	wl_registry_destroy(bridge->registry);
	xkb_keymap_unref(bridge->keymap);
}
