static size_t connection_count(json_object *rows) { return rows ? json_object_array_length(rows) : 0; }

static const char *connection_fields[] = {"address", "name", "identity"};

static const char *connection_identity(json_object *row) {
	const char *path = json_object_get_string(field(row, "identity"));
	return path ? path : "";
}

static bool connection_saved(json_object *row) {
	const char *source = json_object_get_string(field(row, "source"));
	return source && (!strcmp(source, "saved") || !strcmp(source, "draft"));
}

static void connection_clear(struct manager_state *ui) {
	json_object_put(ui->connection_base); ui->connection_base = NULL;
	json_object_put(ui->connection_rows); ui->connection_rows = NULL;
	json_object_put(ui->connection_matches); ui->connection_matches = NULL;
	json_object_put(ui->connection_undo); ui->connection_undo = NULL;
	json_object_put(ui->connection_edit_base); ui->connection_edit_base = NULL;
	json_object_put(ui->connection_request); ui->connection_request = NULL;
	json_object_put(ui->connection_review); ui->connection_review = NULL;
	json_object_put(ui->connection_key_target); ui->connection_key_target = NULL;
	json_object_put(ui->key_matches); ui->key_matches = NULL;
	ui->connection_inserting = false; ui->next_connection_id = -1;
	ui->connection_field = 0;
}

static void connection_load(struct manager_state *ui, json_object *rows, json_object *base) {
	connection_clear(ui);
	json_object_deep_copy(rows, &ui->connection_rows, NULL);
	if (base) json_object_deep_copy(base, &ui->connection_base, NULL);
	else ui->connection_base = json_object_new_array();

}

static json_object *connection_draft(const struct manager_state *ui) {
	json_object *draft = json_object_new_array();
	const char *keys[] = {"connection", "name", "kind", "address"};
	for (size_t i = 0; ui->connection_rows && i < connection_count(ui->connection_rows); i++) {
		json_object *row = json_object_array_get_idx(ui->connection_rows, i);
		if (!connection_saved(row)) continue;
		json_object *entry = json_object_new_object();
		for (size_t k = 0; k < 4; k++) json_object_object_add(entry, keys[k], json_object_get(field(row, keys[k])));
		if (*connection_identity(row)) json_object_object_add(entry, "identity", json_object_get(field(row, "identity")));
		json_object_array_add(draft, entry);
	}
	return draft;
}

bool manager_connection_dirty(const struct manager_state *ui) {
	if (!ui->connection_base) return false;
	json_object *draft = connection_draft(ui);
	bool dirty = !json_object_equal(draft, ui->connection_base);
	json_object_put(draft); return dirty;
}

static void connection_matches(struct manager_state *ui) {
	GArray *matches = g_array_new(false, false, sizeof(struct ssh_match));
	for (size_t i = 0; ui->connection_rows && i < connection_count(ui->connection_rows); i++) {
		json_object *row = json_object_array_get_idx(ui->connection_rows, i);
		char *label = g_strdup_printf("%s %s %s", json_object_get_string(field(row, "name")),
			json_object_get_string(field(row, "address")), connection_identity(row));
		int score = fuzzy_score(label, ui->query); g_free(label);
		if (score != INT_MIN) {
			struct ssh_match match = {row, score, i}; g_array_append_val(matches, match);
		}
	}
	g_array_sort(matches, ssh_compare);
	json_object_put(ui->connection_matches); ui->connection_matches = json_object_new_array();
	for (size_t i = 0; i < matches->len; i++)
		json_object_array_add(ui->connection_matches, json_object_get(g_array_index(matches, struct ssh_match, i).item));
	g_array_free(matches, true);
	if (*ui->query) {
		bool socket = ui->query[0] == '/' || g_str_has_prefix(ui->query, "~/") || g_str_has_prefix(ui->query, "unix://");
		json_object *row = json_object_new_object();
		char *name = g_strdup_printf("Connect to %s", ui->query);
		json_object_object_add(row, "name", json_object_new_string(name)); g_free(name);
		json_object_object_add(row, "kind", json_object_new_string(socket ? "socket" : "ssh"));
		json_object_object_add(row, "address", json_object_new_string(ui->query));
		json_object_object_add(row, "source", json_object_new_string("typed"));
		json_object_array_add(ui->connection_matches, row);
	}
}

