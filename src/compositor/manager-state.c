#include "manager-state.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static json_object *field(json_object *object, const char *name) {
	json_object *value = NULL;
	if (object) json_object_object_get_ex(object, name, &value);
	return value;
}

static json_object *operation(const char *name) {
	json_object *message = json_object_new_object();
	json_object_object_add(message, "op", json_object_new_string(name));
	return message;
}

bool manager_rename(const struct manager_state *ui) {
	return ui->mode == UI_SESSION_NAME || ui->mode == UI_WINDOW_NAME || ui->mode == UI_PANE_NAME || ui->mode == UI_ARRANGE_NAME || ui->mode == UI_CONNECTION_NAME;
}

static size_t ui_previous(struct manager_state *ui, size_t index) {
	char *previous = g_utf8_find_prev_char(ui->query, ui->query + index);
	return previous ? (size_t)(previous - ui->query) : 0;
}

static size_t ui_next(struct manager_state *ui, size_t index) {
	return ui->query[index] ? (size_t)(g_utf8_next_char(ui->query + index) - ui->query) : index;
}

static bool ui_replace(struct manager_state *ui, size_t first, size_t last, const char *text) {
	size_t length = strlen(ui->query), added = strlen(text);
	if (length - (last - first) + added >= sizeof(ui->query)) return false;
	memmove(ui->query + first + added, ui->query + last, length - last + 1);
	memcpy(ui->query + first, text, added);
	ui->cursor = first + added;
	if (ui->mode == UI_SSH_DESTINATION || ui->mode == UI_SSH_KEY || ui->mode == UI_SOCKETS || ui->mode == UI_CONNECTIONS || ui->mode == UI_ARRANGE) ui->selected = ui->first = 0;
	return true;
}

static int ui_char_class(struct manager_state *ui, size_t index) {
	gunichar ch = g_utf8_get_char(ui->query + index);
	return !ch || g_unichar_isspace(ch) ? 0 : g_unichar_isalnum(ch) || ch == '_' ? 1 : 2;
}

static size_t ui_word(struct manager_state *ui, bool forward) {
	size_t index = forward ? ui->cursor : ui_previous(ui, ui->cursor);
	if (forward) {
		int type = ui_char_class(ui, index);
		while (ui->query[index] && ui_char_class(ui, index) == type) index = ui_next(ui, index);
		while (ui->query[index] && !ui_char_class(ui, index)) index = ui_next(ui, index);
	} else {
		while (index && !ui_char_class(ui, index)) index = ui_previous(ui, index);
		int type = ui_char_class(ui, index);
		while (index && ui_char_class(ui, ui_previous(ui, index)) == type) index = ui_previous(ui, index);
	}
	return index;
}

static size_t ui_word_end(struct manager_state *ui, size_t index) {
	while (ui->query[index] && !ui_char_class(ui, index)) index = ui_next(ui, index);
	int type = ui_char_class(ui, index);
	while (ui->query[index] && ui_char_class(ui, index) == type) index = ui_next(ui, index);
	return index;
}

static void ui_yank(struct manager_state *ui, size_t first, size_t last) {
	if (first == last) return;
	memcpy(ui->yank, ui->query + first, last - first);
	ui->yank[last - first] = '\0';
}

static void ui_cut(struct manager_state *ui, size_t first, size_t last, bool insert) {
	ui_yank(ui, first, last);
	ui_replace(ui, first, last, "");
	if (insert) ui->normal = false;
}

static void ui_operator(struct manager_state *ui, xkb_keysym_t sym) {
	char op = ui->edit_operator;
	ui->edit_operator = 0;
	size_t first = ui->cursor, last;
	if (sym == (op == 'c' ? XKB_KEY_c : op == 'd' ? XKB_KEY_d : XKB_KEY_y)) {
		first = 0; last = strlen(ui->query);
	} else switch (sym) {
	case XKB_KEY_w:
		last = op == 'c' && ui_char_class(ui, first) ? ui_word_end(ui, first) : ui_word(ui, true); break;
	case XKB_KEY_e: last = ui_word_end(ui, ui_next(ui, first)); break;
	case XKB_KEY_b: last = ui_word(ui, false); break;
	case XKB_KEY_0: case XKB_KEY_Home: last = 0; break;
	case XKB_KEY_dollar: case XKB_KEY_End: last = strlen(ui->query); break;
	case XKB_KEY_h: case XKB_KEY_Left: last = ui_previous(ui, first); break;
	case XKB_KEY_l: case XKB_KEY_Right: last = ui_next(ui, first); break;
	default: return;
	}
	if (last < first) { size_t swap = first; first = last; last = swap; }
	if (op == 'y') ui_yank(ui, first, last);
	else ui_cut(ui, first, last, op == 'c');
}

json_object *manager_window(const struct manager_state *ui) {
	json_object *windows = field(ui->state, "windows");
	if (!windows) return NULL;
	for (size_t i = 0; i < json_object_array_length(windows); i++) {
		json_object *window = json_object_array_get_idx(windows, i);
		if (json_object_get_int64(field(window, "window")) == json_object_get_int64(field(ui->state, "active_window")))
			return window;
	}
	return NULL;
}

static json_object *ui_items(const struct manager_state *ui) {
	return ui->mode == UI_CONNECTIONS ? ui->connections : ui->mode == UI_SESSIONS ? ui->sessions : field(ui->state, "windows");
}

bool manager_session_empty(const struct manager_state *ui) {
	json_object *windows = field(ui->state, "windows");
	return windows && !json_object_array_length(windows);
}

static json_object *ui_pane(const struct manager_state *ui) {
	json_object *panes = field(manager_window(ui), "panes");
	for (size_t i = 0; panes && i < json_object_array_length(panes); i++) {
		json_object *pane = json_object_array_get_idx(panes, i);
		if (json_object_get_int64(field(pane, "id")) == json_object_get_int64(field(manager_window(ui), "focus"))) return pane;
	}
	return NULL;
}

static int fuzzy_score(const char *label, const char *query);

