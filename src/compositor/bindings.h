struct binding {
	xkb_keysym_t sym;
	uint32_t modifiers;
	json_object *command;
};

struct bindings {
	json_object *config;
	struct binding prefix;
	struct binding *items[3];
	size_t count[3];
	int rate, delay;
	struct wl_event_source *timer;
	struct binding *repeating;
	xkb_keysym_t ui_repeat_sym;
	uint32_t ui_repeat_modifiers;
	uint32_t repeat_code;
};

static bool bindings_init(struct frontend *client);
static void bindings_finish(struct frontend *client);
static void bindings_stop(struct frontend *client);
static bool handle_keybinding(struct frontend *client, xkb_keysym_t sym, uint32_t modifiers);
