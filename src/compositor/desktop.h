#include <wayland-client.h>
#include "xdg-activation-v1-client-protocol.h"

struct desktop_bridge {
	struct frontend *client;
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct xdg_activation_v1 *activation;
	struct xdg_activation_token_v1 *attention_token;
	struct wl_data_device_manager *manager;
	struct wl_data_device *device;
	struct wl_data_source *outgoing;
	struct wlr_data_source *exported;
	struct wl_listener source_destroy;
	struct wl_list offers;
	struct xkb_keymap *keymap;
	uint32_t serial, seat_name;
	int rate, delay;
	bool focused;
	char marker[80];
};

static bool desktop_init(struct frontend *client);
static void desktop_finish(struct frontend *client);
static void desktop_export(struct frontend *client, struct wlr_data_source *source);
static void desktop_attention(struct frontend *client);