static void connection_unfilter(struct manager_state *ui) {
	json_object *row = manager_item(ui, ui->selected);
	ui->selected = MAX(0, (int)connection_count(ui->connection_rows) - 1);
	for (size_t i = 0; ui->connection_rows && i < connection_count(ui->connection_rows); i++) {
		if (json_object_array_get_idx(ui->connection_rows, i) == row) { ui->selected = i; break; }
	}
	ui->query[0] = '\0'; ui->cursor = 0; ui->first = 0;
	connection_matches(ui);
}

static void connection_undo_save(struct manager_state *ui) {
	json_object_put(ui->connection_undo); ui->connection_undo = NULL;
	json_object_deep_copy(ui->connection_rows, &ui->connection_undo, NULL);
}

static void key_matches(struct manager_state *ui) {
	GArray *matches = g_array_new(false, false, sizeof(struct ssh_match));
	for (size_t i = 0; ui->ssh_keys && i < connection_count(ui->ssh_keys); i++) {
		json_object *row = json_object_array_get_idx(ui->ssh_keys, i);
		int score = fuzzy_score(connection_identity(row), ui->query);
		if (score != INT_MIN) {
			struct ssh_match match = {row, score, i}; g_array_append_val(matches, match);
		}
	}
	g_array_sort(matches, ssh_compare);
	json_object_put(ui->key_matches); ui->key_matches = json_object_new_array();
	if (!*ui->query) json_object_array_add(ui->key_matches,
		json_tokener_parse("{\"name\":\"Use SSH defaults\",\"identity\":\"\"}"));
	for (size_t i = 0; i < matches->len; i++)
		json_object_array_add(ui->key_matches, json_object_get(g_array_index(matches, struct ssh_match, i).item));
	g_array_free(matches, true);
	const char *current = connection_identity(ui->connection_key_target);
	bool current_seen = false;
	for (size_t i = 0; i < connection_count(ui->key_matches); i++)
		current_seen |= !strcmp(current, connection_identity(json_object_array_get_idx(ui->key_matches, i)));
	if (*current && !current_seen && fuzzy_score(current, ui->query) != INT_MIN) {
		json_object *row = json_object_new_object();
		json_object_object_add(row, "name", json_object_new_string("Current key"));
		json_object_object_add(row, "identity", json_object_new_string(current));
		json_object_array_add(ui->key_matches, row);
	}
	if (ui->query[0] == '/') {
		json_object *row = json_object_new_object();
		json_object_object_add(row, "name", json_object_new_string("Use absolute path"));
		json_object_object_add(row, "identity", json_object_new_string(ui->query));
		json_object_array_add(ui->key_matches, row);
	}
}

static void connection_key_cancel(struct manager_state *ui) {
	json_object_put(ui->connection_key_target); ui->connection_key_target = NULL;
	json_object_put(ui->key_matches); ui->key_matches = NULL;
	ui->mode = UI_CONNECTIONS; ui->normal = true; ui->edit_operator = 0;
	ui->selected = ui->connection_key_selection; ui->first = 0;
	ui->query[0] = '\0'; ui->cursor = 0; connection_matches(ui);
}

static void connection_key_open(struct manager_state *ui) {
	json_object *row = manager_item(ui, ui->selected);
	if (!row || strcmp(json_object_get_string(field(row, "kind")), "ssh")) {
		snprintf(ui->error, sizeof(ui->error), "Select an SSH connection to choose its key"); return;
	}
	if (!ui->connection_base) { snprintf(ui->error, sizeof(ui->error), "Reload with Ctrl-r before editing"); return; }
	ui->connection_key_target = json_object_get(row);
	connection_unfilter(ui); ui->connection_key_selection = ui->selected;
	ui->mode = UI_SSH_KEY; ui->normal = false; ui->edit_operator = 0;
	ui->query[0] = '\0'; ui->cursor = 0; ui->selected = ui->first = 0;
	if (ui->compose) xkb_compose_state_reset(ui->compose);
	key_matches(ui);
	for (size_t i = 0; i < connection_count(ui->key_matches); i++)
		if (!strcmp(connection_identity(row), connection_identity(json_object_array_get_idx(ui->key_matches, i)))) { ui->selected = i; break; }
}

