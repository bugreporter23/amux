static void arrangement_clear(struct manager_state *ui) {
	json_object_put(ui->arrangement_request); ui->arrangement_request = NULL;
	json_object_put(ui->arrangement_changes); ui->arrangement_changes = NULL;
	json_object_put(ui->arrangement_review_rows); ui->arrangement_review_rows = NULL;
	json_object_put(ui->arrangement_base); ui->arrangement_base = NULL;
	json_object_put(ui->arrangement_rows); ui->arrangement_rows = NULL;
	json_object_put(ui->cut_window); ui->cut_window = NULL;
	json_object_put(ui->arrangement_undo); ui->arrangement_undo = NULL;
	json_object_put(ui->cut_undo); ui->cut_undo = NULL;
	json_object_put(ui->insertion_base); ui->insertion_base = NULL;
	ui->arrangement_inserting = false;
	ui->row_cursor = 0;
}

static void arrangement_load(struct manager_state *ui, json_object *sessions) {
	arrangement_clear(ui);
	json_object_deep_copy(sessions, &ui->arrangement_base, NULL);
	ui->arrangement_rows = json_object_new_array();
	for (size_t s = 0; s < json_object_array_length(sessions); s++) {
		json_object *session = json_object_array_get_idx(sessions, s);
		json_object *header = json_object_new_object();
		json_object_object_add(header, "session", json_object_get(field(session, "session")));
		json_object_object_add(header, "name", json_object_get(field(session, "name")));
		json_object_array_add(ui->arrangement_rows, header);
		json_object *windows = field(session, "windows");
		for (size_t w = 0; w < json_object_array_length(windows); w++) {
			json_object *row = NULL;
			json_object_deep_copy(json_object_array_get_idx(windows, w), &row, NULL);
			json_object_array_add(ui->arrangement_rows, row);
		}
	}
	ui->query[0] = '\0'; ui->cursor = 0; ui->edit_operator = 0;
	ui->next_draft_id = -1;
}

static void arrangement_save_undo(struct manager_state *ui) {
	json_object_put(ui->arrangement_undo); ui->arrangement_undo = NULL;
	json_object_deep_copy(ui->arrangement_rows, &ui->arrangement_undo, NULL);
	json_object_put(ui->cut_undo); ui->cut_undo = json_object_get(ui->cut_window);
}

static json_object *arrangement_draft(const struct manager_state *ui) {
	json_object *sessions = json_object_new_array(), *windows = NULL;
	for (size_t i = 0; i < json_object_array_length(ui->arrangement_rows); i++) {
		json_object *row = json_object_array_get_idx(ui->arrangement_rows, i);
		if (field(row, "session")) {
			json_object *session = NULL;
			json_object_deep_copy(row, &session, NULL);
			windows = json_object_new_array();
			json_object_object_add(session, "windows", windows);
			json_object_array_add(sessions, session);
		} else if (windows) json_object_array_add(windows, json_object_get(row));
	}
	return sessions;
}

bool manager_arrangement_dirty(const struct manager_state *ui) {
	if (!ui->arrangement_base) return false;
	json_object *draft = arrangement_draft(ui);
	bool dirty = !json_object_equal(draft, ui->arrangement_base);
	json_object_put(draft);
	return dirty;
}

static json_object *arrangement_apply(struct manager_state *ui, const struct manager_context *context) {
	if (!ui->arrangement_base) {
		snprintf(ui->error, sizeof(ui->error), "Reload with Ctrl-r before applying"); return NULL;
	}
	for (size_t i = 0; i < json_object_array_length(ui->arrangement_rows); i++) {
		if (!*json_object_get_string(field(json_object_array_get_idx(ui->arrangement_rows, i), "name"))) {
			snprintf(ui->error, sizeof(ui->error), "Name every row before applying"); return NULL;
		}
	}
	json_object *request = operation("arrangement_apply");
	json_object_object_add(request, "base", json_object_get(ui->arrangement_base));
	json_object_object_add(request, "arrangement", arrangement_draft(ui));
	json_object_object_add(request, "width", json_object_new_int(context->width));
	json_object_object_add(request, "height", json_object_new_int(context->height));
	return request;
}

static json_object *arrangement_find(json_object *items, const char *key, int64_t identity) {
	for (size_t i = 0; i < json_object_array_length(items); i++) {
		json_object *item = json_object_array_get_idx(items, i);
		if (json_object_get_int64(field(item, key)) == identity) return item;
	}
	return NULL;
}

