static void output_frame(struct wl_listener *listener, void *data) {

	struct output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->client->scene;

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	bool tracing = output->client->trace_fd >= 0 && output->client->trace_frame_pending;
	if (tracing) trace_event(output->client, "frame_submit_begin", 0, 0,
		",\"revision\":%" PRIu64, output->client->revision);
	uint32_t before = output->wlr_output->commit_seq;
	bool committed = wlr_scene_output_commit(scene_output, NULL);
	if (tracing) {
		trace_event(output->client, "frame_submit_end", 0, 0,
			",\"revision\":%" PRIu64 ",\"commit_seq\":%u,\"ok\":%s,\"submitted\":%s",
			output->client->revision, output->wlr_output->commit_seq, committed ? "true" : "false",
			before != output->wlr_output->commit_seq ? "true" : "false");
		if (committed && before != output->wlr_output->commit_seq) output->client->trace_frame_pending = false;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_present(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, present);
	struct wlr_output_event_present *event = data;
	trace_event(output->client, "output_feedback", 0, 0,
		",\"commit_seq\":%u,\"presented\":%s,\"when_ns\":%" PRIu64 ",\"flags\":%u",
		event->commit_seq, event->presented ? "true" : "false",
		(uint64_t)event->when.tv_sec * 1000000000 + (uint64_t)event->when.tv_nsec, event->flags);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	if (wlr_output_commit_state(output->wlr_output, event->state)) report_viewport(output->client);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->present.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	wl_display_terminate(output->client->wl_display);
	free(output);
}

static void frontend_new_output(struct wl_listener *listener, void *data) {

	struct frontend *client =
		wl_container_of(listener, client, new_output);
	struct wlr_output *wlr_output = data;
	if (!wl_list_empty(&client->outputs)) { fail(client, "Amux supports one nested output"); return; }
	wlr_wl_output_set_title(wlr_output, "amux");
	wlr_wl_output_set_app_id(wlr_output, "amux");

	if (!wlr_output_init_render(wlr_output, client->allocator, client->renderer)) {
		fail(client, "cannot initialize nested output rendering");
		return;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_render_format(&state, DRM_FORMAT_ARGB8888);
	wlr_output_state_set_custom_mode(&state, client->width, client->height, 0);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}

	bool committed = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!committed) { fail(client, "cannot initialize transparent nested output (ARGB8888)"); return; }

	struct output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->client = client;

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->present.notify = output_present;
	wl_signal_add(&wlr_output->events.present, &output->present);

	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&client->outputs, &output->link);

	struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(client->output_layout,
		wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(client->scene, wlr_output);
	wlr_scene_output_set_transparent_background(scene_output, true);
	wlr_scene_output_layout_add_output(client->scene_layout, l_output, scene_output);
	report_viewport(client);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, map);
	view->mapped = true;
	struct pane *pane = find_pane(view->client, view->pane);
	if (pane) pane->presentation = VIEW_MAPPED;
	trace_event(view->client, "surface_mapped", pane ? pane->birth_request : 0, view->pane, NULL);
	view->client->trace_frame_pending = true;
	update_view(view);
	apply_focus(view->client);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, unmap);
	view->mapped = false;
	struct pane *pane = find_pane(view->client, view->pane);
	if (pane && pane->presentation != VIEW_IDLE) pane->presentation = VIEW_FAILED;
	trace_event(view->client, "surface_unmapped", 0, view->pane, NULL);
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	if (!view->client->quitting) pane_gone(view->client, view->pane);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, commit);
	if (view->xdg_toplevel->base->initial_commit) {
		const char *app_id = view->xdg_toplevel->app_id;
		int64_t id = 0;
		char tail;
		if (!app_id || sscanf(app_id, "amux-pane-%" SCNd64 "%c", &id, &tail) != 1 || !find_pane(view->client, id)) {
			wlr_xdg_toplevel_set_size(view->xdg_toplevel, 1, 1);
			wlr_xdg_toplevel_send_close(view->xdg_toplevel);
			return;
		}
		view->pane = id;
		struct view *other;
		wl_list_for_each(other, &view->client->toplevels, link) {
			if (other != view && other->pane == id) {
				view->pane = 0;
				wlr_xdg_toplevel_set_size(view->xdg_toplevel, 1, 1);
				wlr_xdg_toplevel_send_close(view->xdg_toplevel);
				return;
			}
		}
		struct pane *pane = find_pane(view->client, id);
		trace_event(view->client, "surface_initial_commit", pane->birth_request, id, NULL);
	}
	update_view(view);
	struct wlr_xdg_surface *surface = view->xdg_toplevel->base;
	if (surface->surface->buffer && surface->current.configure_serial != view->traced_commit) {
		view->traced_commit = surface->current.configure_serial;
		trace_event(view->client, "surface_buffer_commit", 0, view->pane,
			",\"serial\":%u,\"width\":%d,\"height\":%d", view->traced_commit,
			surface->surface->current.width, surface->surface->current.height);
		view->client->trace_frame_pending = true;
	}
}

static void xdg_configure(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, configure);
	struct wlr_xdg_surface_configure *event = data;
	trace_event(view->client, "configure_sent", view->configure_request, view->pane,
		",\"serial\":%u", event->serial);
}

static void xdg_ack_configure(struct wl_listener *listener, void *data) {
	struct view *view = wl_container_of(listener, view, ack_configure);
	struct wlr_xdg_surface_configure *event = data;
	trace_event(view->client, "configure_acked", 0, view->pane,
		",\"serial\":%u", event->serial);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {

	struct view *toplevel = wl_container_of(listener, toplevel, destroy);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->configure.link);
	wl_list_remove(&toplevel->ack_configure.link);

	toplevel->scene_tree->node.data = NULL;
	struct frontend *client = toplevel->client;
	free(toplevel);
	spawn_views(client);
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) {

	struct view *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {

	struct view *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void frontend_new_xdg_toplevel(struct wl_listener *listener, void *data) {

	struct frontend *client = wl_container_of(listener, client, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct view *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->client = client;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->scene_tree =
		wlr_scene_xdg_surface_create(toplevel->client->surfaces, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
	wl_list_insert(&client->toplevels, &toplevel->link);
	xdg_toplevel->base->data = toplevel->scene_tree;

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->configure.notify = xdg_configure;
	wl_signal_add(&xdg_toplevel->base->events.configure, &toplevel->configure);
	toplevel->ack_configure.notify = xdg_ack_configure;
	wl_signal_add(&xdg_toplevel->base->events.ack_configure, &toplevel->ack_configure);

	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {

	struct popup *popup = wl_container_of(listener, popup, commit);

	if (popup->xdg_popup->base->initial_commit) {

		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {

	struct popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);

	free(popup);
}

static void frontend_new_xdg_popup(struct wl_listener *listener, void *data) {

	struct wlr_xdg_popup *xdg_popup = data;

	struct popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	assert(parent != NULL);
	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}
