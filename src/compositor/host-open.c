static bool desktop_open_allowed(struct desktop_bridge *bridge, int64_t pane, pid_t pid) {
	struct frontend *client = bridge->client;
	return bridge->focused && client->ui.model.mode == UI_NONE && !client->prefix &&
		!client->resizing && pane == client->approved_focus && pid == client->terminal_pid;
}

static bool desktop_open_argv(json_object *argv) {
	if (!argv || !json_object_is_type(argv, json_type_array)) return false;
	size_t count = json_object_array_length(argv);
	if (!count || count > 256) return false;
	for (size_t i = 0; i < count; i++) {
		json_object *arg = json_object_array_get_idx(argv, i);
		if (!json_object_is_type(arg, json_type_string) ||
			(size_t)json_object_get_string_len(arg) != strlen(json_object_get_string(arg))) return false;
	}
	return *json_object_get_string(json_object_array_get_idx(argv, 0)) != '\0';
}

static void desktop_open_launch(struct desktop_bridge *bridge, const char *token) {
	if (!desktop_open_allowed(bridge, bridge->open_pane, bridge->open_pid)) return;
	size_t count = json_object_array_length(bridge->open_command);
	char **argv = calloc(count + 1, sizeof(*argv));
	if (!argv) return;
	for (size_t i = 0; i < count; i++)
		argv[i] = (char *)json_object_get_string(json_object_array_get_idx(bridge->open_command, i));
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		setenv("XDG_RUNTIME_DIR", bridge->host_runtime, true);
		setenv("WAYLAND_DISPLAY", bridge->host_display, true);
		if (bridge->host_x_display && *bridge->host_x_display)
			setenv("DISPLAY", bridge->host_x_display, true);
		else unsetenv("DISPLAY");
		unsetenv("WAYLAND_SOCKET");
		unsetenv("AMUX_STATE_STREAM");
		unsetenv("DESKTOP_STARTUP_ID");
		if (token && *token) setenv("XDG_ACTIVATION_TOKEN", token, true);
		else unsetenv("XDG_ACTIVATION_TOKEN");
		int fd = open("/dev/null", O_RDONLY);
		if (fd >= 0) { dup2(fd, STDIN_FILENO); if (fd > STDIN_FILENO) close(fd); }
		execvp(argv[0], argv);
		perror("host link opener");
		_exit(127);
	}
	if (pid > 0) trace_event(bridge->client, "host_open_launched", 0, bridge->open_pane,
		",\"child_pid\":%ld,\"activation\":%s", (long)pid, token && *token ? "true" : "false");
	free(argv);
}

static void desktop_open_done(void *data, struct xdg_activation_token_v1 *token, const char *value) {
	struct desktop_bridge *bridge = data;
	desktop_open_launch(bridge, value);
	xdg_activation_token_v1_destroy(token);
	bridge->open_token = NULL;
	json_object_put(bridge->open_command);
	bridge->open_command = NULL;
}
static const struct xdg_activation_token_v1_listener desktop_open_listener = {
	.done = desktop_open_done,
};

static int desktop_open_ready(int fd, uint32_t mask, void *data) {
	struct desktop_bridge *bridge = data;
	char bytes[65537];
	union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(struct ucred))]; } credentials;
	struct iovec vector = {.iov_base = bytes, .iov_len = sizeof(bytes) - 1};
	struct msghdr header = {.msg_iov = &vector, .msg_iovlen = 1,
		.msg_control = credentials.bytes, .msg_controllen = sizeof(credentials.bytes)};
	ssize_t size = recvmsg(fd, &header, MSG_DONTWAIT);
	if (size <= 0 || header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return 0;
	struct cmsghdr *control = CMSG_FIRSTHDR(&header);
	if (!control || control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_CREDENTIALS ||
		control->cmsg_len != CMSG_LEN(sizeof(struct ucred))) return 0;
	struct ucred peer;
	memcpy(&peer, CMSG_DATA(control), sizeof(peer));
	if (peer.uid != getuid() || peer.pid != bridge->client->terminal_pid || bridge->open_command) return 0;
	bytes[size] = '\0';
	json_object *request = json_tokener_parse(bytes);
	json_object *pane = field(request, "pane"), *argv = field(request, "argv");
	if (!pane || !json_object_is_type(pane, json_type_int) || !desktop_open_argv(argv) ||
		!desktop_open_allowed(bridge, json_object_get_int64(pane), peer.pid)) {
		json_object_put(request); return 0;
	}
	bridge->open_pane = json_object_get_int64(pane);
	bridge->open_pid = peer.pid;
	bridge->open_command = json_object_get(argv);
	json_object_put(request);
	if (bridge->activation) {
		bridge->open_token = xdg_activation_v1_get_activation_token(bridge->activation);
		xdg_activation_token_v1_add_listener(bridge->open_token, &desktop_open_listener, bridge);
		if (bridge->input_serial && monotonic_ns() - bridge->input_ns < 5000000000ull)
			xdg_activation_token_v1_set_serial(bridge->open_token, bridge->input_serial, bridge->seat);
		struct output *output;
		wl_list_for_each(output, &bridge->client->outputs, link) {
			if (!wlr_output_is_wl(output->wlr_output)) continue;
			xdg_activation_token_v1_set_surface(bridge->open_token, wlr_wl_output_get_surface(output->wlr_output));
			break;
		}
		xdg_activation_token_v1_commit(bridge->open_token);
		wl_display_flush(bridge->display);
	} else {
		desktop_open_launch(bridge, NULL);
		json_object_put(bridge->open_command); bridge->open_command = NULL;
	}
	return 0;
}

static bool desktop_open_init(struct frontend *client) {
	struct desktop_bridge *bridge = &client->desktop;
	bridge->host_runtime = getenv("AMUX_HOST_RUNTIME_DIR");
	bridge->host_display = getenv("AMUX_HOST_WAYLAND_DISPLAY");
	bridge->host_x_display = getenv("AMUX_HOST_X_DISPLAY");
	if (!bridge->host_runtime || !bridge->host_display) return false;
	int length = snprintf(bridge->open_path, sizeof(bridge->open_path), "%s/host-open.sock", getenv("XDG_RUNTIME_DIR"));
	if (length < 0 || (size_t)length >= sizeof(bridge->open_path)) return false;
	bridge->open_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (bridge->open_fd < 0) return false;
	int enabled = 1;
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	strcpy(address.sun_path, bridge->open_path);
	if (setsockopt(bridge->open_fd, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) < 0 ||
		bind(bridge->open_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		close(bridge->open_fd); bridge->open_fd = -1; return false;
	}
	bridge->open_source = wl_event_loop_add_fd(wl_display_get_event_loop(client->wl_display),
		bridge->open_fd, WL_EVENT_READABLE, desktop_open_ready, bridge);
	return bridge->open_source && setenv("AMUX_HOST_OPEN_SOCKET", bridge->open_path, true) == 0;
}

static void desktop_open_finish(struct desktop_bridge *bridge) {
	if (bridge->open_token) xdg_activation_token_v1_destroy(bridge->open_token);
	json_object_put(bridge->open_command);
	if (bridge->open_source) wl_event_source_remove(bridge->open_source);
	if (*bridge->open_path) { close(bridge->open_fd); unlink(bridge->open_path); }
}
