static bool binding_key(struct binding *binding, const char *text) {
	char *copy = strdup(text), *key = copy, *end;
	while ((end = strchr(key, '+')) && end != key) {
		*end = '\0';
		if (!strcasecmp(key, "Ctrl")) binding->modifiers |= WLR_MODIFIER_CTRL;
		else if (!strcasecmp(key, "Alt")) binding->modifiers |= WLR_MODIFIER_ALT;
		else if (!strcasecmp(key, "Shift")) binding->modifiers |= WLR_MODIFIER_SHIFT;
		else if (!strcasecmp(key, "Super")) binding->modifiers |= WLR_MODIFIER_LOGO;
		else { free(copy); return false; }
		key = end + 1;
	}
	if (g_utf8_validate(key, -1, NULL) && g_utf8_strlen(key, -1) == 1)
		binding->sym = xkb_utf32_to_keysym(g_utf8_get_char(key));
	else binding->sym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
	free(copy);
	return binding->sym != XKB_KEY_NoSymbol;
}

static bool binding_matches(struct binding *binding, xkb_keysym_t sym, uint32_t modifiers) {
	uint32_t mask = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
	if (binding->modifiers & WLR_MODIFIER_SHIFT) mask |= WLR_MODIFIER_SHIFT;
	if ((modifiers & mask) != binding->modifiers) return false;
	if (binding->modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT))
		return xkb_keysym_to_lower(sym) == xkb_keysym_to_lower(binding->sym);
	return sym == binding->sym;
}

static void bindings_stop(struct frontend *client) {
	client->bindings.repeating = NULL;
	client->bindings.ui_repeat_sym = XKB_KEY_NoSymbol;
	if (client->bindings.timer) wl_event_source_timer_update(client->bindings.timer, 0);
}

static json_object *previous_window(struct frontend *client) {
	json_object *windows = client->ui.model.state ? field(client->ui.model.state, "windows") : NULL;
	size_t count = windows ? json_object_array_length(windows) : 0, active = count;
	for (size_t i = 0; i < count; i++)
		if (json_object_get_int64(field(json_object_array_get_idx(windows, i), "window")) == client->active_window) active = i;
	for (size_t h = 0; h < client->window_history_count; h++) {
		if (client->window_history[h].session != client->session || client->window_history[h].window == client->active_window) continue;
		for (size_t i = 0; i < count; i++) {
			json_object *window = json_object_array_get_idx(windows, i);
			if (json_object_get_int64(field(window, "window")) == client->window_history[h].window) return window;
		}
	}
	if (active == count || count < 2) return NULL;
	return json_object_array_get_idx(windows, active > 0 ? active - 1 : active + 1);
}

static int64_t previous_session(struct frontend *client, json_object *sessions) {
	size_t count = json_object_array_length(sessions);
	bool available[32] = {false};
	for (size_t h = 0; h < client->session_history_count; h++)
		for (size_t i = 0; i < count; i++) {
			json_object *session = json_object_array_get_idx(sessions, i);
			if (json_object_get_int64(field(session, "session")) == client->session_history[h]
					&& client->session_history[h] != client->session && !json_object_get_boolean(field(session, "attached")))
				available[h] = true;
		}
	for (size_t h = 0; h < client->session_history_count; h++) if (available[h]) return client->session_history[h];
	int64_t lower = 0, higher = 0;
	for (size_t i = 0; i < count; i++) {
		json_object *session = json_object_array_get_idx(sessions, i);
		int64_t id = json_object_get_int64(field(session, "session"));
		if (id == client->session || json_object_get_boolean(field(session, "attached"))) continue;
		if (id < client->session && id > lower) lower = id;
		if (id > client->session && (!higher || id < higher)) higher = id;
	}
	return lower ? lower : higher;
}