static json_object *arrangement_owner(json_object *sessions, int64_t window) {
	for (size_t i = 0; i < json_object_array_length(sessions); i++) {
		json_object *session = json_object_array_get_idx(sessions, i);
		if (arrangement_find(field(session, "windows"), "window", window)) return session;
	}
	return NULL;
}

static json_object *arrangement_change(struct manager_state *ui, const char *kind,
	const char *entity, int64_t identity, const char *name) {
	json_object *row = json_object_new_object();
	json_object_object_add(row, "kind", json_object_new_string(kind));
	json_object_object_add(row, "entity", json_object_new_string(entity));
	json_object_object_add(row, "id", json_object_new_int64(identity));
	json_object_object_add(row, "name", json_object_new_string(name));
	json_object_array_add(ui->arrangement_changes, row);
	return row;
}

static json_object *arrangement_location(json_object *session, size_t position) {
	json_object *location = json_object_new_object();
	json_object_object_add(location, "id", json_object_get(field(session, "session")));
	json_object_object_add(location, "name", json_object_get(field(session, "name")));
	json_object_object_add(location, "position", json_object_new_int64(position + 1));
	return location;
}

static size_t arrangement_position(json_object *items, const char *key, int64_t identity) {
	for (size_t i = 0; i < json_object_array_length(items); i++)
		if (json_object_get_int64(field(json_object_array_get_idx(items, i), key)) == identity) return i;
	return 0;
}

static bool arrangement_reordered(json_object *before, json_object *after, const char *key) {
	size_t previous = 0;
	bool seen = false;
	for (size_t i = 0; i < json_object_array_length(after); i++) {
		int64_t identity = json_object_get_int64(field(json_object_array_get_idx(after, i), key));
		for (size_t j = 0; j < json_object_array_length(before); j++) {
			if (json_object_get_int64(field(json_object_array_get_idx(before, j), key)) != identity) continue;
			if (seen && j < previous) return true;
			previous = j; seen = true; break;
		}
	}
	return false;
}

#include "arrangement-review.c"

static void arrangement_review(struct manager_state *ui, const struct manager_context *context) {
	if (!context->connected) {
		snprintf(ui->error, sizeof(ui->error), "Connect to a server before editing"); return;
	}
	json_object *request = arrangement_apply(ui, context);
	if (!request) return;
	if (!manager_arrangement_dirty(ui)) {
		json_object_put(request);
		snprintf(ui->error, sizeof(ui->error), "No staged changes"); return;
	}
	ui->arrangement_request = request;
	ui->arrangement_changes = json_object_new_array();
	json_object *base = field(request, "base"), *draft = field(request, "arrangement");
	for (size_t s = 0; s < json_object_array_length(draft); s++) {
		json_object *session = json_object_array_get_idx(draft, s), *windows = field(session, "windows");
		int64_t identity = json_object_get_int64(field(session, "session"));
		const char *name = json_object_get_string(field(session, "name"));
		json_object *old = arrangement_find(base, "session", identity);
		if (!old) {
			json_object *change = arrangement_change(ui, "add", "session", identity, name);
			json_object_object_add(change, "position", json_object_new_int64(s + 1));
			json_object_object_add(change, "initial_window", json_object_new_boolean(!json_object_array_length(windows)));
		} else if (strcmp(name, json_object_get_string(field(old, "name")))) {
			json_object *change = arrangement_change(ui, "rename", "session", identity, name);
			json_object_object_add(change, "before", json_object_get(field(old, "name")));
		}
		for (size_t w = 0; w < json_object_array_length(windows); w++) {
			json_object *window = json_object_array_get_idx(windows, w);
			int64_t wid = json_object_get_int64(field(window, "window"));
			const char *wname = json_object_get_string(field(window, "name"));
			json_object *owner = arrangement_owner(base, wid);
			if (!owner) {
				json_object *change = arrangement_change(ui, "add", "window", wid, wname);
				json_object_object_add(change, "to", arrangement_location(session, w));
				continue;
			}
			json_object *original = arrangement_find(field(owner, "windows"), "window", wid);
			if (strcmp(wname, json_object_get_string(field(original, "name")))) {
				json_object *change = arrangement_change(ui, "rename", "window", wid, wname);
				json_object_object_add(change, "before", json_object_get(field(original, "name")));
			}
			if (json_object_get_int64(field(owner, "session")) != identity) {
				json_object *change = arrangement_change(ui, "move", "window", wid, wname);
				json_object_object_add(change, "from", arrangement_location(owner, arrangement_position(field(owner, "windows"), "window", wid)));
				json_object_object_add(change, "to", arrangement_location(session, w));
			}
		}
		if (old && arrangement_reordered(field(old, "windows"), windows, "window")) {
			json_object *change = arrangement_change(ui, "order", "windows", identity, name);
			json_object_object_add(change, "before_order", json_object_get(field(old, "windows")));
			json_object_object_add(change, "after_order", json_object_get(windows));
		}
	}
	for (size_t s = 0; s < json_object_array_length(base); s++) {
		json_object *session = json_object_array_get_idx(base, s), *windows = field(session, "windows");
		for (size_t w = 0; w < json_object_array_length(windows); w++) {
			json_object *window = json_object_array_get_idx(windows, w);
			int64_t identity = json_object_get_int64(field(window, "window"));
			if (!arrangement_owner(draft, identity))
				arrangement_change(ui, "remove", "window", identity, json_object_get_string(field(window, "name")));
		}
	}
	if (arrangement_reordered(base, draft, "session")) {
		json_object *change = arrangement_change(ui, "order", "sessions", 0, "Sessions");
		json_object_object_add(change, "before_order", json_object_get(base));
		json_object_object_add(change, "after_order", json_object_get(draft));
	}
	ui->arrangement_review_rows = arrangement_review_tree(base, draft);
	ui->review_selection = ui->selected; ui->review_first = ui->first;
	if (ui->cursor && ui->cursor == strlen(ui->query)) ui->cursor = ui_previous(ui, ui->cursor);
	ui->selected = ui->first = 0; ui->mode = UI_ARRANGE_REVIEW; ui->normal = true;
	ui->edit_operator = 0; ui->error[0] = '\0';
}

