#define STATUS_HEIGHT 28
#include "manager-state.h"

struct manager_ui {
	struct manager_state model;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *status, *panel;
	int panel_x, panel_y, panel_width, status_width;
	int session_width, tabs_first, tabs_count, tab_width;
	bool button;
	char *status_text;
};

struct frontend;
static void ui_draw(struct frontend *client);
static void ui_open(struct frontend *client, enum ui_mode mode);
static void ui_close(struct frontend *client);
static bool ui_key(struct frontend *client, xkb_keysym_t sym, uint32_t modifiers);
static bool ui_pointer(struct frontend *client, double x, double y);
static void ui_reply(struct frontend *client, json_object *message, bool requested);
static void ui_finish(struct frontend *client);
