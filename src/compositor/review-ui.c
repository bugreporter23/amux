#define UI_REVIEW_RED "#df9290"
#define UI_REVIEW_GREEN "#9dccaa"
#define UI_REVIEW_GOLD "#d6b67c"

static void ui_review_span(GString *markup, const char *text, const char *color, bool strike) {
	gchar *escaped = g_markup_escape_text(text, -1);
	g_string_append_printf(markup, "<span foreground='%s'%s>%s</span>", color,
		strike ? " strikethrough='true'" : "", escaped);
	g_free(escaped);
}

static int ui_review_left(json_object *item) { return field(item, "session") ? 22 : 40; }

static PangoLayout *ui_review_layout(cairo_t *cr, json_object *item, int width) {
	if (field(item, "collapsed")) {
		char markup[160];
		snprintf(markup, sizeof(markup), "<span foreground='" UI_MARKUP_MUTED "'>…  <span size='small'>%zu unchanged windows</span></span>",
			json_object_array_length(field(item, "collapsed")));
		PangoLayout *layout = ui_layout(cr, "", width);
		pango_layout_set_markup(layout, markup, -1);
		return layout;
	}
	bool session = field(item, "session"), ghost = json_object_get_boolean(field(item, "ghost"));
	bool moved = json_object_get_boolean(field(item, "moved")), added = json_object_get_boolean(field(item, "added"));
	bool removed = json_object_get_boolean(field(item, "removed"));
	const char *color = removed ? UI_REVIEW_RED : added ? UI_REVIEW_GREEN : moved ? UI_MARKUP_BLUE : UI_MARKUP_MUTED;
	GString *markup = g_string_new(NULL);
	g_string_append_printf(markup, "<span foreground='%s' size='small'>%s ", color, session ? "session" : "window");
	int64_t identity = json_object_get_int64(field(item, session ? "session" : "window"));
	if (identity > 0) g_string_append_printf(markup, "%" PRId64, identity);
	else g_string_append(markup, "new");
	g_string_append(markup, "</span>  ");
	if (session) g_string_append(markup, "<span weight='bold'>");
	if (field(item, "before")) {
		ui_review_span(markup, json_object_get_string(field(item, "before")), UI_REVIEW_GOLD, true);
		g_string_append(markup, " <span foreground='" UI_REVIEW_GOLD "'>→</span> ");
		color = UI_REVIEW_GOLD;
	}
	ui_review_span(markup, json_object_get_string(field(item, "name")), color, ghost);
	if (session) g_string_append(markup, "</span>");
	if (json_object_get_boolean(field(item, "initial")))
		g_string_append(markup, " <span foreground='" UI_MARKUP_MUTED "' size='small'>· initial</span>");
	PangoLayout *layout = ui_layout(cr, "", width);
	pango_layout_set_markup(layout, markup->str, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	g_string_free(markup, true);
	return layout;
}

static int ui_review_gutter(json_object *rows) {
	int lanes = 0;
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		json_object *lane = field(json_object_array_get_idx(rows, i), "lane");
		if (lane) lanes = MAX(lanes, json_object_get_int(lane) + 1);
	}
	return lanes ? 26 + MIN(lanes, 12) * 12 : 16;
}

static void ui_review_row(cairo_t *cr, json_object *item, int width, int gutter, int y, int height) {
	int left = ui_review_left(item);
	const char *marker = json_object_get_boolean(field(item, "removed")) ? "−" :
		json_object_get_boolean(field(item, "added")) ? "+" :
		json_object_get_boolean(field(item, "moved")) ? (json_object_get_boolean(field(item, "ghost")) ? "↗" : "↳") :
		field(item, "before") ? "~" : "";
	const char *color = json_object_get_boolean(field(item, "removed")) ? UI_REVIEW_RED :
		json_object_get_boolean(field(item, "added")) ? UI_REVIEW_GREEN :
		json_object_get_boolean(field(item, "moved")) ? UI_MARKUP_BLUE : UI_REVIEW_GOLD;
	PangoLayout *mark = ui_layout(cr, "", 16);
	char *markup = g_strdup_printf("<span foreground='%s'>%s</span>", color, marker);
	pango_layout_set_markup(mark, markup, -1); g_free(markup);
	cairo_move_to(cr, left - 14, y + 3); pango_cairo_show_layout(cr, mark); g_object_unref(mark);
	PangoLayout *layout = ui_review_layout(cr, item, width - gutter - left);
	cairo_save(cr); cairo_rectangle(cr, left, y, width - gutter - left, height); cairo_clip(cr);
	cairo_set_source_rgb(cr, UI_RGB(UI_BRIGHT)); cairo_move_to(cr, left, y + 3);
	pango_cairo_show_layout(cr, layout);
	cairo_restore(cr); g_object_unref(layout);
}