static void connection_key_accept(struct manager_state *ui, bool typed) {
	json_object *choice = typed ? NULL : manager_item(ui, ui->selected);
	const char *path = choice ? connection_identity(choice) : ui->query;
	if ((!choice && !*path) || (*path && (!g_path_is_absolute(path) || strlen(path) >= sizeof(ui->query)))) {
		snprintf(ui->error, sizeof(ui->error), "Choose a key or enter an absolute key path"); return;
	}
	json_object *target = ui->connection_key_target;
	if (!target) { connection_key_cancel(ui); return; }
	if (!strcmp(path, connection_identity(target))) { connection_key_cancel(ui); return; }
	connection_undo_save(ui);
	if (!connection_saved(target)) {
		json_object *copy = NULL; json_object_deep_copy(target, &copy, NULL);
		json_object_object_add(copy, "connection", json_object_new_int64(ui->next_connection_id--));
		json_object_object_add(copy, "source", json_object_new_string("draft"));
		bool found = false;
		for (size_t i = 0; i < connection_count(ui->connection_rows); i++)
			if (json_object_array_get_idx(ui->connection_rows, i) == target) {
				json_object_array_put_idx(ui->connection_rows, i, copy); ui->connection_key_selection = i; found = true; break;
			}
		if (!found) { ui->connection_key_selection = connection_count(ui->connection_rows); json_object_array_add(ui->connection_rows, copy); }
		target = copy;
		if (!strcmp(json_object_get_string(field(target, "source")), "draft") &&
			g_str_has_prefix(json_object_get_string(field(target, "name")), "Connect to "))
			json_object_object_add(target, "name", json_object_get(field(target, "address")));
	}
	if (*path) json_object_object_add(target, "identity", json_object_new_string(path));
	else json_object_object_del(target, "identity");
	connection_key_cancel(ui); ui->connection_field = 2; ui->row_cursor = 0;
}

static void connection_edit_start(struct manager_state *ui, json_object *row, bool copy) {
	ui->connection_selection = ui->selected;
	json_object_put(ui->connection_edit_base); ui->connection_edit_base = NULL;
	json_object_deep_copy(ui->connection_rows, &ui->connection_edit_base, NULL);
	if (copy && !connection_saved(row)) {
		json_object_object_add(row, "connection", json_object_new_int64(ui->next_connection_id--));
		json_object_object_add(row, "source", json_object_new_string("draft"));
	}
	ui->mode = UI_CONNECTION_NAME; ui->normal = true;
	snprintf(ui->query, sizeof(ui->query), "%s", json_object_get_string(field(row, connection_fields[ui->connection_field])));
	ui->cursor = MIN(ui->row_cursor, strlen(ui->query));
	while (ui->cursor && !g_utf8_validate(ui->query, ui->cursor, NULL)) ui->cursor--;
}

static void connection_edit_finish(struct manager_state *ui) {
	json_object *row = manager_item(ui, ui->selected);
	json_object_object_add(row, connection_fields[ui->connection_field], json_object_new_string(ui->query));
	if (ui->connection_inserting && !*json_object_get_string(field(row, "name")))
		json_object_object_add(row, "name", json_object_get(field(row, "address")));
	if (!json_object_equal(ui->connection_rows, ui->connection_edit_base)) {
		json_object_put(ui->connection_undo); ui->connection_undo = ui->connection_edit_base;
	} else json_object_put(ui->connection_edit_base);
	ui->connection_edit_base = NULL;
	ui->connection_inserting = false; ui->row_cursor = ui->cursor;
	ui->mode = UI_CONNECTIONS; ui->normal = true; ui->edit_operator = 0;
	ui->query[0] = '\0'; ui->cursor = 0; connection_matches(ui);
}

static void connection_edit_cancel(struct manager_state *ui) {
	json_object_put(ui->connection_rows); ui->connection_rows = ui->connection_edit_base; ui->connection_edit_base = NULL;
	ui->selected = ui->connection_selection; ui->connection_inserting = false;
	ui->mode = UI_CONNECTIONS; ui->normal = true; ui->edit_operator = 0;
	ui->query[0] = '\0'; ui->cursor = 0; connection_matches(ui);
}

static void connection_insert(struct manager_state *ui, bool above) {
	if (!ui->connection_base) { snprintf(ui->error, sizeof(ui->error), "Reload with Ctrl-r before editing"); return; }
	connection_unfilter(ui);
	size_t at = connection_count(ui->connection_rows) ? ui->selected + (above ? 0 : 1) : 0;
	ui->connection_selection = ui->selected;
	json_object_put(ui->connection_edit_base); ui->connection_edit_base = NULL;
	json_object_deep_copy(ui->connection_rows, &ui->connection_edit_base, NULL);
	json_object *rows = json_object_new_array(), *row = json_tokener_parse("{\"name\":\"\",\"kind\":\"ssh\",\"address\":\"\",\"source\":\"draft\"}");
	json_object_object_add(row, "connection", json_object_new_int64(ui->next_connection_id--));
	for (size_t i = 0; i <= connection_count(ui->connection_rows); i++) {
		if (i == at) json_object_array_add(rows, row);
		if (i < connection_count(ui->connection_rows)) json_object_array_add(rows, json_object_get(json_object_array_get_idx(ui->connection_rows, i)));
	}
	json_object_put(ui->connection_rows); ui->connection_rows = rows;
	ui->selected = at; ui->connection_field = 0; ui->row_cursor = 0; ui->connection_inserting = true;
	ui->mode = UI_CONNECTION_NAME; ui->normal = false; ui->edit_operator = 0;
}

