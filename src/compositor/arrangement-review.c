struct arrangement_diff {
	json_object *before, *after;
	const char *key;
	size_t i, j, m, n, *lengths;
};

enum arrangement_diff_step { ARRANGEMENT_DONE, ARRANGEMENT_SAME, ARRANGEMENT_FROM, ARRANGEMENT_TO };

static struct arrangement_diff arrangement_diff(json_object *before, json_object *after, const char *key) {
	struct arrangement_diff diff = {.before = before, .after = after, .key = key,
		.m = before ? json_object_array_length(before) : 0, .n = after ? json_object_array_length(after) : 0};
	diff.lengths = g_new0(size_t, (diff.m + 1) * (diff.n + 1));
	for (size_t i = diff.m; i-- > 0;) for (size_t j = diff.n; j-- > 0;) {
		bool same = json_object_get_int64(field(json_object_array_get_idx(before, i), key)) ==
			json_object_get_int64(field(json_object_array_get_idx(after, j), key));
		diff.lengths[i * (diff.n + 1) + j] = same ? 1 + diff.lengths[(i + 1) * (diff.n + 1) + j + 1] :
			MAX(diff.lengths[(i + 1) * (diff.n + 1) + j], diff.lengths[i * (diff.n + 1) + j + 1]);
	}
	return diff;
}

static enum arrangement_diff_step arrangement_diff_next(struct arrangement_diff *diff,
	json_object **before, json_object **after) {
	*before = diff->i < diff->m ? json_object_array_get_idx(diff->before, diff->i) : NULL;
	*after = diff->j < diff->n ? json_object_array_get_idx(diff->after, diff->j) : NULL;
	if (!*before && !*after) return ARRANGEMENT_DONE;
	if (*before && *after && json_object_get_int64(field(*before, diff->key)) == json_object_get_int64(field(*after, diff->key))) {
		diff->i++; diff->j++; return ARRANGEMENT_SAME;
	}
	if (*before && (!*after || diff->lengths[(diff->i + 1) * (diff->n + 1) + diff->j] >=
		diff->lengths[diff->i * (diff->n + 1) + diff->j + 1])) {
		diff->i++; *after = NULL; return ARRANGEMENT_FROM;
	}
	diff->j++; *before = NULL; return ARRANGEMENT_TO;
}

static json_object *arrangement_review_row(json_object *rows, json_object *before, json_object *after,
	const char *key, bool ghost, bool moved) {
	json_object *item = ghost ? before : after, *row = json_object_new_object();
	json_object_object_add(row, key, json_object_get(field(item, key)));
	json_object_object_add(row, "name", json_object_get(field(item, "name")));
	json_object_object_add(row, "ghost", json_object_new_boolean(ghost));
	json_object_object_add(row, "moved", json_object_new_boolean(moved));
	json_object_object_add(row, "added", json_object_new_boolean(!before && !ghost));
	json_object_object_add(row, "removed", json_object_new_boolean(!after && ghost));
	bool renamed = before && after && strcmp(json_object_get_string(field(before, "name")), json_object_get_string(field(after, "name")));
	json_object_object_add(row, "changed", json_object_new_boolean(moved || !before || !after || renamed));
	if (!ghost && before && strcmp(json_object_get_string(field(before, "name")), json_object_get_string(field(after, "name"))))
		json_object_object_add(row, "before", json_object_get(field(before, "name")));
	json_object_array_add(rows, row);
	return row;
}

static json_object *arrangement_review_window(json_object *rows, json_object *base, json_object *draft,
	json_object *item, bool ghost, GHashTable *moved) {
	int64_t identity = json_object_get_int64(field(item, "window"));
	json_object *owner = arrangement_owner(ghost ? draft : base, identity);
	json_object *other = owner ? arrangement_find(field(owner, "windows"), "window", identity) : NULL;
	return arrangement_review_row(rows, ghost ? item : other, ghost ? other : item, "window", ghost,
		other && g_hash_table_contains(moved, &identity));
}

static json_object *arrangement_review_compact(json_object *rows) {
	json_object *compact = json_object_new_array();
	for (size_t start = 0, end; start < json_object_array_length(rows); start = end) {
		end = start + 1;
		while (end < json_object_array_length(rows) && !field(json_object_array_get_idx(rows, end), "session")) end++;
		bool affected = false;
		for (size_t i = start; i < end; i++)
			affected |= json_object_get_boolean(field(json_object_array_get_idx(rows, i), "changed"));
		if (!affected) continue;
		json_object_array_add(compact, json_object_get(json_object_array_get_idx(rows, start)));
		for (size_t i = start + 1; i < end;) {
			size_t next = i;
			while (next < end && !json_object_get_boolean(field(json_object_array_get_idx(rows, next), "changed"))) next++;
			if (next - i >= 2) {
				json_object *fold = json_object_new_object(), *children = json_object_new_array();
				json_object_object_add(fold, "name", json_object_new_string("…"));
				for (; i < next; i++) json_object_array_add(children, json_object_get(json_object_array_get_idx(rows, i)));
				json_object_object_add(fold, "collapsed", children);
				json_object_array_add(compact, fold);
			} else {
				json_object_array_add(compact, json_object_get(json_object_array_get_idx(rows, i++)));
			}
		}
	}
	json_object_put(rows);
	return compact;
}