static int ui_review_center(int index, int first, int count, const int *heights) {
	int y = 64;
	for (int i = 0; i < count; i++) {
		if (index == first + i) return y + heights[i] / 2;
		if (index < first + i) break;
		y += heights[i];
	}
	return y;
}

static int ui_review_tip(cairo_t *cr, json_object *row, int width, int gutter) {
	int left = ui_review_left(row), text_width;
	PangoLayout *layout = ui_review_layout(cr, row, width - gutter - left);
	pango_layout_get_pixel_size(layout, &text_width, NULL); g_object_unref(layout);
	return MIN(left + text_width + 8, width - gutter + 6);
}

static void ui_review_connections(cairo_t *cr, json_object *rows, int first, int count,
	const int *heights, int width, int gutter, int selected) {
	if (!count) return;
	int bottom = ui_review_center(first + count, first, count, heights);
	int edge = 0;
	for (int i = 0; i < count; i++)
		edge = MAX(edge, ui_review_tip(cr, json_object_array_get_idx(rows, first + i), width, gutter) + 14);
	cairo_save(cr); cairo_rectangle(cr, 8, 64, width - 16, bottom - 64); cairo_clip(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	for (int active = 0; active < 2; active++) for (size_t i = 0; i < json_object_array_length(rows); i++) {
		json_object *from = json_object_array_get_idx(rows, i), *peer = field(from, "peer");
		if (!peer || !field(from, "lane") || !json_object_get_boolean(field(from, "ghost"))) continue;
		int to_index = json_object_get_int(peer);
		bool chosen = (int)i == selected || to_index == selected;
		if (chosen != (bool)active || MAX((int)i, to_index) < first || MIN((int)i, to_index) >= first + count) continue;
		json_object *to = json_object_array_get_idx(rows, to_index);
		bool from_visible = (int)i >= first && (int)i < first + count;
		bool to_visible = to_index >= first && to_index < first + count;
		int lane = json_object_get_int(field(from, "lane"));
		int x = MIN(edge + MIN(lane, 11) * 12, width - 18);
		int y1 = ui_review_center(i, first, count, heights), y2 = ui_review_center(to_index, first, count, heights);
		cairo_set_source_rgba(cr, UI_BLUE[0], UI_BLUE[1], UI_BLUE[2], chosen ? .95 : .45);
		cairo_set_line_width(cr, chosen ? 1.8 : 1.2);
		int source = from_visible ? ui_review_tip(cr, from, width, gutter) : x;
		int target = to_visible ? ui_review_tip(cr, to, width, gutter) : x;
		cairo_move_to(cr, source, y1); cairo_line_to(cr, x, y1); cairo_line_to(cr, x, y2); cairo_line_to(cr, target, y2);
		if (to_visible) {
			cairo_move_to(cr, target + 4, y2 - 4); cairo_line_to(cr, target, y2); cairo_line_to(cr, target + 4, y2 + 4);
		} else {
			int direction = to_index < first ? 1 : -1;
			cairo_move_to(cr, x - 3, y2 + direction * 5); cairo_line_to(cr, x, y2); cairo_line_to(cr, x + 3, y2 + direction * 5);
		}
		cairo_stroke(cr);
	}
	cairo_restore(cr);
}