static json_object *connection_find(json_object *rows, json_object *identity) {
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		json_object *row = json_object_array_get_idx(rows, i);
		if (json_object_equal(field(row, "connection"), identity)) return row;
	}
	return NULL;
}

static void connection_review_open(struct manager_state *ui) {
	ui->error[0] = '\0';
	if (!ui->connection_base) { snprintf(ui->error, sizeof(ui->error), "Reload with Ctrl-r before editing"); return; }
	if (!manager_connection_dirty(ui)) { snprintf(ui->error, sizeof(ui->error), "No staged changes"); return; }
	json_object *draft = connection_draft(ui);
	for (size_t i = 0; i < json_object_array_length(draft); i++) {
		json_object *row = json_object_array_get_idx(draft, i);
		if (!*json_object_get_string(field(row, "name")) || !*json_object_get_string(field(row, "address"))) {
			json_object_put(draft); snprintf(ui->error, sizeof(ui->error), "Name every connection and supply its endpoint"); return;
		}
	}
	ui->connection_request = operation("connections_apply");
	json_object_object_add(ui->connection_request, "base", json_object_get(ui->connection_base));
	json_object_object_add(ui->connection_request, "bookmarks", draft);
	ui->connection_review = json_object_new_array();
	for (int pass = 0; pass < 2; pass++) {
		json_object *rows = pass ? draft : ui->connection_base, *other = pass ? ui->connection_base : draft;
		for (size_t i = 0; i < json_object_array_length(rows); i++) {
			json_object *row = json_object_array_get_idx(rows, i), *old = connection_find(other, field(row, "connection"));
			if ((!pass && old) || (pass && old && json_object_equal(old, row))) continue;
			json_object *change = json_object_new_object();
			json_object_object_add(change, "name", json_object_get(field(row, "name")));
			json_object_object_add(change, "before", json_object_get(pass ? old : row));
			json_object_object_add(change, "after", json_object_get(pass ? row : NULL));
			json_object_array_add(ui->connection_review, change);
		}
	}
	ui->connection_selection = ui->selected; ui->mode = UI_CONNECTION_REVIEW;
	ui->normal = true; ui->selected = ui->first = 0; ui->edit_operator = 0;
}

static void connection_review_cancel(struct manager_state *ui) {
	json_object_put(ui->connection_request); ui->connection_request = NULL;
	json_object_put(ui->connection_review); ui->connection_review = NULL;
	ui->mode = UI_CONNECTIONS; ui->normal = true; ui->selected = ui->connection_selection; ui->first = 0;
}

static json_object *connection_accept(struct manager_state *ui, bool typed) {
	if (manager_connection_dirty(ui)) { snprintf(ui->error, sizeof(ui->error), "Review with = or Ctrl-s before connecting"); return NULL; }
	json_object *row = typed ? NULL : manager_item(ui, ui->selected);
	const char *address = row ? json_object_get_string(field(row, "address")) : ui->query;
	bool socket = row ? !strcmp(json_object_get_string(field(row, "kind")), "socket") :
		address[0] == '/' || g_str_has_prefix(address, "~/") || g_str_has_prefix(address, "unix://");
	if (!row && !*address) { snprintf(ui->error, sizeof(ui->error), "Enter an SSH destination or socket path"); return NULL; }
	if (row && field(row, "connection")) {
		const char *kind = json_object_get_string(field(row, "kind"));
		json_object *request = operation(!strcmp(kind, "ssh") || !strcmp(kind, "machine") ? "connection_browse" : "connection_select");
		json_object_object_add(request, "connection", json_object_get(field(row, "connection"))); return request;
	}
	json_object *request = operation(socket ? "connection_socket" : "connection_browse");
	json_object_object_add(request, socket ? "socket" : "destination", json_object_new_string(address));
	return request;
}