static int arrangement_index(const struct manager_state *ui, int index) {
	if (index < 0 || !ui->arrangement_rows) return -1;
	if (!*ui->query || ui->mode == UI_ARRANGE_NAME)
		return (size_t)index < json_object_array_length(ui->arrangement_rows) ? index : -1;
	json_object *session = NULL;
	int matched = 0;
	for (size_t i = 0; i < json_object_array_length(ui->arrangement_rows); i++) {
		json_object *row = json_object_array_get_idx(ui->arrangement_rows, i);
		if (field(row, "session")) session = row;
		char *label = field(row, "session") ?
			g_strdup_printf("%" PRId64 " %s", json_object_get_int64(field(row, "session")), json_object_get_string(field(row, "name"))) :
			g_strdup_printf("%s %" PRId64 " %s", json_object_get_string(field(session, "name")),
				json_object_get_int64(field(row, "window")), json_object_get_string(field(row, "name")));
		bool match = fuzzy_score(label, ui->query) != INT_MIN;
		g_free(label);
		if (match)
			if (matched++ == index) return i;
	}
	return -1;
}

json_object *manager_arrangement_session(const struct manager_state *ui, int index) {
	int raw = arrangement_index(ui, index);
	while (raw >= 0) {
		json_object *row = json_object_array_get_idx(ui->arrangement_rows, raw--);
		if (field(row, "session")) return row;
	}
	return NULL;
}

json_object *manager_item(const struct manager_state *ui, int index) {
	if (ui->mode == UI_SOCKETS) return index < 0 || !ui->socket_matches ? NULL : json_object_array_get_idx(ui->socket_matches, index);
	if (ui->mode == UI_SSH_KEY) return index < 0 || !ui->key_matches ? NULL : json_object_array_get_idx(ui->key_matches, index);
	if (ui->mode == UI_CONNECTION_REVIEW) return index < 0 ? NULL : json_object_array_get_idx(ui->connection_review, index);
	if (ui->mode == UI_CONNECTION_NAME) return index < 0 ? NULL : json_object_array_get_idx(ui->connection_rows, index);
	if (ui->mode == UI_CONNECTIONS) return index < 0 || !ui->connection_matches ? NULL : json_object_array_get_idx(ui->connection_matches, index);
	if (ui->mode == UI_ARRANGE_REVIEW)
		return index < 0 ? NULL : json_object_array_get_idx(ui->arrangement_review_rows, index);
	if (ui->mode == UI_ARRANGE || ui->mode == UI_ARRANGE_NAME) {
		int raw = arrangement_index(ui, index);
		return raw < 0 ? NULL : json_object_array_get_idx(ui->arrangement_rows, raw);
	}
	if (ui->mode == UI_SSH_DESTINATION)
		return !ui->ssh_matches || index < 0 ? NULL : json_object_array_get_idx(ui->ssh_matches, index);
	json_object *items = ui_items(ui);
	if (!items) return NULL;
	int matched = 0;
	for (size_t i = 0; i < json_object_array_length(items); i++) {
		json_object *item = json_object_array_get_idx(items, i);
		char label[1024];
		const char *identity = ui->mode == UI_CONNECTIONS ? "connection" : ui->mode == UI_SESSIONS ? "session" : "number";
		snprintf(label, sizeof(label), "%" PRId64 " %s", json_object_get_int64(field(item, identity)),
			json_object_get_string(field(item, "name")));
		if (!*ui->query || strcasestr(label, ui->query)) {
			if (matched++ == index) return item;
		}
	}
	return NULL;
}

static int fuzzy_score(const char *label, const char *query) {
	if (!*query) return 0;
	gchar *text = g_utf8_casefold(label, -1), *needle = g_utf8_casefold(query, -1);
	int score = INT_MIN;
	char *substring = strstr(text, needle);
	if (!strcmp(text, needle)) score = 100000;
	else if (substring) score = (substring == text ? 90000 : 80000) - (substring - text);
	else {
		const char *next = needle;
		int value = 1000, gap = 0;
		gunichar previous = 0;
		for (const char *at = text; *at && *next; at = g_utf8_next_char(at)) {
			gunichar ch = g_utf8_get_char(at);
			if (ch == g_utf8_get_char(next)) {
				value += (gap ? -gap : 20) + (!previous || !g_unichar_isalnum(previous) ? 15 : 0);
				next = g_utf8_next_char(next); gap = 0;
			} else gap++;
			previous = ch;
		}
		if (!*next) score = value;
	}
	g_free(text); g_free(needle);
	return score;
}

struct ssh_match { json_object *item; int score; size_t order; };

static gint ssh_compare(gconstpointer left, gconstpointer right) {
	const struct ssh_match *a = left, *b = right;
	if (a->score != b->score) return a->score > b->score ? -1 : 1;
	return a->order < b->order ? -1 : a->order > b->order;
}

static void ssh_matches(struct manager_state *ui) {
	GArray *matches = g_array_new(false, false, sizeof(struct ssh_match));
	for (size_t i = 0; ui->ssh_targets && i < json_object_array_length(ui->ssh_targets); i++) {
		json_object *item = json_object_array_get_idx(ui->ssh_targets, i);
		int score = fuzzy_score(json_object_get_string(field(item, "destination")), ui->query);
		if (score != INT_MIN) {
			struct ssh_match match = {item, score, i};
			g_array_append_val(matches, match);
		}
	}
	g_array_sort(matches, ssh_compare);
	json_object_put(ui->ssh_matches);
	ui->ssh_matches = json_object_new_array();
	for (guint i = 0; i < matches->len; i++)
		json_object_array_add(ui->ssh_matches, json_object_get(g_array_index(matches, struct ssh_match, i).item));
	g_array_free(matches, true);
	if (*ui->query) {
		json_object *typed = json_object_new_object();
		char label[sizeof(ui->query) + 32];
		snprintf(label, sizeof(label), "Connect to %s", ui->query);
		json_object_object_add(typed, "name", json_object_new_string(label));
		json_object_object_add(typed, "destination", json_object_new_string(ui->query));
		json_object_object_add(typed, "source", json_object_new_string("typed"));
		json_object_array_add(ui->ssh_matches, typed);
	}
}

static void ui_selection(struct manager_state *ui);
#include "connection-state.c"

