#ifndef AMUX_MANAGER_STATE_H
#define AMUX_MANAGER_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <json-c/json.h>
#include <xkbcommon/xkbcommon-compose.h>

enum ui_mode { UI_NONE, UI_WORKSPACES, UI_SESSIONS, UI_COMMAND, UI_SESSION_NAME,
	UI_WORKSPACE_NAME, UI_PANE_NAME, UI_HELP, UI_CONNECTIONS };
enum ui_modifiers { UI_CONTROL = 1, UI_ALT = 2, UI_SUPER = 4 };

struct manager_state {
	struct xkb_compose_state *compose;
	json_object *state, *sessions, *connections;
	enum ui_mode mode;
	int selected, first;
	size_t cursor;
	char edit_operator;
	bool pending, normal, repeatable;
	char query[512], error[256], yank[512];
	char *help;
};

struct manager_context {
	bool attached, connected, defer_attach;
	int width, height;
};

bool manager_rename(const struct manager_state *ui);
json_object *manager_workspace(const struct manager_state *ui);
bool manager_session_empty(const struct manager_state *ui);
json_object *manager_item(const struct manager_state *ui, int index);
json_object *manager_open(struct manager_state *ui, enum ui_mode mode, const struct manager_context *context);
void manager_close(struct manager_state *ui, const struct manager_context *context);
json_object *manager_accept(struct manager_state *ui, const struct manager_context *context);
bool manager_key(struct manager_state *ui, const struct manager_context *context,
	xkb_keysym_t sym, unsigned modifiers, json_object **request);
void manager_reply(struct manager_state *ui, const struct manager_context *context,
	json_object *message, bool requested);
void manager_finish(struct manager_state *ui);

#endif