static void arrangement_review_cancel(struct manager_state *ui) {
	json_object_put(ui->arrangement_request); ui->arrangement_request = NULL;
	json_object_put(ui->arrangement_changes); ui->arrangement_changes = NULL;
	json_object_put(ui->arrangement_review_rows); ui->arrangement_review_rows = NULL;
	ui->mode = UI_ARRANGE; ui->selected = ui->review_selection; ui->first = ui->review_first;
	ui_selection(ui);
}

static json_object *arrangement_review_confirm(struct manager_state *ui, const struct manager_context *context) {
	if (!context->connected) {
		snprintf(ui->error, sizeof(ui->error), "Connect to a server before applying"); return NULL;
	}
	json_object *request = ui->arrangement_request;
	ui->arrangement_request = NULL;
	arrangement_review_cancel(ui);
	return request;
}

static void arrangement_unfilter(struct manager_state *ui) {
	int selected = arrangement_index(ui, ui->selected);
	ui->query[0] = '\0'; ui->cursor = 0;
	ui->selected = selected < 0 ? 0 : selected;
	ui_selection(ui);
}

static void arrangement_insert(struct manager_state *ui, const struct manager_context *context, bool above) {
	arrangement_unfilter(ui);
	ui->insertion_selection = ui->selected;
	int at = json_object_array_length(ui->arrangement_rows) ? ui->selected + (above ? 0 : 1) : 0;
	bool session = at == 0 || (above && field(manager_item(ui, ui->selected), "session"));
	json_object_deep_copy(ui->arrangement_rows, &ui->insertion_base, NULL);
	json_object *row = json_object_new_object(), *rows = json_object_new_array();
	json_object_object_add(row, session ? "session" : "window", json_object_new_int64(ui->next_draft_id--));
	json_object_object_add(row, "name", json_object_new_string(""));
	for (int i = 0; i <= (int)json_object_array_length(ui->arrangement_rows); i++) {
		if (i == at) json_object_array_add(rows, row);
		if (i < (int)json_object_array_length(ui->arrangement_rows))
			json_object_array_add(rows, json_object_get(json_object_array_get_idx(ui->arrangement_rows, i)));
	}
	json_object_put(ui->arrangement_rows); ui->arrangement_rows = rows;
	manager_open(ui, UI_ARRANGE_NAME, context);
	ui->selected = at;
	ui->arrangement_inserting = true;
	ui_selection(ui);
}