static void socket_matches(struct manager_state *ui) {
	GArray *matches = g_array_new(false, false, sizeof(struct ssh_match));
	for (size_t i = 0; ui->sockets && i < json_object_array_length(ui->sockets); i++) {
		json_object *row = json_object_array_get_idx(ui->sockets, i);
		char *label = g_strdup_printf("%s %s", json_object_get_string(field(row, "name")), json_object_get_string(field(row, "path")));
		int score = fuzzy_score(label, ui->query); g_free(label);
		if (score != INT_MIN) { struct ssh_match match = {row, score, i}; g_array_append_val(matches, match); }
	}
	g_array_sort(matches, ssh_compare);
	json_object_put(ui->socket_matches); ui->socket_matches = json_object_new_array();
	for (size_t i = 0; i < matches->len; i++) json_object_array_add(ui->socket_matches, json_object_get(g_array_index(matches, struct ssh_match, i).item));
	g_array_free(matches, true);
	if (*ui->query) {
		json_object *row = json_object_new_object();
		json_object_object_add(row, "socket", json_object_new_string(ui->query));
		json_object_object_add(row, "path", json_object_new_string(ui->query));
		json_object_object_add(row, "name", json_object_new_string("Use this socket path"));
		json_object_object_add(row, "source", json_object_new_string("typed"));
		json_object_array_add(ui->socket_matches, row);
	}
}

static json_object *socket_accept(struct manager_state *ui, bool typed) {
	json_object *row = typed ? NULL : manager_item(ui, ui->selected);
	const char *path = row ? json_object_get_string(field(row, "socket")) : ui->query;
	if (!row && !*path) { snprintf(ui->error, sizeof(ui->error), "Select a server or enter a socket path"); return NULL; }
	json_object *request = operation("route_select");
	json_object_object_add(request, "socket", json_object_new_string(path));
	return request;
}

static void ui_selection(struct manager_state *ui) {
	if (ui->mode == UI_SSH_KEY) key_matches(ui);
	if (ui->mode == UI_CONNECTIONS) connection_matches(ui);
	if (ui->mode == UI_SSH_DESTINATION) ssh_matches(ui);
	if (ui->mode == UI_SOCKETS) socket_matches(ui);
	bool picker = ui->mode == UI_SOCKETS || ui->mode == UI_SESSIONS || ui->mode == UI_WINDOWS || ui->mode == UI_CONNECTIONS || ui->mode == UI_SSH_DESTINATION || ui->mode == UI_SSH_KEY || ui->mode == UI_ARRANGE || ui->mode == UI_ARRANGE_REVIEW || ui->mode == UI_CONNECTION_REVIEW;
	if (picker && !manager_item(ui, ui->selected)) ui->selected = 0;
	if (ui->selected < ui->first) ui->first = ui->selected;
	int visible = (ui->mode == UI_ARRANGE_REVIEW || ui->mode == UI_CONNECTION_REVIEW) ? MANAGER_REVIEW_ROWS : 8;
	if (ui->selected >= ui->first + visible) ui->first = ui->selected - visible + 1;
	if (ui->mode == UI_CONNECTIONS && ui->normal) {
		json_object *row = manager_item(ui, ui->selected);
		if (row) {
			if (ui->connection_field >= 2 && strcmp(json_object_get_string(field(row, "kind")), "ssh")) ui->connection_field = 0;
			const char *text = ui->connection_field == 2 ? connection_identity(row) : json_object_get_string(field(row, connection_fields[ui->connection_field]));
			ui->row_cursor = MIN(ui->row_cursor, strlen(text));
			while (ui->row_cursor && !g_utf8_validate(text, ui->row_cursor, NULL)) ui->row_cursor--;
			if (ui->row_cursor && !text[ui->row_cursor]) ui->row_cursor = g_utf8_find_prev_char(text, text + ui->row_cursor) - text;
		}
	}
	if (ui->mode == UI_ARRANGE && ui->normal) {
		json_object *row = manager_item(ui, ui->selected);
		if (row) {
			const char *name = json_object_get_string(field(row, "name"));
			size_t length = strlen(name);
			if (ui->row_cursor > length) ui->row_cursor = length;
			while (ui->row_cursor && !g_utf8_validate(name, ui->row_cursor, NULL)) ui->row_cursor--;
			if (length && ui->row_cursor == length) ui->row_cursor = g_utf8_find_prev_char(name, name + length) - name;
		}
	}
}

static void arrangement_finish_name(struct manager_state *ui, const struct manager_context *context, bool allow_empty);
#include "arrangement-state.c"

void manager_close(struct manager_state *ui, const struct manager_context *context) {
	if (ui->mode == UI_SSH_KEY) { connection_key_cancel(ui); ui_selection(ui); return; }
	if (ui->compose) xkb_compose_state_reset(ui->compose);
	if (ui->mode == UI_CONNECTION_NAME) { connection_edit_cancel(ui); ui_selection(ui); return; }
	if (ui->mode == UI_CONNECTION_REVIEW) connection_review_cancel(ui);
	connection_clear(ui);
	if (ui->mode == UI_ARRANGE_REVIEW) arrangement_review_cancel(ui);
	if (ui->mode == UI_ARRANGE_NAME) {
		if (ui->arrangement_inserting) {
			json_object_array_del_idx(ui->arrangement_rows, ui->selected, 1);
			ui->selected = ui->insertion_selection;
			ui->arrangement_inserting = false;
			json_object_put(ui->insertion_base); ui->insertion_base = NULL;
		}
		ui->mode = UI_ARRANGE; ui->normal = true; ui->edit_operator = 0;
		ui->query[0] = '\0'; ui->cursor = 0;
		ui_selection(ui);
		return;
	}
	if (ui->mode == UI_ARRANGE && !context->attached && context->defer_attach && context->connected) {
		if (ui->arrangement_base) {
			json_object *base = json_object_get(ui->arrangement_base);
			arrangement_load(ui, base);
			json_object_put(base);
		}
		ui->normal = false; ui->query[0] = '\0'; ui->cursor = 0;
		ui->selected = ui->first = 0; ui->opening_window = 0;
		return;
	}
	arrangement_clear(ui);
	ui->opening_window = 0;
	ui->mode = UI_NONE;
	if (!context->attached && context->defer_attach) ui->mode = context->connected ? UI_SESSIONS : UI_CONNECTIONS;
}

