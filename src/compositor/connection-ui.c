static void ui_socket_row(cairo_t *cr, json_object *row, int y, int width) {
	json_object *hint = field(row, "protocol_hint");
	ui_text(cr, json_object_get_string(field(row, "name")), 22, y + 1, width - (hint ? 160 : 44), UI_NORMAL);
	ui_text(cr, json_object_get_string(field(row, "path")), 22, y + 20, width - 44, UI_SUBDUED);
	if (hint) {
		char protocol[40]; snprintf(protocol, sizeof(protocol), "protocol %d", json_object_get_int(hint));
		ui_text(cr, protocol, width - 140, y + 1, 118, UI_SUBDUED);
	}
}

static const char *ui_connection_field(struct manager_state *ui) {
	return ui->connection_field == 1 ? "name" : ui->connection_field == 2 ? "identity" : "address";
}

static const char *ui_connection_identity(json_object *row) {
	const char *path = json_object_get_string(field(row, "identity"));
	return path ? path : "";
}

static char *ui_connection_summary(json_object *row) {
	const char *address = json_object_get_string(field(row, "address"));
	const char *name = json_object_get_string(field(row, "name"));
	const char *identity = ui_connection_identity(row);
	return g_strdup_printf("%s %s%s%s%s%s", json_object_get_string(field(row, "kind")), address,
		strcmp(name, address) ? " · " : "", strcmp(name, address) ? name : "",
		*identity ? " · key " : "", identity);
}

static PangoLayout *ui_connection_review_layout(cairo_t *cr, json_object *row, int width) {
	json_object *before = field(row, "before"), *after = field(row, "after");
	const char *color = !before ? UI_REVIEW_GREEN : !after ? UI_REVIEW_RED : UI_REVIEW_GOLD;
	GString *markup = g_string_new(NULL);
	g_string_append_printf(markup, "<span foreground='%s'>%s</span>  ", color, !before ? "+" : !after ? "−" : "~");
	if (before) {
		char *summary = ui_connection_summary(before);
		ui_review_span(markup, summary, color, true); g_free(summary);
		if (after) g_string_append(markup, "\n<span foreground='" UI_REVIEW_GOLD "'>↳</span>  ");
	}
	if (after) {
		char *summary = ui_connection_summary(after);
		ui_review_span(markup, summary, color, false); g_free(summary);
	}
	PangoLayout *layout = ui_layout(cr, "", width);
	pango_layout_set_markup(layout, markup->str, -1); g_string_free(markup, true);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE); pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	return layout;
}

static void ui_connection_row(cairo_t *cr, struct manager_ui *ui, json_object *row, bool selected, int y, int width) {
	struct manager_state *model = &ui->model;
	bool editing = selected && model->mode == UI_CONNECTION_NAME;
	const char *active = ui_connection_field(model);
	const char *name = editing && model->connection_field == 1 ? model->query : json_object_get_string(field(row, "name"));
	const char *address = editing && model->connection_field == 0 ? model->query : json_object_get_string(field(row, "address"));
	const char *kind = json_object_get_string(field(row, "kind"));
	const char *source = json_object_get_string(field(row, "source"));
	bool cursor = selected && (editing || model->normal) && strcmp(kind, "machine") && strcmp(kind, "command") && strcmp(source, "typed");
	if (!(cursor && model->connection_field == 1)) ui_text(cr, name, 22, y + 1, width - 140, UI_NORMAL);
	char *source_label = g_strdup_printf("%s%s", json_object_get_boolean(field(row, "active")) ? "● " : "", source);
	gchar *tag = g_markup_escape_text(source_label, -1); g_free(source_label);
	char *markup = g_strdup_printf("<span foreground='" UI_MARKUP_MUTED "' size='small'>%s</span>", tag);
	ui_markup(cr, markup, width - 112, y + 1, 90, PANGO_ALIGN_RIGHT); g_free(markup); g_free(tag);
	ui_text(cr, !strcmp(kind, "machine") ? "local" : kind, 22, y + 20, 52, UI_ACCENT);
	int address_left = 80;
	if (!(cursor && model->connection_field == 0)) ui_text(cr, address, address_left, y + 20,
		width - address_left - 22, UI_NORMAL);
	if (!strcmp(kind, "ssh")) {
		const char *identity = ui_connection_identity(row);
		char *label = g_strdup_printf("key  %s", *identity ? identity : "SSH defaults");
		ui_text(cr, label, 80, y + 39, width - 102, selected && model->normal && model->connection_field == 2 ? UI_ACCENT : UI_SUBDUED);
		g_free(label);
	}
	if (cursor && model->connection_field != 2 && strcmp(kind, "machine") && strcmp(kind, "command") && strcmp(source, "typed")) {
		int left = model->connection_field == 1 ? 22 : address_left;
		int top = model->connection_field == 1 ? y + 1 : y + 20;
		int available = model->connection_field == 1 ? width - 140 - left : width - left - 22;
		if (editing) ui_edit_at(cr, ui, left, top, available);
		else ui_edit_text_at(cr, json_object_get_string(field(row, active)), model->row_cursor, true, left, top, available);
	}
}
