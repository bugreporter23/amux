#include "manager-state.h"
#include <inttypes.h>
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
	return ui->mode == UI_SESSION_NAME || ui->mode == UI_WORKSPACE_NAME || ui->mode == UI_PANE_NAME;
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

json_object *manager_workspace(const struct manager_state *ui) {
	json_object *workspaces = field(ui->state, "workspaces");
	if (!workspaces) return NULL;
	for (size_t i = 0; i < json_object_array_length(workspaces); i++) {
		json_object *workspace = json_object_array_get_idx(workspaces, i);
		if (json_object_get_int64(field(workspace, "workspace")) == json_object_get_int64(field(ui->state, "active_workspace")))
			return workspace;
	}
	return NULL;
}

static json_object *ui_items(const struct manager_state *ui) {
	return ui->mode == UI_CONNECTIONS ? ui->connections : ui->mode == UI_SESSIONS ? ui->sessions : field(ui->state, "workspaces");
}

bool manager_session_empty(const struct manager_state *ui) {
	json_object *workspaces = field(ui->state, "workspaces");
	return workspaces && !json_object_array_length(workspaces);
}

static json_object *ui_pane(const struct manager_state *ui) {
	json_object *panes = field(manager_workspace(ui), "panes");
	for (size_t i = 0; panes && i < json_object_array_length(panes); i++) {
		json_object *pane = json_object_array_get_idx(panes, i);
		if (json_object_get_int64(field(pane, "id")) == json_object_get_int64(field(manager_workspace(ui), "focus"))) return pane;
	}
	return NULL;
}