static void arrangement_indent(struct manager_state *ui, bool indent) {
	json_object *row = manager_item(ui, ui->selected);
	const char *from = indent ? "session" : "window", *to = indent ? "window" : "session";
	if (!field(row, from)) return;
	if (indent && !ui->selected) {
		snprintf(ui->error, sizeof(ui->error), "A window requires a session header above it"); return;
	}
	json_object *identity = json_object_get(field(row, from));
	json_object_object_del(row, from);
	json_object_object_add(row, to, identity);
	ui->error[0] = '\0';
}

static void arrangement_edit(struct manager_state *ui, const struct manager_context *context, xkb_keysym_t sym) {
	arrangement_unfilter(ui);
	json_object *row = manager_item(ui, ui->selected);
	const char *name = json_object_get_string(field(row, "name"));
	if (strlen(name) >= sizeof(ui->query)) {
		snprintf(ui->error, sizeof(ui->error), "Name is too long for inline editing"); return;
	}
	int selected = ui->selected, first = ui->first;
	char op = ui->edit_operator;
	size_t cursor = ui->row_cursor;
	manager_open(ui, UI_ARRANGE_NAME, context);
	ui->selected = selected; ui->first = first;
	snprintf(ui->query, sizeof(ui->query), "%s", name);
	const char *end;
	if (!g_utf8_validate(ui->query, -1, &end)) ui->query[end - ui->query] = '\0';
	ui->cursor = cursor < strlen(ui->query) ? cursor : strlen(ui->query);
	while (ui->cursor && !g_utf8_validate(ui->query, ui->cursor, NULL)) ui->cursor--;
	ui->normal = true; ui->edit_operator = op;
	json_object *request = NULL;
	manager_key(ui, context, sym, 0, &request);
	json_object_put(request);
	if (ui->mode == UI_ARRANGE_NAME && ui->normal) arrangement_finish_name(ui, context, true);
}

static json_object *arrangement_choose(struct manager_state *ui, const struct manager_context *context) {
	if (manager_arrangement_dirty(ui)) {
		snprintf(ui->error, sizeof(ui->error), "Review with = or Ctrl-s before switching"); return NULL;
	}
	json_object *row = manager_item(ui, ui->selected);
	json_object *session = manager_arrangement_session(ui, ui->selected);
	if (!row || !session) return NULL;
	ui->error[0] = '\0';
	int64_t owner = json_object_get_int64(field(session, "session"));
	ui->opening_window = json_object_get_int64(field(row, "window"));
	if (context->attached && owner == json_object_get_int64(field(ui->state, "session"))) {
		if (ui->opening_window) return manager_followup(ui, context);
		manager_close(ui, context); return NULL;
	}
	json_object *request = operation(context->attached ? "session_select" : "attach");
	json_object_object_add(request, "session", json_object_new_int64(owner));
	if (!context->attached) {
		json_object_object_add(request, "width", json_object_new_int(context->width));
		json_object_object_add(request, "height", json_object_new_int(context->height));
		json_object_object_add(request, "events", json_object_new_boolean(true));
	}
	return request;
}