static enum ui_mode binding_ui(struct binding *binding) {
	json_object *ui = field(binding->command, "ui");
	static const char *names[] = {"", "windows", "sessions", "command", "session-name", "window-name", "pane-name", "help", "connections"};
	if (ui && !strcmp(json_object_get_string(ui), "arrange")) return UI_ARRANGE;
	for (size_t i = 1; ui && i < sizeof(names) / sizeof(*names); i++)
		if (!strcmp(json_object_get_string(ui), names[i])) return i;
	return UI_NONE;
}

static bool binding_action(struct frontend *client, struct binding *binding) {
	json_object *command = binding->command, *mode = field(command, "mode");
	if (field(command, "ui")) {
		enum ui_mode ui = binding_ui(binding);
		if (ui) ui_open(client, ui);
		return false;
	}
	if (mode) {
		client->prefix = false;
		client->resizing = !strcmp(json_object_get_string(mode), "resize");
		return false;
	}
	const char *op = json_object_get_string(field(command, "op"));
	if (!strcmp(op, "detach")) {
		client->quitting = true;
		wl_display_terminate(client->wl_display);
		return false;
	}
	if (!strcmp(op, "window_last")) {
		client->prefix = false;
		json_object *window = previous_window(client);
		if (window) {
			json_object *message = operation("window_select");
			json_object_object_add(message, "number", json_object_new_int64(json_object_get_int64(field(window, "number"))));
			enqueue(client, message);
		}
		return false;
	}
	if (!strcmp(op, "session_last")) {
		client->prefix = false;
		struct command *list = enqueue(client, operation("list"));
		if (list) list->session_last = true;
		return false;
	}
	json_object *message = NULL;
	json_object_deep_copy(command, &message, NULL);
	if (!strcmp(op, "split") && !client->active_window && !field(command, "pane")) {
		json_object_object_add(message, "op", json_object_new_string("window_new"));
		json_object_object_del(message, "axis");
	}
	enqueue(client, message);
	client->prefix = false;
	if (!strcmp(op, "resize")) client->resizing = true;
	return !strcmp(op, "focus") || !strcmp(op, "move") || !strcmp(op, "resize");
}

static int bindings_repeat(void *data) {
	struct frontend *client = data;
	struct bindings *bindings = &client->bindings;
	if (bindings->ui_repeat_sym != XKB_KEY_NoSymbol) {
		if (!client->desktop.focused || !client->ui.model.mode || client->ui.model.pending || client->desktop.rate <= 0) {
			bindings_stop(client);
			return 0;
		}
		ui_key(client, bindings->ui_repeat_sym, bindings->ui_repeat_modifiers);
		if (!client->ui.model.repeatable) { bindings_stop(client); return 0; }
		int interval = 1000 / client->desktop.rate;
		wl_event_source_timer_update(bindings->timer, interval > 0 ? interval : 1);
		return 0;
	}
	if (!bindings->repeating) return 0;
	if (!client->waiting && wl_list_empty(&client->requests)) {
		binding_action(client, bindings->repeating);
		ui_draw(client);
	}
	int interval = 1000 / bindings->rate;
	wl_event_source_timer_update(bindings->timer, interval > 0 ? interval : 1);
	return 0;
}