json_object *manager_open(struct manager_state *ui, enum ui_mode mode, const struct manager_context *context) {
	if (ui->pending) return NULL;
	if (mode != UI_CONNECTION_NAME) connection_clear(ui);
	bool browsing = mode == UI_SESSIONS;
	if (mode == UI_SESSIONS) mode = UI_ARRANGE;
	if (mode == UI_ARRANGE && !context->connected) mode = UI_CONNECTIONS;
	ui->opening_window = 0;
	if (mode != UI_ARRANGE_NAME) arrangement_clear(ui);
	if (!ui->compose) {
		const char *locale = getenv("LC_ALL");
		if (!locale || !*locale) locale = getenv("LC_CTYPE");
		if (!locale || !*locale) locale = getenv("LANG");
		struct xkb_context *xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_compose_table *table = xkb_compose_table_new_from_locale(xkb,
			locale && *locale ? locale : "C.UTF-8", XKB_COMPOSE_COMPILE_NO_FLAGS);
		if (table) ui->compose = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
		xkb_compose_table_unref(table);
		xkb_context_unref(xkb);
	}
	if (ui->compose) xkb_compose_state_reset(ui->compose);
	ui->mode = mode;
	ui->normal = false;
	if (mode == UI_ARRANGE) ui->normal = !browsing;
	if (mode == UI_HELP) ui->normal = true;
	ui->edit_operator = 0;
	ui->selected = ui->first = 0;
	ui->query[0] = ui->error[0] = '\0';
	if (mode == UI_SSH_DESTINATION) ui->ssh_destination[0] = '\0';
	const char *name = NULL;
	if (mode == UI_SESSION_NAME) name = json_object_get_string(field(ui->state, "name"));
	if (mode == UI_WINDOW_NAME) name = json_object_get_string(field(manager_window(ui), "name"));
	if (mode == UI_PANE_NAME) {
		name = json_object_get_string(field(ui_pane(ui), "name"));
	}
	if (name) snprintf(ui->query, sizeof(ui->query), "%s", name);
	const char *valid_end;
	if (!g_utf8_validate(ui->query, -1, &valid_end)) ui->query[valid_end - ui->query] = '\0';
	ui->cursor = strlen(ui->query);
	ui_selection(ui);
	if (mode == UI_SESSIONS) return operation("list");
	if (mode == UI_CONNECTIONS) return operation("connections");
	if (mode == UI_SSH_DESTINATION) return operation("ssh_targets");
	if (mode == UI_SOCKETS) return operation("route_sockets");
	if (mode == UI_HELP) return operation("help");
	if (mode == UI_ARRANGE) return operation("arrangement");
	return NULL;
}

static json_object *ssh_accept(struct manager_state *ui, bool typed) {
	json_object *item = typed ? NULL : manager_item(ui, ui->selected);
	const char *destination = item ? json_object_get_string(field(item, "destination")) : ui->query;
	if (!*destination) {
		snprintf(ui->error, sizeof(ui->error), "Enter an SSH destination"); return NULL;
	}
	snprintf(ui->ssh_destination, sizeof(ui->ssh_destination), "%s", destination);
	json_object *message = operation("connection_browse");
	json_object_object_add(message, "destination", json_object_new_string(destination));
	if (*connection_identity(item)) json_object_object_add(message, "identity", json_object_get(field(item, "identity")));
	return message;
}

static void arrangement_finish_name(struct manager_state *ui, const struct manager_context *context, bool allow_empty) {
	json_object *row = manager_item(ui, ui->selected);
	if ((!allow_empty && !*ui->query) || !g_utf8_validate(ui->query, -1, NULL)) return;
	if (ui->arrangement_inserting) {
		json_object_put(ui->arrangement_undo); ui->arrangement_undo = ui->insertion_base; ui->insertion_base = NULL;
		json_object_put(ui->cut_undo); ui->cut_undo = json_object_get(ui->cut_window);
	} else if (strcmp(json_object_get_string(field(row, "name")), ui->query)) arrangement_save_undo(ui);
	json_object_object_add(row, "name", json_object_new_string(ui->query));
	ui->row_cursor = ui->cursor;
	ui->arrangement_inserting = false;
	manager_close(ui, context);
}

json_object *manager_accept(struct manager_state *ui, const struct manager_context *context) {
	if (!ui->mode || ui->pending) return NULL;
	if (ui->mode == UI_SSH_KEY) { connection_key_accept(ui, false); ui_selection(ui); return NULL; }
	if (ui->mode == UI_CONNECTION_REVIEW) {
		json_object *request = ui->connection_request; ui->connection_request = NULL;
		connection_review_cancel(ui); ui_selection(ui); return request;
	}
	if (ui->mode == UI_ARRANGE_REVIEW) return arrangement_review_confirm(ui, context);
	if (ui->mode == UI_ARRANGE) return arrangement_choose(ui, context);
	if (ui->mode == UI_ARRANGE_NAME) {
		arrangement_finish_name(ui, context, false); return NULL;
	}
	json_object *message;
	if (ui->mode == UI_CONNECTIONS) return connection_accept(ui, false);
	if (ui->mode == UI_SOCKETS) return socket_accept(ui, false);
	if (ui->mode == UI_CONNECTION_NAME) { connection_edit_finish(ui); return NULL; }
	if (ui->mode == UI_SSH_DESTINATION) {
		return ssh_accept(ui, false);
	}
	if (ui->mode == UI_WINDOWS || ui->mode == UI_SESSIONS || ui->mode == UI_CONNECTIONS) {
		json_object *item = manager_item(ui, ui->selected);
		if (!item) return NULL;
		bool session = ui->mode == UI_SESSIONS;
		message = operation(ui->mode == UI_CONNECTIONS ? "connection_select" : session ? (context->attached ? "session_select" : "attach") : "window_select");
		const char *id = ui->mode == UI_CONNECTIONS ? "connection" : session ? "session" : "number";
		json_object_object_add(message, id, json_object_get(field(item, id)));
		if (session && !context->attached) {
			json_object_object_add(message, "width", json_object_new_int(context->width));
			json_object_object_add(message, "height", json_object_new_int(context->height));
			json_object_object_add(message, "events", json_object_new_boolean(true));
		}
	} else if (ui->mode == UI_HELP) { manager_close(ui, context); return NULL; }
	else {
		const char *op = ui->mode == UI_COMMAND ? "command" : ui->mode == UI_SESSION_NAME ? "session_rename" : ui->mode == UI_WINDOW_NAME ? "window_rename" : "pane_rename";
		message = operation(op);
		json_object_object_add(message, ui->mode == UI_COMMAND ? "text" : "name", json_object_new_string(ui->query));
	}
	return message;
}

