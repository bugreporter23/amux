#include <cairo.h>
#include <drm_fourcc.h>
#include <pango/pangocairo.h>
#include <wlr/interfaces/wlr_buffer.h>

struct ui_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void ui_buffer_destroy(struct wlr_buffer *base) {
	struct ui_buffer *buffer = wl_container_of(base, buffer, base);
	cairo_surface_destroy(buffer->surface);
	free(buffer);
}

static bool ui_buffer_data(struct wlr_buffer *base, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	struct ui_buffer *buffer = wl_container_of(base, buffer, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
	*data = cairo_image_surface_get_data(buffer->surface);
	*stride = cairo_image_surface_get_stride(buffer->surface);
	*format = DRM_FORMAT_ARGB8888;
	return true;
}

static void ui_buffer_end(struct wlr_buffer *base) {}
static const struct wlr_buffer_impl ui_buffer_impl = {
	.destroy = ui_buffer_destroy, .begin_data_ptr_access = ui_buffer_data,
	.end_data_ptr_access = ui_buffer_end,
};

static const double UI_BAR[3] = {.071, .086, .102}, UI_SURFACE[3] = {.090, .110, .130},
	UI_FIELD[3] = {.055, .068, .080}, UI_RAISED[3] = {.125, .160, .195}, UI_EDGE[3] = {.180, .220, .260},
	UI_BRIGHT[3] = {.820, .850, .870}, UI_MUTED[3] = {.470, .530, .580}, UI_BLUE[3] = {.530, .710, .820},
	UI_DARK[3] = {.060, .070, .080};
#define UI_RGB(c) (c)[0], (c)[1], (c)[2]
#define UI_MARKUP_BRIGHT "#d1d9de"
#define UI_MARKUP_MUTED "#78878f"
#define UI_MARKUP_BLUE "#87b5d1"

static struct ui_buffer *ui_canvas(int width, int height, cairo_t **cr) {
	struct ui_buffer *buffer = calloc(1, sizeof(*buffer));
	wlr_buffer_init(&buffer->base, &ui_buffer_impl, width, height);
	buffer->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	*cr = cairo_create(buffer->surface);
	cairo_set_source_rgb(*cr, UI_RGB(UI_BAR));
	cairo_paint(*cr);
	return buffer;
}

static void ui_rounded(cairo_t *cr, double x, double y, double width, double height, double radius) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + width - radius, y + radius, radius, -G_PI / 2, 0);
	cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, G_PI / 2);
	cairo_arc(cr, x + radius, y + height - radius, radius, G_PI / 2, G_PI);
	cairo_arc(cr, x + radius, y + radius, radius, G_PI, 3 * G_PI / 2);
	cairo_close_path(cr);
}

static struct ui_buffer *ui_panel_canvas(int width, int height, cairo_t **cr) {
	struct ui_buffer *buffer = ui_canvas(width, height, cr);
	cairo_set_operator(*cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(*cr);
	cairo_set_operator(*cr, CAIRO_OPERATOR_OVER);
	ui_rounded(*cr, .5, .5, width - 1, height - 1, 10);
	cairo_set_source_rgb(*cr, UI_RGB(UI_SURFACE));
	cairo_fill_preserve(*cr);
	cairo_set_source_rgb(*cr, UI_RGB(UI_EDGE));
	cairo_set_line_width(*cr, 1);
	cairo_stroke(*cr);
	return buffer;
}

enum ui_ink { UI_NORMAL, UI_ACCENT, UI_INVERTED, UI_SUBDUED };

static PangoLayout *ui_layout(cairo_t *cr, const char *text, int width) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *font = pango_font_description_from_string("monospace 11");
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);
	pango_layout_set_text(layout, text ? text : "", -1);
	pango_layout_set_single_paragraph_mode(layout, width < 0);
	pango_layout_set_width(layout, width < 0 ? -1 : (width > 0 ? width : 1) * PANGO_SCALE);
	if (width >= 0) pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	return layout;
}

