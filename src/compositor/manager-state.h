#ifndef AMUX_MANAGER_STATE_H
#define AMUX_MANAGER_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <json-c/json.h>
#include <xkbcommon/xkbcommon-compose.h>

enum ui_mode { UI_NONE, UI_WINDOWS, UI_SESSIONS, UI_COMMAND, UI_SESSION_NAME,
	UI_WINDOW_NAME, UI_PANE_NAME, UI_HELP, UI_CONNECTIONS, UI_SSH_DESTINATION,
	UI_ARRANGE, UI_ARRANGE_NAME, UI_ARRANGE_REVIEW, UI_CONNECTION_NAME, UI_CONNECTION_REVIEW, UI_SSH_KEY, UI_SOCKETS };
enum ui_modifiers { UI_CONTROL = 1, UI_ALT = 2, UI_SUPER = 4 };
#define MANAGER_REVIEW_ROWS 16

struct manager_state {
	struct xkb_compose_state *compose;
	json_object *state, *sessions, *connections;
	json_object *ssh_targets, *ssh_matches;
	json_object *sockets, *socket_matches;
	json_object *ssh_keys, *key_matches, *connection_key_target;
	int connection_key_selection;
	json_object *connection_base, *connection_rows, *connection_matches, *connection_undo, *connection_edit_base;
	json_object *connection_request, *connection_review;
	int64_t next_connection_id;
	int connection_field, connection_selection;
	bool connection_inserting, route_cancel_pending;
	json_object *arrangement_base, *arrangement_rows, *cut_window, *arrangement_undo, *cut_undo;
	json_object *insertion_base;
	json_object *arrangement_request, *arrangement_changes;
	json_object *arrangement_review_rows;
	int64_t opening_window, next_draft_id;
	enum ui_mode mode;
	int selected, first, insertion_selection;
	int review_selection, review_first;
	size_t cursor, row_cursor;
	char edit_operator;
	bool pending, normal, repeatable, arrangement_inserting;
	char query[512], error[256], yank[512];
	char ssh_destination[512];
	char route_name[512], route_identity[512];
	char *help;
};

struct manager_context {
	bool attached, connected, defer_attach;
	int width, height;
};

bool manager_rename(const struct manager_state *ui);
json_object *manager_window(const struct manager_state *ui);
bool manager_session_empty(const struct manager_state *ui);
json_object *manager_item(const struct manager_state *ui, int index);
json_object *manager_open(struct manager_state *ui, enum ui_mode mode, const struct manager_context *context);
void manager_close(struct manager_state *ui, const struct manager_context *context);
json_object *manager_accept(struct manager_state *ui, const struct manager_context *context);
json_object *manager_followup(struct manager_state *ui, const struct manager_context *context);
bool manager_arrangement_dirty(const struct manager_state *ui);
bool manager_connection_dirty(const struct manager_state *ui);
json_object *manager_arrangement_session(const struct manager_state *ui, int index);
bool manager_key(struct manager_state *ui, const struct manager_context *context,
	xkb_keysym_t sym, unsigned modifiers, json_object **request);
void manager_reply(struct manager_state *ui, const struct manager_context *context,
	json_object *message, bool requested);
void manager_finish(struct manager_state *ui);

#endif