static bool connection_key(struct manager_state *ui, const struct manager_context *context, xkb_keysym_t sym, unsigned modifiers, json_object **request) {
	ui->error[0] = '\0';
	if (modifiers & UI_SUPER) return true;
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		if (!(modifiers & UI_ALT)) *request = connection_accept(ui, modifiers & UI_CONTROL);
		return true;
	}
	if (modifiers & (UI_CONTROL | UI_ALT)) return true;
	json_object *row = manager_item(ui, ui->selected);
	if (sym == XKB_KEY_slash) { ui->normal = false; ui->query[0] = '\0'; ui->cursor = 0; ui->selected = ui->first = 0; ui->edit_operator = 0; return true; }
	if (sym == XKB_KEY_equal && !ui->edit_operator) { connection_review_open(ui); return true; }
	if (sym == XKB_KEY_K && !ui->edit_operator) { connection_key_open(ui); return true; }
	if (sym == XKB_KEY_o || sym == XKB_KEY_O) { connection_insert(ui, sym == XKB_KEY_O); return true; }
	if (sym == XKB_KEY_u && ui->connection_undo) {
		json_object_put(ui->connection_rows); ui->connection_rows = ui->connection_undo; ui->connection_undo = NULL;
		ui->query[0] = '\0'; connection_matches(ui); return true;
	}
	if (sym == XKB_KEY_j || sym == XKB_KEY_Down || sym == XKB_KEY_k || sym == XKB_KEY_Up || sym == XKB_KEY_G) {
		if ((sym == XKB_KEY_j || sym == XKB_KEY_Down) && manager_item(ui, ui->selected + 1)) ui->selected++;
		if ((sym == XKB_KEY_k || sym == XKB_KEY_Up) && ui->selected) ui->selected--;
		if (sym == XKB_KEY_G) ui->selected = MAX(0, (int)connection_count(ui->connection_matches) - 1);
		ui->row_cursor = 0; ui->edit_operator = 0; ui->repeatable = true; return true;
	}
	if (!row) return true;
	if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
		int count = !strcmp(json_object_get_string(field(row, "kind")), "ssh") ? 3 : 2;
		ui->connection_field = (ui->connection_field + (sym == XKB_KEY_Tab ? 1 : count - 1)) % count;
		ui->row_cursor = 0; return true;
	}
	if (sym == XKB_KEY_d && ui->edit_operator == 'd') {
		ui->edit_operator = 0;
		if (!connection_saved(row)) { snprintf(ui->error, sizeof(ui->error), "Only saved connections can be forgotten"); return true; }
		connection_unfilter(ui); connection_undo_save(ui);
		json_object_array_del_idx(ui->connection_rows, ui->selected, 1); connection_matches(ui); return true;
	}
	if (sym == XKB_KEY_d || sym == XKB_KEY_c) { ui->edit_operator = sym == XKB_KEY_d ? 'd' : 'c'; return true; }
	bool text = ui->edit_operator || sym == XKB_KEY_i || sym == XKB_KEY_a || sym == XKB_KEY_I || sym == XKB_KEY_A ||
		sym == XKB_KEY_h || sym == XKB_KEY_l || sym == XKB_KEY_0 || sym == XKB_KEY_dollar || sym == XKB_KEY_w || sym == XKB_KEY_b || sym == XKB_KEY_e ||
		sym == XKB_KEY_x || sym == XKB_KEY_X || sym == XKB_KEY_C || sym == XKB_KEY_D || sym == XKB_KEY_s;
	if (text) {
		if (ui->connection_field == 2) { ui->edit_operator = 0; connection_key_open(ui); return true; }
		if (!strcmp(json_object_get_string(field(row, "kind")), "machine") || !strcmp(json_object_get_string(field(row, "kind")), "command") || !strcmp(json_object_get_string(field(row, "source")), "typed")) {
			snprintf(ui->error, sizeof(ui->error), "Use o/O to add a saved SSH or socket connection"); ui->edit_operator = 0; return true;
		}
		if (strlen(json_object_get_string(field(row, connection_fields[ui->connection_field]))) >= sizeof(ui->query)) {
			snprintf(ui->error, sizeof(ui->error), "Field is too long for inline editing"); ui->edit_operator = 0; return true;
		}
		bool motion = !ui->edit_operator && (sym == XKB_KEY_h || sym == XKB_KEY_l || sym == XKB_KEY_0 || sym == XKB_KEY_dollar || sym == XKB_KEY_w || sym == XKB_KEY_b || sym == XKB_KEY_e);
		connection_unfilter(ui); connection_edit_start(ui, row, !motion);
		manager_key(ui, context, sym, 0, request);
		if (ui->mode == UI_CONNECTION_NAME && ui->normal) connection_edit_finish(ui);
	}
	return true;
}