bool manager_key(struct manager_state *ui, const struct manager_context *context,
	xkb_keysym_t sym, unsigned modifiers, json_object **request) {
	*request = NULL;
	ui->repeatable = false;
	if (!ui->mode) return false;
	if ((modifiers & UI_CONTROL) && sym == XKB_KEY_g) {
		if (ui->mode == UI_SOCKETS) {
			if (ui->pending) ui->route_cancel_pending = true;
			else *request = operation("route_cancel");
		}
		manager_close(ui, context); return true;
	}
	if (ui->pending) return true;
	if (sym == XKB_KEY_Escape) {
		if (ui->mode == UI_CONNECTION_REVIEW) { connection_review_cancel(ui); ui_selection(ui); return true; }
		if (ui->mode == UI_CONNECTION_NAME) {
			if (!ui->normal) ui->cursor = ui_previous(ui, ui->cursor);
			connection_edit_finish(ui); ui_selection(ui); return true;
		}
		if (ui->mode == UI_ARRANGE_REVIEW) { arrangement_review_cancel(ui); return true; }
		if (ui->mode == UI_ARRANGE_NAME) {
			if (!ui->normal) ui->cursor = ui_previous(ui, ui->cursor);
			arrangement_finish_name(ui, context, true);
		}
		else {
			if (!ui->normal) ui->cursor = ui_previous(ui, ui->cursor);
			ui->normal = true;
			ui->edit_operator = 0;
			if (ui->compose) xkb_compose_state_reset(ui->compose);
		}
		ui_selection(ui);
		return true;
	}
	if (ui->normal && !ui->edit_operator && sym == XKB_KEY_q && !(modifiers & (UI_CONTROL | UI_ALT | UI_SUPER))) {
		if (ui->mode == UI_SOCKETS) *request = operation("route_cancel");
		struct manager_context closing = *context;
		closing.defer_attach = false;
		if (ui->mode == UI_CONNECTION_NAME) { connection_edit_finish(ui); }
		if (ui->mode == UI_ARRANGE_REVIEW) arrangement_review_cancel(ui);
		if (ui->mode == UI_ARRANGE_NAME) ui->mode = UI_ARRANGE;
		manager_close(ui, &closing); return true;
	}
	if (ui->mode == UI_SOCKETS && (modifiers & UI_CONTROL)) {
		if (sym == XKB_KEY_o) { *request = manager_open(ui, UI_CONNECTIONS, context); return true; }
		if (sym == XKB_KEY_r) { *request = operation("route_sockets"); return true; }
	}
	if (ui->mode == UI_SOCKETS && ui->normal && sym == XKB_KEY_slash && !modifiers) {
		ui->normal = false; ui->query[0] = '\0'; ui->cursor = 0; ui->selected = ui->first = 0;
		ui->edit_operator = 0; ui_selection(ui); return true;
	}
	if (ui->mode == UI_CONNECTION_REVIEW) {
		if (modifiers & (UI_CONTROL | UI_ALT | UI_SUPER)) return true;
		if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_y) {
			*request = manager_accept(ui, context);
		} else if (sym == XKB_KEY_n) connection_review_cancel(ui);
		else if (sym == XKB_KEY_j || sym == XKB_KEY_Down) { if (manager_item(ui, ui->selected + 1)) ui->selected++; }
		else if (sym == XKB_KEY_k || sym == XKB_KEY_Up) { if (ui->selected) ui->selected--; }
		else if (sym == XKB_KEY_G) ui->selected = MAX(0, (int)json_object_array_length(ui->connection_review) - 1);
		ui_selection(ui); return true;
	}
	if (ui->mode == UI_CONNECTION_NAME) {
		if ((modifiers & UI_CONTROL) && sym == XKB_KEY_o) { snprintf(ui->error, sizeof(ui->error), "Finish the connection edit before switching"); return true; }
		if ((modifiers & UI_CONTROL) && sym == XKB_KEY_s) { connection_edit_finish(ui); connection_review_open(ui); ui_selection(ui); return true; }
		if (ui->connection_inserting && !(modifiers & (UI_CONTROL | UI_ALT | UI_SUPER)) && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
			json_object *row = manager_item(ui, ui->selected);
			bool socket = !strcmp(json_object_get_string(field(row, "kind")), "ssh");
			json_object_object_add(row, "kind", json_object_new_string(socket ? "socket" : "ssh")); return true;
		}
	}
	if (ui->mode == UI_CONNECTIONS && (modifiers & UI_CONTROL)) {
		if (sym == XKB_KEY_k) { connection_key_open(ui); ui_selection(ui); return true; }
		if (sym == XKB_KEY_r) { *request = operation("connections"); return true; }
		if (sym == XKB_KEY_e) { connection_unfilter(ui); ui->normal = true; ui_selection(ui); return true; }
		if (sym == XKB_KEY_s) { connection_unfilter(ui); connection_review_open(ui); ui_selection(ui); return true; }
		if ((sym == XKB_KEY_o || sym == XKB_KEY_n) && manager_connection_dirty(ui)) {
			snprintf(ui->error, sizeof(ui->error), "Review with = or Ctrl-s before switching"); return true;
		}
		if (sym == XKB_KEY_o && context->connected) { *request = manager_open(ui, UI_SESSIONS, context); return true; }
	}
	if (ui->mode == UI_ARRANGE_REVIEW) {
		if (modifiers & (UI_CONTROL | UI_ALT | UI_SUPER)) return true;
		if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_y)
			*request = arrangement_review_confirm(ui, context);
		else if (sym == XKB_KEY_n) arrangement_review_cancel(ui);
		else if (sym == XKB_KEY_Tab) {
			json_object *peer = field(manager_item(ui, ui->selected), "peer");
			if (peer) ui->selected = json_object_get_int(peer);
		}
		else if (sym == XKB_KEY_j || sym == XKB_KEY_Down) {
			if (manager_item(ui, ui->selected + 1)) ui->selected++;
			ui->repeatable = true;
		} else if (sym == XKB_KEY_k || sym == XKB_KEY_Up) {
			if (ui->selected) ui->selected--;
			ui->repeatable = true;
		} else if (sym == XKB_KEY_G) {
			ui->selected = json_object_array_length(ui->arrangement_review_rows) - 1;
		}
		ui_selection(ui); return true;
	}
	if (ui->mode == UI_ARRANGE_NAME && ui->arrangement_inserting) {
		if (!(modifiers & (UI_CONTROL | UI_ALT | UI_SUPER)) && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
			arrangement_indent(ui, sym == XKB_KEY_Tab); return true;
		}
	}
	if (ui->mode == UI_ARRANGE_NAME && (modifiers & UI_CONTROL) && sym == XKB_KEY_o) {
		snprintf(ui->error, sizeof(ui->error), "Finish or cancel the name edit before switching"); return true;
	}
	if (ui->mode == UI_ARRANGE_NAME && (modifiers & UI_CONTROL) && sym == XKB_KEY_s) {
		arrangement_finish_name(ui, context, true);
		if (ui->mode == UI_ARRANGE) arrangement_review(ui, context);
		return true;
	}
	if (ui->mode == UI_ARRANGE && (modifiers & UI_CONTROL)) {
		if (sym == XKB_KEY_s) {
			arrangement_review(ui, context);
			return true;
		}
		if (sym == XKB_KEY_r) { *request = operation("arrangement"); return true; }
		if (sym == XKB_KEY_e) { ui->normal = true; arrangement_unfilter(ui); return true; }
		if ((sym == XKB_KEY_o || sym == XKB_KEY_n) && manager_arrangement_dirty(ui)) {
			snprintf(ui->error, sizeof(ui->error), "Review with = or Ctrl-s before switching"); return true;
		}
		if (sym == XKB_KEY_n) {
			*request = operation(context->attached ? "session_new" : "attach");
			if (!context->attached) {
				json_object_object_add(*request, "width", json_object_new_int(context->width));
				json_object_object_add(*request, "height", json_object_new_int(context->height));
				json_object_object_add(*request, "events", json_object_new_boolean(true));
			}
			return true;
		}
	}
	if ((modifiers & UI_CONTROL) && sym == XKB_KEY_e && (ui->mode == UI_SESSIONS || ui->mode == UI_WINDOWS)) {
		*request = manager_open(ui, UI_ARRANGE, context); return true;
	}
	if ((modifiers & UI_CONTROL) && sym == XKB_KEY_n && ui->mode == UI_CONNECTIONS) {
		*request = manager_open(ui, UI_SSH_DESTINATION, context); return true;
	}
	if (ui->mode == UI_SSH_KEY && (modifiers & UI_CONTROL) && sym == XKB_KEY_o) {
		connection_key_cancel(ui); ui_selection(ui); return true;
	}
	if (ui->mode == UI_SSH_KEY && ui->normal && !ui->edit_operator && sym == XKB_KEY_slash && !modifiers) {
		ui->normal = false; ui->query[0] = '\0'; ui->cursor = 0; ui->selected = ui->first = 0; ui_selection(ui); return true;
	}
	if ((modifiers & UI_CONTROL) && sym == XKB_KEY_o && ui->mode != UI_CONNECTIONS) {
		*request = manager_open(ui, UI_CONNECTIONS, context); return true;
	}
	if (ui->mode == UI_CONNECTIONS && ui->normal) { bool handled = connection_key(ui, context, sym, modifiers, request); ui_selection(ui); return handled; }
	if (ui->mode == UI_ARRANGE && ui->normal) return arrangement_key(ui, context, sym, modifiers, request);
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		if ((ui->mode == UI_CONNECTIONS || ui->mode == UI_SSH_DESTINATION) && (modifiers & (UI_ALT | UI_SUPER))) return true;
		if (ui->mode == UI_SSH_KEY) { connection_key_accept(ui, modifiers & UI_CONTROL); ui_selection(ui); return true; }
		if (ui->mode == UI_SOCKETS) { if (!(modifiers & (UI_ALT | UI_SUPER))) *request = socket_accept(ui, modifiers & UI_CONTROL); return true; }
		*request = ui->mode == UI_CONNECTIONS ? connection_accept(ui, modifiers & UI_CONTROL) : ui->mode == UI_SSH_DESTINATION ? ssh_accept(ui, modifiers & UI_CONTROL) : manager_accept(ui, context);
		return true;
	}
	if (ui->mode == UI_SESSIONS && (modifiers & UI_CONTROL) && sym == XKB_KEY_n) {
		json_object *message = operation(context->attached ? "session_new" : "attach");
		if (!context->attached) {
			json_object_object_add(message, "width", json_object_new_int(context->width));
			json_object_object_add(message, "height", json_object_new_int(context->height));
			json_object_object_add(message, "events", json_object_new_boolean(true));
		}
		*request = message; return true;
	}
	bool picker = ui->mode == UI_SOCKETS || ui->mode == UI_SESSIONS || ui->mode == UI_WINDOWS || ui->mode == UI_CONNECTIONS || ui->mode == UI_SSH_DESTINATION || ui->mode == UI_SSH_KEY || ui->mode == UI_ARRANGE;
	if (picker && sym == XKB_KEY_Up) {
		if (ui->selected > 0) ui->selected--;
		ui->repeatable = true;
	} else if (picker && sym == XKB_KEY_Down) {
		if (manager_item(ui, ui->selected + 1)) ui->selected++;
		ui->repeatable = true;
	} else if (picker && ui->normal && !ui->edit_operator && !(modifiers & (UI_CONTROL | UI_ALT | UI_SUPER))
			&& (sym == XKB_KEY_j || sym == XKB_KEY_k)) {
		if (sym == XKB_KEY_k && ui->selected > 0) ui->selected--;
		else if (sym == XKB_KEY_j && manager_item(ui, ui->selected + 1)) ui->selected++;
		ui->repeatable = true;
	} else if (ui->normal && !(modifiers & (UI_CONTROL | UI_ALT | UI_SUPER))) {
		if (ui->edit_operator) ui_operator(ui, sym);
		else switch (sym) {
		case XKB_KEY_c: case XKB_KEY_d: case XKB_KEY_y:
			ui->edit_operator = sym == XKB_KEY_c ? 'c' : sym == XKB_KEY_d ? 'd' : 'y'; break;
		case XKB_KEY_p: case XKB_KEY_P:
			if (*ui->yank) {
				size_t at = sym == XKB_KEY_p ? ui_next(ui, ui->cursor) : ui->cursor;
				if (ui_replace(ui, at, at, ui->yank)) ui->cursor = ui_previous(ui, ui->cursor);
			}
			break;
		case XKB_KEY_Y: ui_yank(ui, 0, strlen(ui->query)); break;
		case XKB_KEY_i: ui->normal = false; break;
		case XKB_KEY_a: ui->cursor = ui_next(ui, ui->cursor); ui->normal = false; break;
		case XKB_KEY_I: ui->cursor = 0; ui->normal = false; break;
		case XKB_KEY_A: ui->cursor = strlen(ui->query); ui->normal = false; break;
		case XKB_KEY_h: case XKB_KEY_Left: case XKB_KEY_BackSpace:
			ui->cursor = ui_previous(ui, ui->cursor); ui->repeatable = true; break;
		case XKB_KEY_l: case XKB_KEY_Right:
			ui->cursor = ui_next(ui, ui->cursor); ui->repeatable = true; break;
		case XKB_KEY_0: case XKB_KEY_Home: ui->cursor = 0; break;
		case XKB_KEY_dollar: case XKB_KEY_End: ui->cursor = strlen(ui->query); break;
		case XKB_KEY_w: case XKB_KEY_b:
			ui->cursor = ui_word(ui, sym == XKB_KEY_w); ui->repeatable = true; break;
		case XKB_KEY_e:
			ui->cursor = ui_previous(ui, ui_word_end(ui, ui_next(ui, ui->cursor))); ui->repeatable = true; break;
		case XKB_KEY_x: case XKB_KEY_Delete:
			ui_cut(ui, ui->cursor, ui_next(ui, ui->cursor), false); ui->repeatable = true; break;
		case XKB_KEY_X:
			ui_cut(ui, ui_previous(ui, ui->cursor), ui->cursor, false); ui->repeatable = true; break;
		case XKB_KEY_D: case XKB_KEY_C:
			ui_cut(ui, ui->cursor, strlen(ui->query), sym == XKB_KEY_C);
			break;
		case XKB_KEY_s:
			ui_cut(ui, ui->cursor, ui_next(ui, ui->cursor), true); break;
		default: break;
		}
	} else if (!ui->normal && ui->mode != UI_HELP &&
			(sym == XKB_KEY_BackSpace || ((modifiers & UI_CONTROL) && sym == XKB_KEY_h))) {
		if (ui->compose) xkb_compose_state_reset(ui->compose);
		ui_replace(ui, ui_previous(ui, ui->cursor), ui->cursor, "");
		ui->repeatable = true;
	} else if (!ui->normal && ui->mode != UI_HELP && sym == XKB_KEY_Delete) {
		ui_replace(ui, ui->cursor, ui_next(ui, ui->cursor), ""); ui->repeatable = true;
	} else if (!ui->normal && (modifiers & UI_CONTROL) && (sym == XKB_KEY_u || sym == XKB_KEY_w)) {
		if (ui->compose) xkb_compose_state_reset(ui->compose);
		ui_replace(ui, sym == XKB_KEY_u ? 0 : ui_word(ui, false), sym == XKB_KEY_u ? strlen(ui->query) : ui->cursor, "");
	} else if (!ui->normal && (sym == XKB_KEY_Left || sym == XKB_KEY_Right || sym == XKB_KEY_Home || sym == XKB_KEY_End)) {
		if (ui->compose) xkb_compose_state_reset(ui->compose);
		ui->cursor = sym == XKB_KEY_Home ? 0 : sym == XKB_KEY_End ? strlen(ui->query) :
			sym == XKB_KEY_Left ? ui_previous(ui, ui->cursor) : ui_next(ui, ui->cursor);
		ui->repeatable = true;
	} else if (!ui->normal && !(modifiers & (UI_CONTROL | UI_ALT | UI_SUPER)) && ui->mode != UI_HELP) {
		char text[64];
		int count = xkb_keysym_to_utf8(sym, text, sizeof(text));
		if (ui->compose) {
			xkb_compose_state_feed(ui->compose, sym);
			enum xkb_compose_status status = xkb_compose_state_get_status(ui->compose);
			if (status == XKB_COMPOSE_COMPOSING) return true;
			if (status == XKB_COMPOSE_COMPOSED) count = 1 + xkb_compose_state_get_utf8(ui->compose, text, sizeof(text));
			if (status == XKB_COMPOSE_CANCELLED) count = 0;
			if (status != XKB_COMPOSE_NOTHING) xkb_compose_state_reset(ui->compose);
		}
		if (count > 1 && count <= (int)sizeof(text) && (unsigned char)text[0] >= 32 && g_utf8_validate(text, -1, NULL)) {
			ui_replace(ui, ui->cursor, ui->cursor, text);
			ui->repeatable = true;
		}
	}
	if (ui->normal && ui->cursor == strlen(ui->query)) ui->cursor = ui_previous(ui, ui->cursor);
	ui_selection(ui);
	ui->error[0] = '\0';
	return true;
}