json_object *manager_item(const struct manager_state *ui, int index) {
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

static void ui_selection(struct manager_state *ui) {
	bool picker = ui->mode == UI_SESSIONS || ui->mode == UI_WORKSPACES || ui->mode == UI_CONNECTIONS;
	if (picker && !manager_item(ui, ui->selected)) ui->selected = 0;
	if (ui->selected < ui->first) ui->first = ui->selected;
	if (ui->selected >= ui->first + 8) ui->first = ui->selected - 7;
}

void manager_close(struct manager_state *ui, const struct manager_context *context) {
	if (ui->compose) xkb_compose_state_reset(ui->compose);
	ui->mode = UI_NONE;
	if (!context->attached && context->defer_attach) ui->mode = context->connected ? UI_SESSIONS : UI_CONNECTIONS;
}

json_object *manager_open(struct manager_state *ui, enum ui_mode mode, const struct manager_context *context) {
	if (mode == UI_SESSIONS && !context->connected) mode = UI_CONNECTIONS;
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
	ui->edit_operator = 0;
	ui->selected = ui->first = 0;
	ui->query[0] = ui->error[0] = '\0';
	const char *name = NULL;
	if (mode == UI_SESSION_NAME) name = json_object_get_string(field(ui->state, "name"));
	if (mode == UI_WORKSPACE_NAME) name = json_object_get_string(field(manager_workspace(ui), "name"));
	if (mode == UI_PANE_NAME) {
		name = json_object_get_string(field(ui_pane(ui), "name"));
	}
	if (name) snprintf(ui->query, sizeof(ui->query), "%s", name);
	const char *valid_end;
	if (!g_utf8_validate(ui->query, -1, &valid_end)) ui->query[valid_end - ui->query] = '\0';
	ui->cursor = strlen(ui->query);
	if (mode == UI_SESSIONS) return operation("list");
	if (mode == UI_CONNECTIONS) return operation("connections");
	if (mode == UI_HELP) return operation("help");
	return NULL;
}

json_object *manager_accept(struct manager_state *ui, const struct manager_context *context) {
	if (!ui->mode || ui->pending) return NULL;
	json_object *message;
	if (ui->mode == UI_WORKSPACES || ui->mode == UI_SESSIONS || ui->mode == UI_CONNECTIONS) {
		json_object *item = manager_item(ui, ui->selected);
		if (!item) return NULL;
		bool session = ui->mode == UI_SESSIONS;
		message = operation(ui->mode == UI_CONNECTIONS ? "connection_select" : session ? (context->attached ? "session_select" : "attach") : "workspace_select");
		const char *id = ui->mode == UI_CONNECTIONS ? "connection" : session ? "session" : "number";
		json_object_object_add(message, id, json_object_get(field(item, id)));
		if (session && !context->attached) {
			json_object_object_add(message, "width", json_object_new_int(context->width));
			json_object_object_add(message, "height", json_object_new_int(context->height));
			json_object_object_add(message, "events", json_object_new_boolean(true));
		}
	} else if (ui->mode == UI_HELP) { manager_close(ui, context); return NULL; }
	else {
		const char *op = ui->mode == UI_COMMAND ? "command" : ui->mode == UI_SESSION_NAME ? "session_rename" : ui->mode == UI_WORKSPACE_NAME ? "workspace_rename" : "pane_rename";
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
	if ((modifiers & UI_CONTROL) && sym == XKB_KEY_g) { manager_close(ui, context); return true; }
	if (ui->pending) return true;
	if ((modifiers & UI_CONTROL) && sym == XKB_KEY_o && ui->mode != UI_CONNECTIONS) {
		*request = manager_open(ui, UI_CONNECTIONS, context); return true;
	}
	if (sym == XKB_KEY_Escape) {
		if (manager_rename(ui) && !ui->normal) {
			ui->normal = true;
			ui->cursor = ui_previous(ui, ui->cursor);
			if (ui->compose) xkb_compose_state_reset(ui->compose);
		} else manager_close(ui, context);
		return true;
	}
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { *request = manager_accept(ui, context); return true; }
	if (ui->mode == UI_SESSIONS && (modifiers & UI_CONTROL) && sym == XKB_KEY_n) {
		json_object *message = operation(context->attached ? "session_new" : "attach");
		if (!context->attached) {
			json_object_object_add(message, "width", json_object_new_int(context->width));
			json_object_object_add(message, "height", json_object_new_int(context->height));
			json_object_object_add(message, "events", json_object_new_boolean(true));
		}
		*request = message; return true;
	}
	bool picker = ui->mode == UI_SESSIONS || ui->mode == UI_WORKSPACES || ui->mode == UI_CONNECTIONS;
	if (picker && sym == XKB_KEY_Up) {
		if (ui->selected > 0) ui->selected--;
		ui->repeatable = true;
	} else if (picker && sym == XKB_KEY_Down) {
		if (manager_item(ui, ui->selected + 1)) ui->selected++;
		ui->repeatable = true;
	} else if (ui->normal && !(modifiers & (UI_CONTROL | UI_ALT | UI_SUPER))) {
		if (sym == XKB_KEY_q) { manager_close(ui, context); return true; }
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
	json_object *state = field(message, "state"), *sessions = field(message, "sessions"), *help = field(message, "help");
	if (state) { json_object_put(ui->state); ui->state = json_object_get(state); }
	if (sessions) { json_object_put(ui->sessions); ui->sessions = json_object_get(sessions); }
	json_object *connections = field(message, "connections");
	if (connections) { json_object_put(ui->connections); ui->connections = json_object_get(connections); }
	if (help) { free(ui->help); ui->help = strdup(json_object_get_string(help)); }
	if (field(message, "error")) snprintf(ui->error, sizeof(ui->error), "%s", json_object_get_string(field(message, "error")));
	if (requested) {
		ui->pending = false;
		if (!json_object_get_boolean(field(message, "ok"))) snprintf(ui->error, sizeof(ui->error), "%s", json_object_get_string(field(message, "error")));
		else if (connections) ui->mode = UI_CONNECTIONS;
		else if (sessions) ui->mode = UI_SESSIONS;
		else if (help) ui->mode = UI_HELP;
		else manager_close(ui, context);
	}
	ui_selection(ui);
}

void manager_finish(struct manager_state *ui) {
	xkb_compose_state_unref(ui->compose);
	json_object_put(ui->state);
	json_object_put(ui->sessions);
	json_object_put(ui->connections);
	free(ui->help);
}