static void ui_text_aligned(cairo_t *cr, const char *text, int x, int y, int width,
		enum ui_ink ink, PangoAlignment alignment) {
	PangoLayout *layout = ui_layout(cr, text, width);
	pango_layout_set_alignment(layout, alignment);
	const double *colors[] = {UI_BRIGHT, UI_BLUE, UI_DARK, UI_MUTED};
	cairo_set_source_rgb(cr, UI_RGB(colors[ink]));
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

static void ui_text(cairo_t *cr, const char *text, int x, int y, int width, enum ui_ink ink) {
	ui_text_aligned(cr, text, x, y, width, ink, PANGO_ALIGN_LEFT);
}

static void ui_markup(cairo_t *cr, const char *markup, int x, int y, int width, PangoAlignment alignment) {
	PangoLayout *layout = ui_layout(cr, "", width);
	pango_layout_set_markup(layout, markup, -1);
	pango_layout_set_alignment(layout, alignment);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

static void ui_hint(cairo_t *cr, const char *hint, int x, int y, int width, bool error) {
	GString *markup = g_string_new(NULL);
	gchar **parts = g_strsplit(hint, "  ", -1);
	for (gchar **part = parts; *part; part++) {
		if (!**part) continue;
		if (markup->len) g_string_append(markup, "   ");
		char *colon = error ? NULL : strstr(*part, ": ");
		gchar *escaped;
		if (!error && part == parts && (!strcmp(*part, "INSERT") || !strcmp(*part, "NORMAL"))) {
			escaped = g_markup_escape_text(*part, -1);
			g_string_append_printf(markup, "<span foreground='" UI_MARKUP_BLUE "' weight='bold'>%s</span>", escaped);
		} else if (colon) {
			gchar *key = g_markup_escape_text(*part, colon - *part);
			escaped = g_markup_escape_text(colon + 2, -1);
			g_string_append_printf(markup, "<span foreground='" UI_MARKUP_BRIGHT "'>%s</span> "
				"<span foreground='" UI_MARKUP_MUTED "'>%s</span>", key, escaped);
			g_free(key);
		} else {
			escaped = g_markup_escape_text(*part, -1);
			g_string_append_printf(markup, "<span foreground='%s'>%s</span>", error ? UI_MARKUP_BRIGHT : UI_MARKUP_MUTED, escaped);
		}
		g_free(escaped);
	}
	g_strfreev(parts);
	PangoFontDescription *font = pango_font_description_from_string("monospace 9.5");
	PangoLayout *layout = ui_layout(cr, "", width);
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);
	pango_layout_set_markup(layout, markup->str, -1);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	g_string_free(markup, true);
}

static void ui_edit_draw(cairo_t *cr, struct manager_ui *ui, int width) {
	ui_rounded(cr, 8, 30, width - 16, 30, 6);
	cairo_set_source_rgb(cr, UI_RGB(UI_FIELD));
	cairo_fill(cr);
	ui_text(cr, "›", 15, 35, 16, UI_ACCENT);
	PangoLayout *layout = ui_layout(cr, ui->model.query, -1);
	PangoRectangle cursor, weak;
	if (ui->model.normal) pango_layout_index_to_pos(layout, ui->model.cursor, &cursor);
	else pango_layout_get_cursor_pos(layout, ui->model.cursor, &cursor, &weak);
	int available = width > 44 ? width - 44 : 1;
	double x = (double)cursor.x / PANGO_SCALE;
	double offset = x > available - 12 ? x - available + 12 : 0;
	double height = (double)cursor.height / PANGO_SCALE;
	cairo_save(cr);
	cairo_rectangle(cr, 32, 33, available, 24);
	cairo_clip(cr);
	cairo_set_source_rgb(cr, UI_RGB(UI_BRIGHT));
	cairo_move_to(cr, 32 - offset, 35);
	pango_cairo_show_layout(cr, layout);
	cairo_set_source_rgb(cr, UI_RGB(UI_BLUE));
	double left = 32 + x - offset, top = 35 + (double)cursor.y / PANGO_SCALE;
	if (ui->model.normal) {
		double span = (double)cursor.width / PANGO_SCALE;
		if (span < 0) { left += span; span = -span; }
		if (span <= 0) {
			PangoLayout *cell = ui_layout(cr, "M", -1);
			int cell_width;
			pango_layout_get_pixel_size(cell, &cell_width, NULL);
			g_object_unref(cell);
			span = cell_width;
		}
		cairo_rectangle(cr, left, top, span, height);
		cairo_fill_preserve(cr);
		cairo_clip(cr);
		cairo_set_source_rgb(cr, UI_RGB(UI_DARK));
		cairo_move_to(cr, 32 - offset, 35);
		pango_cairo_show_layout(cr, layout);
	} else {
		cairo_rectangle(cr, left - 1, top, 2, height);
		cairo_fill(cr);
	}
	cairo_restore(cr);
	g_object_unref(layout);
}

static void ui_upload(struct frontend *client, struct wlr_scene_buffer **node,
		struct ui_buffer *buffer, cairo_t *cr, int x, int y) {
	cairo_destroy(cr);
	cairo_surface_flush(buffer->surface);
	if (*node) wlr_scene_buffer_set_buffer(*node, &buffer->base);
	else *node = wlr_scene_buffer_create(client->ui.tree, &buffer->base);
	wlr_scene_node_set_position(&(*node)->node, x, y);
	wlr_scene_node_set_enabled(&(*node)->node, true);
	wlr_buffer_drop(&buffer->base);
}

static json_object *previous_window(struct frontend *client);

static void ui_zoom_marks(cairo_t *cr, double x, double width) {
	double inset = 3, arm = 6, top = 3, bottom = STATUS_HEIGHT - 6;
	cairo_set_source_rgb(cr, UI_RGB(UI_BLUE));
	cairo_set_line_width(cr, 1.5);
	double corners[][4] = {{x + inset, top, 1, 1}, {x + width - inset, top, -1, 1},
		{x + inset, bottom, 1, -1}, {x + width - inset, bottom, -1, -1}};
	for (size_t i = 0; i < 4; i++) {
		cairo_move_to(cr, corners[i][0], corners[i][1] + corners[i][3] * arm);
		cairo_line_to(cr, corners[i][0], corners[i][1]);
		cairo_line_to(cr, corners[i][0] + corners[i][2] * arm, corners[i][1]);
	}
	cairo_stroke(cr);
}

static void ui_draw(struct frontend *client) {
	struct manager_ui *ui = &client->ui;
	if (!ui->tree) return;
	json_object *windows = field(ui->model.state, "windows");
	struct pane *focused = find_pane(client, client->approved_focus);
	const char *mode = focused && focused->presentation == VIEW_FAILED ? "View unavailable: view retry" :
		!client->debug ? "" : ui->model.mode ? "MANAGER" : client->resizing ? "RESIZE" : client->prefix ? "PREFIX" : "";
	const char *session = ui->model.state ? json_object_get_string(field(ui->model.state, "name")) : client->defer_attach ? (*client->server_path ? "Choose a session" : "Choose a connection") : "attaching";
	char notice[512];
	snprintf(notice, sizeof(notice), "%s%s%s",
		*ui->model.error ? ui->model.error : mode,
		client->debug && client->waiting ? "  [waiting]" : "", client->debug && ui->model.pending ? "  [selecting]" : "");
	int tint = client->resizing ? 2 : client->prefix ? 1 : 0;
	json_object *previous = previous_window(client);
	int64_t previous_id = previous ? json_object_get_int64(field(previous, "window")) : 0;
	char *status = g_strdup_printf("%s|%s|%d|%" PRId64 "|%" PRId64 "|%s", session, notice, tint,
		client->active_window, previous_id, json_object_to_json_string_ext(windows, JSON_C_TO_STRING_PLAIN));
	if (!ui->status_text || strcmp(status, ui->status_text) || !ui->status || ui->status_width != client->width) {
		ui->status_width = client->width;
		free(ui->status_text);
		ui->status_text = status;
		cairo_t *cr;
		struct ui_buffer *buffer = ui_canvas(client->width, STATUS_HEIGHT, &cr);
		if (tint) {
			if (tint == 1) cairo_set_source_rgb(cr, .12, .19, .31);
			else cairo_set_source_rgb(cr, .30, .21, .08);
			cairo_paint(cr);
		}
		cairo_set_source_rgb(cr, UI_RGB(UI_EDGE));
		cairo_rectangle(cr, 0, STATUS_HEIGHT - 1, client->width, 1);
		cairo_fill(cr);
		ui->session_width = client->width / 4;
		if (ui->session_width > 240) ui->session_width = 240;
		int notice_width = client->width / 4;
		if (notice_width > 220) notice_width = 220;
		int session_x = client->width - ui->session_width;
		int end = session_x - notice_width;
		int available = end;
		int count = windows ? json_object_array_length(windows) : 0;
		bool overflow = count && available / count < 100;
		if (overflow) available = available > 28 ? available - 28 : 0;
		ui->tabs_count = available / 100;
		if (ui->tabs_count < 1) ui->tabs_count = 1;
		if (ui->tabs_count > count) ui->tabs_count = count;
		ui->tabs_first = 0;
		for (int i = 0; i < count; i++) {
			json_object *window = json_object_array_get_idx(windows, i);
			if (json_object_get_int64(field(window, "window")) == client->active_window && i >= ui->tabs_count)
				ui->tabs_first = i - ui->tabs_count + 1;
		}
		ui->tab_width = ui->tabs_count ? available / ui->tabs_count : 0;
		if (ui->tab_width > 180) ui->tab_width = 180;
		gchar *escaped_session = g_markup_escape_text(session, -1);
		char *session_label = g_strdup_printf("<span foreground='" UI_MARKUP_MUTED "'>session</span> "
			"<span foreground='" UI_MARKUP_BLUE "'>%s</span>", escaped_session);
		ui_markup(cr, session_label, session_x + 9, 5, ui->session_width - 18, PANGO_ALIGN_RIGHT);
		g_free(session_label);
		g_free(escaped_session);
		for (int i = 0; i < ui->tabs_count && ui->tab_width > 0; i++) {
			json_object *window = json_object_array_get_idx(windows, ui->tabs_first + i);
			int64_t id = json_object_get_int64(field(window, "window"));
			bool active = id == client->active_window;
			int x = i * ui->tab_width;
			if (active) {
				cairo_set_source_rgb(cr, UI_RGB(UI_RAISED));
				cairo_rectangle(cr, x, 0, ui->tab_width, STATUS_HEIGHT - 1);
				cairo_fill(cr);
				cairo_set_source_rgb(cr, UI_RGB(UI_BLUE));
				cairo_rectangle(cr, x, STATUS_HEIGHT - 3, ui->tab_width, 2);
				cairo_fill(cr);
				if (json_object_get_boolean(field(window, "zoomed"))) ui_zoom_marks(cr, x, ui->tab_width);
			} else if (id == previous_id) {
				cairo_set_source_rgb(cr, UI_BLUE[0] * .55, UI_BLUE[1] * .55, UI_BLUE[2] * .55);
				cairo_rectangle(cr, x + 6, STATUS_HEIGHT - 2, ui->tab_width - 12, 1);
				cairo_fill(cr);
			}
			json_object *panes = field(window, "panes");
			bool attention = false;
			for (size_t p = 0; panes && p < json_object_array_length(panes); p++) {
				json_object *flag = field(json_object_array_get_idx(panes, p), "attention");
				attention |= flag && json_object_get_boolean(flag);
			}
			if (attention) {
				cairo_set_source_rgb(cr, .86, .66, .32);
				cairo_arc(cr, x + ui->tab_width - 9, STATUS_HEIGHT / 2.0 - 1, 3, 0, 2 * G_PI);
				cairo_fill(cr);
			}
			gchar *name = g_markup_escape_text(json_object_get_string(field(window, "name")), -1);
			char *label = g_strdup_printf("<span foreground='%s'>%" PRId64 "</span> <span foreground='%s'>%s</span>",
				active ? UI_MARKUP_BLUE : UI_MARKUP_MUTED, json_object_get_int64(field(window, "number")),
				active ? UI_MARKUP_BRIGHT : UI_MARKUP_MUTED, name);
			ui_markup(cr, label, x + 10, 5, ui->tab_width - (attention ? 28 : 20), PANGO_ALIGN_LEFT);
			g_free(label);
			g_free(name);
		}
		if (overflow) ui_text(cr, "…", end - 28, 5, 28, UI_SUBDUED);
		ui_text(cr, notice, end + 9, 5, notice_width - 18, UI_SUBDUED);
		ui_upload(client, &ui->status, buffer, cr, 0, 0);
	} else free(status);
	int width = client->width > 680 ? 640 : client->width > 40 ? client->width - 20 : client->width;
	if (!ui->model.mode) {
		if (manager_session_empty(&client->ui.model) && !client->prefix && !client->resizing) {
			int height = client->height - STATUS_HEIGHT;
			if (height > 86) height = 86;
			if (height < 1) height = 1;
			cairo_t *cr;
			struct ui_buffer *buffer = ui_panel_canvas(width, height, &cr);
			ui_text(cr, "Session is empty", 16, 14, width - 32, UI_NORMAL);
			ui_hint(cr, client->waiting || !wl_list_empty(&client->requests) ? "Waiting for server" :
				"q: delete this session (no prefix)", 16, 44, width - 32, false);
			ui_upload(client, &ui->panel, buffer, cr,
				(client->width - width) / 2, STATUS_HEIGHT + (client->height - STATUS_HEIGHT - height) / 3);
		} else if (ui->panel) wlr_scene_node_set_enabled(&ui->panel->node, false);
		return;
	}
	bool picker = ui->model.mode == UI_WINDOWS || ui->model.mode == UI_SESSIONS || ui->model.mode == UI_CONNECTIONS;
	int rows = 0;
	while (picker && rows < 8 && manager_item(&ui->model, ui->model.first + rows)) rows++;
	int height = ui->model.mode == UI_HELP ? 240 : picker ? 96 + rows * 26 : 110;
	if (height > client->height - STATUS_HEIGHT) height = client->height - STATUS_HEIGHT;
	if (height < 1) height = 1;
	cairo_t *cr;
	struct ui_buffer *buffer = ui_panel_canvas(width, height, &cr);
	static const char *titles[] = {"", "WINDOWS", "SESSIONS", "COMMAND", "SESSION NAME", "WINDOW NAME", "PANE NAME", "COMMANDS", "CONNECTIONS"};
	char *title = g_strdup_printf("<span foreground='" UI_MARKUP_MUTED "' weight='bold' size='small' "
		"letter_spacing='1200'>%s</span>", titles[ui->model.mode]);
	ui_markup(cr, title, 14, 9, width - 28, PANGO_ALIGN_LEFT);
	g_free(title);
	if (ui->model.mode == UI_HELP) ui_text(cr, ui->model.help, 14, 38, width - 28, UI_NORMAL);
	else {
		ui_edit_draw(cr, ui, width);
		for (int row = 0; row < rows; row++) {
			json_object *item = manager_item(&ui->model, ui->model.first + row);
			bool selected = ui->model.first + row == ui->model.selected;
			if (selected) {
				ui_rounded(cr, 8, 64 + row * 26, width - 16, 24, 5);
				cairo_set_source_rgb(cr, UI_RGB(UI_RAISED));
				cairo_fill(cr);
				ui_rounded(cr, 11, 70 + row * 26, 3, 12, 1.5);
				cairo_set_source_rgb(cr, UI_RGB(UI_BLUE));
				cairo_fill(cr);
			}
			const char *id = ui->model.mode == UI_CONNECTIONS ? "connection" : ui->model.mode == UI_SESSIONS ? "session" : "number";
			const char *tag = ui->model.mode == UI_CONNECTIONS ? (json_object_get_boolean(field(item, "remote")) ? "remote" : "local") :
				ui->model.mode == UI_SESSIONS && json_object_get_boolean(field(item, "attached")) ? "attached" : NULL;
			gchar *name = g_markup_escape_text(json_object_get_string(field(item, "name")), -1);
			char *label = g_strdup_printf("<span foreground='%s'>%" PRId64 "</span>  <span foreground='%s'>%s</span>%s%s%s",
				selected ? UI_MARKUP_BLUE : UI_MARKUP_MUTED, json_object_get_int64(field(item, id)),
				UI_MARKUP_BRIGHT, name,
				tag ? "  <span foreground='" UI_MARKUP_MUTED "' size='small'>" : "", tag ? tag : "", tag ? "</span>" : "");
			ui_markup(cr, label, 22, 67 + row * 26, width - 36, PANGO_ALIGN_LEFT);
			g_free(label);
			g_free(name);
		}
		ui_hint(cr, *ui->model.error ? ui->model.error : ui->model.pending ? "Waiting for server" :
			manager_rename(&ui->model) ? (ui->model.normal ? "NORMAL  c/d/y: motion  p/P: paste  i: insert  Enter: save  q: cancel" : "INSERT  Enter: save  Esc: normal") :
			ui->model.mode == UI_SESSIONS ? (ui->model.normal ? "NORMAL  j/k: select  i: filter  Enter: attach  Ctrl-n: new  q: cancel" : "INSERT  Enter: attach  Ctrl-n: new  Ctrl-o: connections  Esc: normal") :
			picker ? (ui->model.normal ? "NORMAL  j/k: select  i: filter  Enter: accept  q: cancel" : "INSERT  Enter: accept  Esc: normal") :
			ui->model.normal ? "NORMAL  i: insert  Enter: accept  q: cancel" : "INSERT  Enter: accept  Esc: normal",
			14, height - 24, width - 28, *ui->model.error);
	}
	ui->panel_x = (client->width - width) / 2;
	ui->panel_width = width;
	ui->panel_y = STATUS_HEIGHT + (client->height - STATUS_HEIGHT - height) / 3;
	ui_upload(client, &ui->panel, buffer, cr, ui->panel_x, ui->panel_y);
}

static void ui_request(struct frontend *client, json_object *message) {
	if (!strcmp(json_object_get_string(field(message, "op")), "attach")) client->attach_sent = true;
	struct command *command = enqueue(client, message);
	if (command) { command->ui = true; client->ui.model.pending = true; }
	ui_draw(client);
}

static struct manager_context ui_context(struct frontend *client) {
	return (struct manager_context){
		.attached = client->key != NULL, .connected = *client->server_path != '\0',
		.defer_attach = client->defer_attach, .width = client->width,
		.height = client->height > STATUS_HEIGHT ? client->height - STATUS_HEIGHT : 1,
	};
}

static void ui_mode_changed(struct frontend *client) {
	trace_event(client, "ui_mode", 0, 0, ",\"mode\":%d", client->ui.model.mode);
	bindings_stop(client);
	client->prefix = client->resizing = false;
	apply_focus(client);
}

static void ui_close(struct frontend *client) {
	struct manager_context context = ui_context(client);
	manager_close(&client->ui.model, &context);
	ui_mode_changed(client);
	ui_draw(client);
}

static void ui_open(struct frontend *client, enum ui_mode mode) {
	struct manager_context context = ui_context(client);
	json_object *request = manager_open(&client->ui.model, mode, &context);
	ui_mode_changed(client);
	if (request) ui_request(client, request);
	ui_draw(client);
}

static void ui_accept(struct frontend *client) {
	struct manager_context context = ui_context(client);
	enum ui_mode previous = client->ui.model.mode;
	json_object *request = manager_accept(&client->ui.model, &context);
	if (previous != client->ui.model.mode) ui_mode_changed(client);
	if (request) ui_request(client, request);
	ui_draw(client);
}

static bool ui_key(struct frontend *client, xkb_keysym_t sym, uint32_t modifiers) {
	struct manager_context context = ui_context(client);
	unsigned flags = ((modifiers & WLR_MODIFIER_CTRL) ? UI_CONTROL : 0) |
		((modifiers & WLR_MODIFIER_ALT) ? UI_ALT : 0) |
		((modifiers & WLR_MODIFIER_LOGO) ? UI_SUPER : 0);
	enum ui_mode previous = client->ui.model.mode;
	json_object *request;
	bool consumed = manager_key(&client->ui.model, &context, sym, flags, &request);
	if (previous != client->ui.model.mode) ui_mode_changed(client);
	if (request) ui_request(client, request);
	if (consumed) ui_draw(client);
	return consumed;
}

static bool ui_pointer(struct frontend *client, double x, double y) {
	struct manager_ui *ui = &client->ui;
	if (!ui->model.mode && y < STATUS_HEIGHT) {
		if (x >= client->width - ui->session_width) ui_open(client, UI_SESSIONS);
		else if (ui->tab_width > 0 && x < ui->tabs_count * ui->tab_width) {
			bindings_stop(client);
			client->prefix = client->resizing = false;
			ui->model.error[0] = '\0';
			int index = ui->tabs_first + x / ui->tab_width;
			json_object *window = json_object_array_get_idx(field(ui->model.state, "windows"), index);
			json_object *message = operation("window_select");
			json_object_object_add(message, "number", json_object_get(field(window, "number")));
			ui_request(client, message);
		} else ui_open(client, UI_WINDOWS);
		return true;
	}
	if (!ui->model.mode) return false;
	int row = (y - ui->panel_y - 63) / 26;
	if ((ui->model.mode == UI_SESSIONS || ui->model.mode == UI_WINDOWS || ui->model.mode == UI_CONNECTIONS) && ui->panel && x >= ui->panel_x && x < ui->panel_x + ui->panel_width && y >= ui->panel_y + 63 && row >= 0 && row < 8 && manager_item(&ui->model, ui->model.first + row)) {
		ui->model.selected = ui->model.first + row;
		ui_accept(client);
	}
	return true;
}

static void ui_reply(struct frontend *client, json_object *message, bool requested) {
	struct manager_context context = ui_context(client);
	enum ui_mode previous = client->ui.model.mode;
	manager_reply(&client->ui.model, &context, message, requested);
	if (previous != client->ui.model.mode) ui_mode_changed(client);
	ui_draw(client);
}

static void ui_finish(struct frontend *client) {
	manager_finish(&client->ui.model);
	free(client->ui.status_text);
}