void manager_reply(struct manager_state *ui, const struct manager_context *context,
	json_object *message, bool requested) {
	if (requested && ui->route_cancel_pending) { ui->pending = false; return; }
	if (requested && json_object_get_boolean(field(message, "ok"))) ui->error[0] = '\0';
	json_object *state = field(message, "state"), *sessions = field(message, "sessions"), *help = field(message, "help");
	if (state) { json_object_put(ui->state); ui->state = json_object_get(state); }
	if (sessions) { json_object_put(ui->sessions); ui->sessions = json_object_get(sessions); }
	json_object *connections = field(message, "connections");
	json_object *keys = field(message, "ssh_keys");
	if (keys) { json_object_put(ui->ssh_keys); ui->ssh_keys = json_object_get(keys); }
	if (connections) {
		json_object_put(ui->connections); ui->connections = json_object_get(connections);
		if (requested && json_object_get_boolean(field(message, "ok"))) {
			json_object *selected = json_object_get(manager_item(ui, ui->selected));
			connection_load(ui, connections, field(message, "connection_bookmarks"));
			connection_matches(ui);
			json_object *identity = field(selected, "connection");
			bool fresh = identity && json_object_is_type(identity, json_type_int) && json_object_get_int64(identity) < 0;
			if (fresh && field(message, "connection_ids")) {
				char temporary[32]; snprintf(temporary, sizeof(temporary), "%" PRId64, json_object_get_int64(identity));
				json_object *assigned = field(field(message, "connection_ids"), temporary);
				if (assigned) { identity = assigned; fresh = false; }
			}
			for (size_t i = 0; selected && i < connection_count(ui->connection_matches); i++) {
				json_object *row = json_object_array_get_idx(ui->connection_matches, i);
				bool same = identity && json_object_equal(identity, field(row, "connection"));
				if (fresh) same = json_object_equal(field(selected, "name"), field(row, "name")) &&
					json_object_equal(field(selected, "address"), field(row, "address")) &&
					json_object_equal(field(selected, "kind"), field(row, "kind"));
				if (same) { ui->selected = i; break; }
			}
			json_object_put(selected);
		}
	}
	json_object *targets = field(message, "ssh_targets");
	json_object *sockets = field(message, "sockets");
	if (sockets && requested && json_object_get_boolean(field(message, "ok"))) {
		bool opening = ui->mode != UI_SOCKETS;
		json_object_put(ui->sockets); ui->sockets = json_object_get(sockets);
		if (field(message, "route_name")) snprintf(ui->route_name, sizeof(ui->route_name), "%s", json_object_get_string(field(message, "route_name")));
		if (field(message, "route_identity")) snprintf(ui->route_identity, sizeof(ui->route_identity), "%s", json_object_get_string(field(message, "route_identity")));
		if (opening) { ui->query[0] = '\0'; ui->cursor = 0; ui->normal = false; ui->selected = ui->first = 0; }
	}
	if (targets) { json_object_put(ui->ssh_targets); ui->ssh_targets = json_object_get(targets); }
	if (help) { free(ui->help); ui->help = strdup(json_object_get_string(help)); }
	if (field(message, "warning")) snprintf(ui->error, sizeof(ui->error), "%s", json_object_get_string(field(message, "warning")));
	if (field(message, "error")) snprintf(ui->error, sizeof(ui->error), "%s", json_object_get_string(field(message, "error")));
	if (requested) {
		ui->pending = false;
		if (!json_object_get_boolean(field(message, "ok"))) snprintf(ui->error, sizeof(ui->error), "%s", json_object_get_string(field(message, "error")));
		else if (field(message, "arrangement") && (ui->mode == UI_ARRANGE || ui->mode == UI_ARRANGE_NAME)) {
			size_t cursor = ui->row_cursor;
			arrangement_load(ui, field(message, "arrangement")); ui->mode = UI_ARRANGE;
			ui->row_cursor = cursor;
		}
		else if (sockets) ui->mode = UI_SOCKETS;
		else if (connections) ui->mode = UI_CONNECTIONS;
		else if (targets) ui->mode = UI_SSH_DESTINATION;
		else if (field(message, "route_cancelled")) {                                         }
		else if (sessions) {
			if (ui->mode == UI_SOCKETS || ui->mode == UI_SSH_DESTINATION || ui->mode == UI_CONNECTIONS) {
				ui->query[0] = '\0'; ui->cursor = 0; ui->normal = false;
			}
			ui->mode = UI_SESSIONS;
		}
		else if (help) ui->mode = UI_HELP;
		else if (!(ui->mode == UI_ARRANGE && ui->opening_window && state)) manager_close(ui, context);
	}
	ui_selection(ui);
}