static bool arrangement_key(struct manager_state *ui, const struct manager_context *context,
		xkb_keysym_t sym, unsigned modifiers, json_object **request) {
	if (sym == XKB_KEY_equal && !(modifiers & (UI_CONTROL | UI_ALT | UI_SUPER)) && !ui->edit_operator) {
		arrangement_review(ui, context); return true;
	}
	if (modifiers & (UI_CONTROL | UI_ALT | UI_SUPER)) return true;
	json_object *row = manager_item(ui, ui->selected);
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { *request = arrangement_choose(ui, context); return true; }
	if (sym == XKB_KEY_slash) {
		ui->normal = false; ui->query[0] = '\0'; ui->cursor = 0; ui->selected = ui->first = 0;
		ui->edit_operator = 0; return true;
	}
	bool text_key = sym == XKB_KEY_i || sym == XKB_KEY_a || sym == XKB_KEY_I || sym == XKB_KEY_A ||
		sym == XKB_KEY_h || sym == XKB_KEY_l || sym == XKB_KEY_Left || sym == XKB_KEY_Right ||
		sym == XKB_KEY_0 || sym == XKB_KEY_Home || sym == XKB_KEY_dollar || sym == XKB_KEY_End ||
		sym == XKB_KEY_w || sym == XKB_KEY_b || sym == XKB_KEY_e || sym == XKB_KEY_x || sym == XKB_KEY_X ||
		sym == XKB_KEY_D || sym == XKB_KEY_C || sym == XKB_KEY_s;
	bool editing = text_key || sym == XKB_KEY_c || sym == XKB_KEY_d || sym == XKB_KEY_m || sym == XKB_KEY_p || sym == XKB_KEY_P || sym == XKB_KEY_u || sym == XKB_KEY_o || sym == XKB_KEY_O;
	if (editing && !context->connected) {
		snprintf(ui->error, sizeof(ui->error), "Connect to a server before editing"); return true;
	}
	if (editing && !ui->arrangement_rows) {
		snprintf(ui->error, sizeof(ui->error), "Reload with Ctrl-r before editing"); return true;
	}
	if (editing && row) arrangement_unfilter(ui);
	if ((sym == XKB_KEY_o || sym == XKB_KEY_O) && (row || !json_object_array_length(ui->arrangement_rows))) {
		arrangement_insert(ui, context, sym == XKB_KEY_O); return true;
	}
	if (row && ((ui->edit_operator && !(ui->edit_operator == 'd' && sym == XKB_KEY_d)) || text_key)) {
		arrangement_edit(ui, context, sym); return true;
	}
	if (row && ((sym == XKB_KEY_d && ui->edit_operator == 'd') || sym == XKB_KEY_m)) {
		if (!field(row, "window")) snprintf(ui->error, sizeof(ui->error), "Session headers stay in the buffer");
		else if (sym == XKB_KEY_m && ui->cut_window) snprintf(ui->error, sizeof(ui->error), "Place the held window with p/P first");
		else {
			arrangement_save_undo(ui);
			if (sym == XKB_KEY_m) ui->cut_window = json_object_get(row);
			json_object_array_del_idx(ui->arrangement_rows, ui->selected, 1);
			if (!manager_item(ui, ui->selected) && ui->selected) ui->selected--;
			ui->error[0] = '\0';
		}
		ui->edit_operator = 0;
	} else if (sym == XKB_KEY_d) ui->edit_operator = 'd';
	else if (sym == XKB_KEY_c) ui->edit_operator = 'c';
	else {
		ui->edit_operator = 0;
		if (sym == XKB_KEY_y || sym == XKB_KEY_Y) snprintf(ui->error, sizeof(ui->error), "Copying windows is not supported; use m and p/P");
		else if ((sym == XKB_KEY_p || sym == XKB_KEY_P) && ui->cut_window && row) {
			arrangement_save_undo(ui);
			size_t at = ui->selected + (field(row, "session") || sym == XKB_KEY_p ? 1 : 0);
			json_object *rows = json_object_new_array();
			for (size_t i = 0; i <= json_object_array_length(ui->arrangement_rows); i++) {
				if (i == at) json_object_array_add(rows, json_object_get(ui->cut_window));
				if (i < json_object_array_length(ui->arrangement_rows))
					json_object_array_add(rows, json_object_get(json_object_array_get_idx(ui->arrangement_rows, i)));
			}
			json_object_put(ui->arrangement_rows); ui->arrangement_rows = rows;
			json_object_put(ui->cut_window); ui->cut_window = NULL;
			ui->selected = at; ui->error[0] = '\0';
		} else if (sym == XKB_KEY_u && ui->arrangement_undo) {
			json_object_put(ui->arrangement_rows); ui->arrangement_rows = ui->arrangement_undo; ui->arrangement_undo = NULL;
			json_object_put(ui->cut_window); ui->cut_window = ui->cut_undo; ui->cut_undo = NULL;
			ui->error[0] = '\0';
		} else if (sym == XKB_KEY_j || sym == XKB_KEY_Down) {
			if (manager_item(ui, ui->selected + 1)) ui->selected++;
			ui->repeatable = true;
		} else if (sym == XKB_KEY_k || sym == XKB_KEY_Up) {
			if (ui->selected) ui->selected--;
			ui->repeatable = true;
		} else if (sym == XKB_KEY_G) {
			ui->selected = 0;
			while (manager_item(ui, ui->selected + 1)) ui->selected++;
		}
	}
	ui_selection(ui);
	return true;
}