static bool handle_keybinding(struct frontend *client, xkb_keysym_t sym, uint32_t modifiers) {
	bindings_stop(client);
	if ((sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) ||
			(sym >= XKB_KEY_ISO_Lock && sym <= XKB_KEY_ISO_Last_Group_Lock) ||
			sym == XKB_KEY_Num_Lock || sym == XKB_KEY_Scroll_Lock || sym == XKB_KEY_Mode_switch)
		return false;
	if (!client->prefix && !client->resizing && manager_session_empty(&client->ui.model) &&
			client->ui.model.mode == UI_NONE && sym == XKB_KEY_q &&
			!(modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT))) {
		json_object *message = operation("destroy");
		json_object_object_add(message, "session", json_object_new_int64(client->session));
		json_object_object_add(message, "if_empty", json_object_new_boolean(true));
		json_object_object_add(message, "select_next", json_object_new_boolean(true));
		ui_close(client);
		enqueue(client, message);
		return true;
	}
	for (size_t i = 0; (client->ui.model.mode == UI_SESSIONS || client->ui.model.mode == UI_ARRANGE ||
			client->ui.model.mode == UI_ARRANGE_NAME || client->ui.model.mode == UI_ARRANGE_REVIEW) && i < client->bindings.count[0]; i++) {
		struct binding *binding = &client->bindings.items[0][i];
		if (binding_matches(binding, sym, modifiers) && binding_ui(binding) == UI_SESSIONS) {
			if (client->ui.model.mode == UI_ARRANGE_NAME || client->ui.model.mode == UI_ARRANGE_REVIEW) client->ui.model.mode = UI_ARRANGE;
			ui_close(client);
			return true;
		}
	}
	if (ui_key(client, sym, modifiers)) {
		if (client->ui.model.repeatable && client->desktop.rate > 0) {
			client->bindings.ui_repeat_sym = sym;
			client->bindings.ui_repeat_modifiers = modifiers;
			wl_event_source_timer_update(client->bindings.timer, client->desktop.delay > 0 ? client->desktop.delay : 1);
		}
		return true;
	}
	if (binding_matches(&client->bindings.prefix, sym, modifiers)) {
		if (client->prefix) { client->prefix = false; return false; }
		client->prefix = true;
		client->prefix_ns = client->input_ns;
		trace_event(client, "prefix_received", 0, 0, NULL);
		return true;
	}
	int mode = client->resizing ? 2 : client->prefix ? 1 : 0;
	struct bindings *bindings = &client->bindings;
	for (size_t i = 0; i < bindings->count[mode]; i++) {
		struct binding *binding = &bindings->items[mode][i];
		if (!binding_matches(binding, sym, modifiers)) continue;
		if (binding_action(client, binding) && bindings->rate) {
			bindings->repeating = binding;
			wl_event_source_timer_update(bindings->timer, bindings->delay > 0 ? bindings->delay : 1);
		}
		return true;
	}
	if (mode == 1) client->prefix = false;
	return mode != 0;
}

static bool bindings_init(struct frontend *client) {
	struct bindings *bindings = &client->bindings;
	const char *text = getenv("AMUX_MANAGER_CONFIG");
	bindings->config = text ? json_tokener_parse(text) : NULL;
	if (!bindings->config) return false;
	const char *prefix = json_object_get_string(field(bindings->config, "prefix"));
	if (!prefix || !binding_key(&bindings->prefix, prefix)) return false;
	bindings->rate = json_object_get_int(field(bindings->config, "repeat_rate"));
	bindings->delay = json_object_get_int(field(bindings->config, "repeat_delay"));
	json_object *tables = field(bindings->config, "bindings");
	const char *names[] = {"normal", "prefix", "resize"};
	for (int mode = 0; mode < 3; mode++) {
		json_object *table = field(tables, names[mode]);
		if (!table) return false;
		bindings->count[mode] = json_object_object_length(table);
		bindings->items[mode] = calloc(bindings->count[mode], sizeof(struct binding));
		size_t index = 0;
		json_object_object_foreach(table, key, command) {
			struct binding *binding = &bindings->items[mode][index++];
			binding->command = command;
			if (!binding_key(binding, key)) { fprintf(stderr, "Invalid manager binding: %s\n", key); return false; }
		}
	}
	bindings->timer = wl_event_loop_add_timer(wl_display_get_event_loop(client->wl_display), bindings_repeat, client);
	return bindings->timer != NULL;
}

static void bindings_finish(struct frontend *client) {
	bindings_stop(client);
	if (client->bindings.timer) wl_event_source_remove(client->bindings.timer);
	client->bindings.timer = NULL;
	for (int mode = 0; mode < 3; mode++) free(client->bindings.items[mode]);
	json_object_put(client->bindings.config);
}