json_object *manager_followup(struct manager_state *ui, const struct manager_context *context) {
	if (ui->pending) return NULL;
	if (ui->route_cancel_pending) { ui->route_cancel_pending = false; return operation("route_cancel"); }
	if (ui->mode == UI_SESSIONS) return manager_open(ui, UI_SESSIONS, context);
	if (ui->mode != UI_ARRANGE || !ui->opening_window) return NULL;
	int64_t identity = ui->opening_window;
	ui->opening_window = 0;
	if (*ui->error) return NULL;
	json_object *windows = field(ui->state, "windows");
	for (size_t i = 0; windows && i < json_object_array_length(windows); i++) {
		json_object *window = json_object_array_get_idx(windows, i);
		if (json_object_get_int64(field(window, "window")) == identity) {
			json_object *request = operation("window_select");
			json_object_object_add(request, "number", json_object_get(field(window, "number")));
			return request;
		}
	}
	snprintf(ui->error, sizeof(ui->error), "Window changed; reload with Ctrl-r");
	return NULL;
}

void manager_finish(struct manager_state *ui) {
	arrangement_clear(ui);
	connection_clear(ui);
	xkb_compose_state_unref(ui->compose);
	json_object_put(ui->state);
	json_object_put(ui->sessions);
	json_object_put(ui->connections);
	json_object_put(ui->ssh_targets);
	json_object_put(ui->ssh_matches);
	json_object_put(ui->ssh_keys);
	json_object_put(ui->sockets);
	json_object_put(ui->socket_matches);
	free(ui->help);
}