struct arrangement_review_link { int from, to, start, end; };

static int arrangement_review_link_compare(const void *a, const void *b) {
	const struct arrangement_review_link *left = a, *right = b;
	if (left->start != right->start) return left->start < right->start ? -1 : 1;
	return left->end < right->end ? -1 : left->end > right->end;
}

static void arrangement_review_pairs(json_object *rows) {
	GArray *links = g_array_new(false, false, sizeof(struct arrangement_review_link));
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		json_object *from = json_object_array_get_idx(rows, i);
		if (!json_object_get_boolean(field(from, "ghost"))) continue;
		const char *key = field(from, "session") ? "session" : "window";
		int64_t identity = json_object_get_int64(field(from, key));
		for (size_t j = 0; j < json_object_array_length(rows); j++) {
			json_object *to = json_object_array_get_idx(rows, j);
			if (json_object_get_boolean(field(to, "ghost")) || !field(to, key) || json_object_get_int64(field(to, key)) != identity) continue;
			json_object_object_add(from, "peer", json_object_new_int(j));
			json_object_object_add(to, "peer", json_object_new_int(i));
			if (json_object_get_boolean(field(from, "moved"))) {
				struct arrangement_review_link link = {.from = i, .to = j, .start = MIN(i, j), .end = MAX(i, j)};
				g_array_append_val(links, link);
			}
			break;
		}
	}
	g_array_sort(links, arrangement_review_link_compare);
	GArray *ends = g_array_new(false, false, sizeof(int));
	for (size_t i = 0; i < links->len; i++) {
		struct arrangement_review_link link = g_array_index(links, struct arrangement_review_link, i);
		size_t lane = 0;
		while (lane < ends->len && g_array_index(ends, int, lane) >= link.start) lane++;
		if (lane == ends->len) g_array_append_val(ends, link.end);
		else g_array_index(ends, int, lane) = link.end;
		json_object_object_add(json_object_array_get_idx(rows, link.from), "lane", json_object_new_int(lane));
		json_object_object_add(json_object_array_get_idx(rows, link.to), "lane", json_object_new_int(lane));
	}
	g_array_free(ends, true); g_array_free(links, true);
}

static json_object *arrangement_review_tree(json_object *base, json_object *draft) {
	json_object *rows = json_object_new_array();
	GHashTable *moved = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	for (size_t s = 0; s < json_object_array_length(base); s++) {
		json_object *old = json_object_array_get_idx(base, s);
		json_object *new = arrangement_find(draft, "session", json_object_get_int64(field(old, "session")));
		struct arrangement_diff diff = arrangement_diff(field(old, "windows"), field(new, "windows"), "window");
		json_object *before, *after;
		enum arrangement_diff_step step;
		while ((step = arrangement_diff_next(&diff, &before, &after)) != ARRANGEMENT_DONE) if (step == ARRANGEMENT_FROM) {
			int64_t *identity = g_new(int64_t, 1); *identity = json_object_get_int64(field(before, "window"));
			g_hash_table_add(moved, identity);
		}
		g_free(diff.lengths);
	}
	struct arrangement_diff sessions = arrangement_diff(base, draft, "session");
	json_object *before, *after;
	enum arrangement_diff_step step;
	while ((step = arrangement_diff_next(&sessions, &before, &after)) != ARRANGEMENT_DONE) {
		bool ghost = step == ARRANGEMENT_FROM;
		json_object *session = ghost ? before : after;
		int64_t identity = json_object_get_int64(field(session, "session"));
		json_object *other = arrangement_find(ghost ? draft : base, "session", identity);
		arrangement_review_row(rows, ghost ? session : other, ghost ? other : session, "session", ghost,
			step != ARRANGEMENT_SAME && other);
		if (step == ARRANGEMENT_SAME) {
			struct arrangement_diff windows = arrangement_diff(field(before, "windows"), field(after, "windows"), "window");
			json_object *old, *new;
			enum arrangement_diff_step ws;
			while ((ws = arrangement_diff_next(&windows, &old, &new)) != ARRANGEMENT_DONE)
				arrangement_review_window(rows, base, draft, ws == ARRANGEMENT_FROM ? old : new, ws == ARRANGEMENT_FROM, moved);
			g_free(windows.lengths);
		} else {
			json_object *windows = field(session, "windows");
			for (size_t w = 0; w < json_object_array_length(windows); w++)
				arrangement_review_window(rows, base, draft, json_object_array_get_idx(windows, w), ghost, moved);
		}
		if (!ghost && !other && !json_object_array_length(field(session, "windows"))) {
			json_object *shell = json_tokener_parse("{\"window\":0,\"name\":\"shell\"}");
			json_object *row = arrangement_review_row(rows, NULL, shell, "window", false, false);
			json_object_object_add(row, "initial", json_object_new_boolean(true)); json_object_put(shell);
		}
	}
	g_free(sessions.lengths); g_hash_table_destroy(moved);
	rows = arrangement_review_compact(rows);
	arrangement_review_pairs(rows);
	return rows;
}
