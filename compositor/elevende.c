/*
 * ElevenDE compositor — a Windows 11-style Wayland desktop for Linux.
 *
 * Built on wlroots 0.17. Extends the classic tinywl architecture with:
 *   - wlr-layer-shell (taskbar / start menu / wallpaper layers)
 *   - XWayland (legacy X11 application compatibility)
 *   - Unix-socket JSON IPC with the ElevenDE shell
 *   - window management: focus, minimize, maximize, fullscreen, Alt-Tab
 *   - xdg-decoration (prefer CSD), activation, fractional scale, etc.
 *
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xkbcommon/xkbcommon.h>

#include "wlr-layer-shell-unstable-v1-protocol.h"

#define IPC_PATH_MAX 256
#define TITLE_MAX 256
#define APPID_MAX 128

enum cursor_mode {
	CURSOR_PASSTHROUGH,
	CURSOR_MOVE,
	CURSOR_RESIZE,
};

enum view_type {
	VIEW_XDG,
	VIEW_XWAYLAND,
};

struct server;
struct view;
struct output;

struct output {
	struct wl_list link;
	struct server *server;
	struct wlr_output *wlr_output;
	struct wlr_box usable_area; /* work area excluding exclusive layer zones */
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct view {
	struct wl_list link; /* server->views, head = most recently focused */
	struct server *server;
	enum view_type type;
	uint32_t id;
	char title[TITLE_MAX];
	char app_id[APPID_MAX];
	bool mapped;
	bool minimized;
	bool maximized;
	bool fullscreen;
	struct wlr_box saved_geo; /* geometry before maximize/fullscreen */

	struct wlr_xdg_toplevel *xdg_toplevel;   /* VIEW_XDG */
	struct wlr_xwayland_surface *xws;        /* VIEW_XWAYLAND */
	struct wlr_scene_tree *scene_tree;
	struct wlr_scene_tree *scene_content;    /* xwayland subsurface tree */

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;     /* xwayland */
	struct wl_listener request_configure;    /* xwayland */
	struct wl_listener request_activate;     /* xwayland */
	struct wl_listener associate;            /* xwayland */
	struct wl_listener dissociate;           /* xwayland */
	struct wl_listener surface_map;          /* xwayland */
	struct wl_listener surface_unmap;        /* xwayland */
	bool surface_listeners_bound;
};

struct layer_surface {
	struct wl_list link; /* server->layer_surfaces */
	struct server *server;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener new_popup;
	struct wl_listener commit;
};

struct keyboard {
	struct wl_list link;
	struct server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct ipc_client {
	struct wl_list link;
	struct server *server;
	int fd;
	struct wl_event_source *src;
	char buf[8192];
	size_t len;
};

struct server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;

	/* scene stacking order: bg, bottom, toplevels, top, overlay */
	struct wlr_scene_tree *tree_bg;
	struct wlr_scene_tree *tree_bottom;
	struct wlr_scene_tree *tree_toplevels;
	struct wlr_scene_tree *tree_top;
	struct wlr_scene_tree *tree_overlay;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_surface;

	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;

	struct wlr_xwayland *xwayland;
	struct wl_listener xwayland_ready;
	struct wl_listener xwayland_new_surface;

	struct wlr_xdg_decoration_manager_v1 *deco_mgr;
	struct wl_listener new_decorator;

	struct wlr_xdg_activation_v1 *activation;
	struct wl_listener activation_request;

	struct wl_list views;
	struct wl_list layer_surfaces;
	struct wl_list outputs;
	struct wl_list keyboards;
	struct wl_listener new_output;
	struct wl_listener new_input;
	uint32_t next_view_id;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	enum cursor_mode cursor_mode;
	struct view *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct view *focused_view;
	struct layer_surface *focused_layer;
	bool super_held;
	bool super_used;
	bool alt_held;

	int ipc_fd;
	struct wl_event_source *ipc_src;
	struct wl_list ipc_clients;
};

/* ================================ IPC ================================= */

static void ipc_broadcast(struct server *server, const char *msg) {
	size_t len = strlen(msg);
	struct ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
		ssize_t n = send(client->fd, msg, len, MSG_NOSIGNAL);
		(void)n; /* best effort; drop on backpressure */
	}
}

static void json_escape(char *dst, size_t n, const char *src) {
	size_t i = 0;
	if (!src) { if (n) dst[0] = 0; return; }
	for (; *src && i + 2 < n; src++) {
		char c = *src;
		if (c == '"' || c == '\\') { if (i + 3 >= n) break; dst[i++] = '\\'; dst[i++] = c; }
		else if (c == '\n') { if (i + 3 >= n) break; dst[i++] = '\\'; dst[i++] = 'n'; }
		else if ((unsigned char)c < 0x20) { dst[i++] = ' '; }
		else dst[i++] = c;
	}
	dst[i] = 0;
}

static const char *json_find(const char *json, const char *key) {
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = strstr(json, pat);
	if (!p) return NULL;
	p += strlen(pat);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	return p;
}

static bool json_get_str(const char *json, const char *key, char *out, size_t n) {
	const char *p = json_find(json, key);
	if (!p || *p != '"') return false;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
	out[i] = 0;
	return true;
}

static long json_get_int(const char *json, const char *key, long dflt) {
	const char *p = json_find(json, key);
	if (!p) return dflt;
	return strtol(p, NULL, 10);
}

static void ipc_send_hello(struct server *server, struct ipc_client *client) {
	size_t cap = 65536;
	char *buf = malloc(cap);
	if (!buf) return;
	size_t off = 0;
	off += snprintf(buf + off, cap - off, "{\"event\":\"hello\",\"focused\":%u,\"windows\":[",
		server->focused_view ? server->focused_view->id : 0);
	struct view *v;
	bool first = true;
	wl_list_for_each(v, &server->views, link) {
		if (!v->mapped) continue;
		char etitle[TITLE_MAX * 2], eapp[APPID_MAX * 2];
		json_escape(etitle, sizeof(etitle), v->title);
		json_escape(eapp, sizeof(eapp), v->app_id);
		off += snprintf(buf + off, cap - off,
			"%s{\"id\":%u,\"title\":\"%s\",\"app_id\":\"%s\",\"minimized\":%s,\"maximized\":%s,\"fullscreen\":%s}",
			first ? "" : ",", v->id, etitle, eapp,
			v->minimized ? "true" : "false",
			v->maximized ? "true" : "false",
			v->fullscreen ? "true" : "false");
		first = false;
		if (off > cap - 512) break;
	}
	snprintf(buf + off, cap - off, "]}\n");
	send(client->fd, buf, strlen(buf), MSG_NOSIGNAL);
	free(buf);
}

static void ipc_event_window_added(struct server *server, struct view *v) {
	char msg[1024], etitle[TITLE_MAX * 2], eapp[APPID_MAX * 2];
	json_escape(etitle, sizeof(etitle), v->title);
	json_escape(eapp, sizeof(eapp), v->app_id);
	snprintf(msg, sizeof(msg),
		"{\"event\":\"window_added\",\"id\":%u,\"title\":\"%s\",\"app_id\":\"%s\"}\n",
		v->id, etitle, eapp);
	ipc_broadcast(server, msg);
}

static void ipc_event_window_title(struct server *server, struct view *v) {
	char msg[1024], etitle[TITLE_MAX * 2];
	json_escape(etitle, sizeof(etitle), v->title);
	snprintf(msg, sizeof(msg), "{\"event\":\"window_title\",\"id\":%u,\"title\":\"%s\"}\n",
		v->id, etitle);
	ipc_broadcast(server, msg);
}

static void ipc_event_window_removed(struct server *server, uint32_t id) {
	char msg[128];
	snprintf(msg, sizeof(msg), "{\"event\":\"window_removed\",\"id\":%u}\n", id);
	ipc_broadcast(server, msg);
}

static void ipc_event_window_focused(struct server *server, uint32_t id) {
	char msg[128];
	snprintf(msg, sizeof(msg), "{\"event\":\"window_focused\",\"id\":%u}\n", id);
	ipc_broadcast(server, msg);
}

static void ipc_event_window_state(struct server *server, struct view *v) {
	char msg[256];
	snprintf(msg, sizeof(msg),
		"{\"event\":\"window_state\",\"id\":%u,\"minimized\":%s,\"maximized\":%s,\"fullscreen\":%s}\n",
		v->id, v->minimized ? "true" : "false",
		v->maximized ? "true" : "false", v->fullscreen ? "true" : "false");
	ipc_broadcast(server, msg);
}

static void ipc_event_super(struct server *server) {
	ipc_broadcast(server, "{\"event\":\"super\"}\n");
}

static void ipc_event_alt_tab(struct server *server, bool active) {
	char msg[64];
	snprintf(msg, sizeof(msg), "{\"event\":\"alt_tab\",\"active\":%s}\n",
		active ? "true" : "false");
	ipc_broadcast(server, msg);
}

/* ============================= utilities ============================== */

static struct view *view_by_id(struct server *server, uint32_t id) {
	struct view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->id == id) return v;
	}
	return NULL;
}

static struct output *output_from_wlr(struct server *server, struct wlr_output *wlr_output) {
	struct output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == wlr_output) return o;
	}
	return NULL;
}

static struct output *output_at(struct server *server, double lx, double ly) {
	struct wlr_output *wo = wlr_output_layout_output_at(server->output_layout, lx, ly);
	return wo ? output_from_wlr(server, wo) : NULL;
}

static struct output *primary_output(struct server *server) {
	if (wl_list_empty(&server->outputs)) return NULL;
	struct output *o;
	return wl_container_of(server->outputs.next, o, link);
}

static void view_get_geometry(struct view *v, struct wlr_box *box) {
	if (v->type == VIEW_XDG) {
		wlr_xdg_surface_get_geometry(v->xdg_toplevel->base, box);
	} else {
		box->x = 0;
		box->y = 0;
		box->width = v->xws->width;
		box->height = v->xws->height;
	}
}

/* layout coordinates of the window's visible top-left corner */
static void view_get_position(struct view *v, int *lx, int *ly) {
	struct wlr_box geo;
	view_get_geometry(v, &geo);
	*lx = v->scene_tree->node.x + geo.x;
	*ly = v->scene_tree->node.y + geo.y;
}

static void view_set_position(struct view *v, int lx, int ly) {
	struct wlr_box geo;
	view_get_geometry(v, &geo);
	wlr_scene_node_set_position(&v->scene_tree->node, lx - geo.x, ly - geo.y);
}

/* ========================= layer shell arrange ======================== */

static void arrange_layer_surface(struct server *server, struct layer_surface *ls,
		const struct wlr_box *bounds, struct wlr_box *usable) {
	struct wlr_layer_surface_v1 *l = ls->layer_surface;
	struct wlr_layer_surface_v1_state *state = &l->current;
	const uint32_t A_LEFT = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	const uint32_t A_RIGHT = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	const uint32_t A_TOP = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	const uint32_t A_BOTTOM = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;

	uint32_t width = state->desired_width;
	uint32_t height = state->desired_height;
	if ((state->anchor & A_LEFT) && (state->anchor & A_RIGHT)) {
		width = bounds->width - state->margin.left - state->margin.right;
	}
	if ((state->anchor & A_TOP) && (state->anchor & A_BOTTOM)) {
		height = bounds->height - state->margin.top - state->margin.bottom;
	}
	if (width < 1) width = 1;
	if (height < 1) height = 1;

	/* Some clients (e.g. fixed-height GTK windows) render a buffer smaller
	 * than the configured size; anchor the node using the real buffer size
	 * so it stays flush with the screen edge. */
	uint32_t placed_w = width, placed_h = height;
	if (l->surface->mapped && l->surface->current.width > 0) {
		placed_w = l->surface->current.width;
		placed_h = l->surface->current.height;
	}

	int x, y;
	if ((state->anchor & A_LEFT) && (state->anchor & A_RIGHT)) {
		x = bounds->x + state->margin.left;
	} else if (state->anchor & A_LEFT) {
		x = bounds->x + state->margin.left;
	} else if (state->anchor & A_RIGHT) {
		x = bounds->x + bounds->width - (int)placed_w - state->margin.right;
	} else {
		x = bounds->x + (bounds->width - (int)placed_w) / 2;
	}
	if ((state->anchor & A_TOP) && (state->anchor & A_BOTTOM)) {
		y = bounds->y + state->margin.top;
	} else if (state->anchor & A_TOP) {
		y = bounds->y + state->margin.top;
	} else if (state->anchor & A_BOTTOM) {
		y = bounds->y + bounds->height - (int)placed_h - state->margin.bottom;
	} else {
		y = bounds->y + (bounds->height - (int)placed_h) / 2;
	}

	wlr_layer_surface_v1_configure(l, width, height);
	wlr_scene_node_set_position(&ls->scene_tree->node, x, y);
}

static void apply_exclusive_zone(struct wlr_layer_surface_v1_state *state,
		struct wlr_box *usable) {
	const uint32_t A_LEFT = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	const uint32_t A_RIGHT = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	const uint32_t A_TOP = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	const uint32_t A_BOTTOM = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	if (state->exclusive_zone <= 0) return;
	int32_t z = state->exclusive_zone;
	bool left = state->anchor & A_LEFT, right = state->anchor & A_RIGHT;
	bool top = state->anchor & A_TOP, bottom = state->anchor & A_BOTTOM;
	if (left && !right) {
		z += state->margin.left;
		usable->x += z;
		usable->width -= z;
	} else if (right && !left) {
		z += state->margin.right;
		usable->width -= z;
	} else if (top && !bottom) {
		z += state->margin.top;
		usable->y += z;
		usable->height -= z;
	} else if (bottom && !top) {
		z += state->margin.bottom;
		usable->height -= z;
	}
}

static void arrange_output_layers(struct server *server, struct output *output) {
	struct wlr_box full;
	struct wlr_output_layout_output *lo =
		wlr_output_layout_get(server->output_layout, output->wlr_output);
	if (!lo) return;
	full.x = lo->x;
	full.y = lo->y;
	output->wlr_output->width; /* keep in sync below */
	full.width = output->wlr_output->width;
	full.height = output->wlr_output->height;

	struct wlr_box usable = full;

	/* pass 1: mapped exclusive surfaces donate their zone to the work area */
	struct layer_surface *ls;
	wl_list_for_each(ls, &server->layer_surfaces, link) {
		struct wlr_layer_surface_v1 *l = ls->layer_surface;
		if (l->output != output->wlr_output) continue;
		if (!l->initialized || !l->surface->mapped) continue;
		if (l->current.exclusive_zone > 0) {
			apply_exclusive_zone(&l->current, &usable);
		}
	}
	/* pass 2: configure every initialized surface so clients can attach
	 * buffers and map. Exclusive surfaces span the full output edge,
	 * everything else lives inside the work area. */
	wl_list_for_each(ls, &server->layer_surfaces, link) {
		struct wlr_layer_surface_v1 *l = ls->layer_surface;
		if (l->output != output->wlr_output) continue;
		if (!l->initialized) continue;
		if (l->current.exclusive_zone == -1 || l->current.exclusive_zone > 0) {
			arrange_layer_surface(server, ls, &full, NULL);
		} else {
			arrange_layer_surface(server, ls, &usable, NULL);
		}
	}
	output->usable_area = usable;
}

static void arrange_all_outputs(struct server *server) {
	struct output *o;
	wl_list_for_each(o, &server->outputs, link) {
		arrange_output_layers(server, o);
	}
}

/* ============================ focus handling ========================== */

static void focus_layer(struct server *server, struct layer_surface *ls) {
	if (!ls) return;
	struct wlr_seat *seat = server->seat;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	server->focused_layer = ls;
	wlr_scene_node_raise_to_top(&ls->scene_tree->node);
	if (kb) {
		wlr_seat_keyboard_notify_enter(seat, ls->layer_surface->surface,
			kb->keycodes, kb->num_keycodes, &kb->modifiers);
	}
}

static void focus_view(struct view *v, struct wlr_surface *surface) {
	if (v == NULL) return;
	struct server *server = v->server;
	struct wlr_seat *seat = server->seat;
	server->focused_layer = NULL;

	struct view *prev = server->focused_view;
	if (prev == v) {
		return;
	}
	if (prev) {
		if (prev->type == VIEW_XDG && prev->mapped) {
			wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
		} else if (prev->type == VIEW_XWAYLAND && prev->mapped) {
			wlr_xwayland_surface_activate(prev->xws, false);
		}
	}

	wlr_scene_node_raise_to_top(&v->scene_tree->node);
	wl_list_remove(&v->link);
	wl_list_insert(&server->views, &v->link);
	server->focused_view = v;

	if (v->type == VIEW_XDG) {
		wlr_xdg_toplevel_set_activated(v->xdg_toplevel, true);
	} else {
		wlr_xwayland_surface_activate(v->xws, true);
	}

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	struct wlr_surface *enter_surface = surface;
	if (!enter_surface) {
		enter_surface = (v->type == VIEW_XDG)
			? v->xdg_toplevel->base->surface : v->xws->surface;
	}
	if (kb && enter_surface) {
		wlr_seat_keyboard_notify_enter(seat, enter_surface,
			kb->keycodes, kb->num_keycodes, &kb->modifiers);
	}
	ipc_event_window_focused(server, v->id);
}

static void unfocus_all(struct server *server) {
	struct wlr_seat *seat = server->seat;
	if (server->focused_view) {
		if (server->focused_view->type == VIEW_XDG && server->focused_view->mapped) {
			wlr_xdg_toplevel_set_activated(server->focused_view->xdg_toplevel, false);
		}
	}
	server->focused_view = NULL;
	server->focused_layer = NULL;
	wlr_seat_keyboard_clear_focus(seat);
	ipc_event_window_focused(server, 0);
}

/* ========================== window management ========================= */

static void view_close(struct view *v) {
	if (!v) return;
	if (v->type == VIEW_XDG) {
		wlr_xdg_toplevel_send_close(v->xdg_toplevel);
	} else {
		wlr_xwayland_surface_close(v->xws);
	}
}

static void view_minimize(struct view *v, bool minimized) {
	if (!v || v->minimized == minimized) return;
	v->minimized = minimized;
	wlr_scene_node_set_enabled(&v->scene_tree->node, !minimized);
	if (v->type == VIEW_XWAYLAND) {
		wlr_xwayland_surface_set_minimized(v->xws, minimized);
	}
	if (minimized) {
		if (v->server->focused_view == v) {
			/* focus the next visible window if any */
			struct server *server = v->server;
			struct view *next;
			bool found = false;
			wl_list_for_each(next, &server->views, link) {
				if (next != v && next->mapped && !next->minimized) {
					focus_view(next, NULL);
					found = true;
					break;
				}
			}
			if (!found) unfocus_all(server);
		}
	} else {
		focus_view(v, NULL);
	}
	ipc_event_window_state(v->server, v);
}

static void view_toggle_maximize(struct view *v) {
	if (!v || !v->mapped || v->fullscreen) return;
	struct server *server = v->server;
	int lx, ly;
	view_get_position(v, &lx, &ly);
	struct output *o = output_at(server, lx, ly);
	if (!o) o = primary_output(server);
	if (!o) return;

	if (!v->maximized) {
		struct wlr_box geo;
		view_get_geometry(v, &geo);
		v->saved_geo.x = lx;
		v->saved_geo.y = ly;
		v->saved_geo.width = geo.width;
		v->saved_geo.height = geo.height;
		v->maximized = true;
		struct wlr_box *ua = &o->usable_area;
		if (v->type == VIEW_XDG) {
			wlr_xdg_toplevel_set_maximized(v->xdg_toplevel, true);
			wlr_xdg_toplevel_set_size(v->xdg_toplevel, ua->width, ua->height);
			view_set_position(v, ua->x, ua->y);
		} else {
			wlr_xwayland_surface_set_maximized(v->xws, true);
			wlr_xwayland_surface_configure(v->xws, ua->x, ua->y, ua->width, ua->height);
			wlr_scene_node_set_position(&v->scene_tree->node, ua->x, ua->y);
		}
	} else {
		v->maximized = false;
		if (v->type == VIEW_XDG) {
			wlr_xdg_toplevel_set_maximized(v->xdg_toplevel, false);
			wlr_xdg_toplevel_set_size(v->xdg_toplevel,
				v->saved_geo.width, v->saved_geo.height);
			view_set_position(v, v->saved_geo.x, v->saved_geo.y);
		} else {
			wlr_xwayland_surface_set_maximized(v->xws, false);
			wlr_xwayland_surface_configure(v->xws, v->saved_geo.x, v->saved_geo.y,
				v->saved_geo.width, v->saved_geo.height);
			wlr_scene_node_set_position(&v->scene_tree->node,
				v->saved_geo.x, v->saved_geo.y);
		}
	}
	ipc_event_window_state(server, v);
}

static void view_set_fullscreen(struct view *v, bool fs) {
	if (!v || !v->mapped || v->fullscreen == fs) return;
	struct server *server = v->server;
	int lx, ly;
	view_get_position(v, &lx, &ly);
	struct output *o = output_at(server, lx, ly);
	if (!o) o = primary_output(server);
	if (!o) return;

	if (fs) {
		if (!v->maximized) {
			struct wlr_box geo;
			view_get_geometry(v, &geo);
			v->saved_geo.x = lx;
			v->saved_geo.y = ly;
			v->saved_geo.width = geo.width;
			v->saved_geo.height = geo.height;
		}
		v->fullscreen = true;
		struct wlr_box full = { .x = o->usable_area.x, .y = o->usable_area.y };
		struct wlr_output_layout_output *lo =
			wlr_output_layout_get(server->output_layout, o->wlr_output);
		if (lo) { full.x = lo->x; full.y = lo->y; }
		full.width = o->wlr_output->width;
		full.height = o->wlr_output->height;
		if (v->type == VIEW_XDG) {
			wlr_xdg_toplevel_set_fullscreen(v->xdg_toplevel, true);
			wlr_xdg_toplevel_set_size(v->xdg_toplevel, full.width, full.height);
			view_set_position(v, full.x, full.y);
		} else {
			wlr_xwayland_surface_set_fullscreen(v->xws, true);
			wlr_xwayland_surface_configure(v->xws, full.x, full.y,
				full.width, full.height);
			wlr_scene_node_set_position(&v->scene_tree->node, full.x, full.y);
		}
		wlr_scene_node_raise_to_top(&v->scene_tree->node);
	} else {
		v->fullscreen = false;
		if (v->type == VIEW_XDG) {
			wlr_xdg_toplevel_set_fullscreen(v->xdg_toplevel, false);
			wlr_xdg_toplevel_set_size(v->xdg_toplevel,
				v->saved_geo.width, v->saved_geo.height);
			view_set_position(v, v->saved_geo.x, v->saved_geo.y);
		} else {
			wlr_xwayland_surface_set_fullscreen(v->xws, false);
			wlr_xwayland_surface_configure(v->xws, v->saved_geo.x, v->saved_geo.y,
				v->saved_geo.width, v->saved_geo.height);
			wlr_scene_node_set_position(&v->scene_tree->node,
				v->saved_geo.x, v->saved_geo.y);
		}
	}
	ipc_event_window_state(server, v);
}

/* ============================== keyboard ============================== */

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, modifiers);
	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat,
		&kb->wlr_keyboard->modifiers);
}

static void cycle_focus(struct server *server) {
	if (wl_list_length(&server->views) < 2) return;
	struct view *current = server->focused_view;
	struct view *start = current;
	struct wl_list *cursor_link = current ? current->link.next : server->views.next;
	for (;;) {
		if (cursor_link == &server->views) {
			cursor_link = cursor_link->next;
			continue;
		}
		struct view *v = wl_container_of(cursor_link, v, link);
		if (v->mapped && !v->minimized && v != start) {
			focus_view(v, NULL);
			return;
		}
		cursor_link = cursor_link->next;
		if (cursor_link == (current ? &current->link : server->views.next)) {
			return; /* full loop, nothing else */
		}
	}
}

static bool handle_keybinding(struct server *server, xkb_keysym_t sym) {
	/* Assumes Alt is held down. */
	switch (sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
	case XKB_KEY_Tab:
	case XKB_KEY_F1:
		cycle_focus(server);
		break;
	case XKB_KEY_F4:
		view_close(server->focused_view);
		break;
	default:
		return false;
	}
	return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, key);
	struct server *server = kb->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);

	bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

	for (int i = 0; i < nsyms; i++) {
		xkb_keysym_t sym = syms[i];
		bool is_super = (sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R);
		if (is_super) {
			handled = true; /* swallow Super itself */
			if (pressed && !server->super_held) {
				server->super_held = true;
				server->super_used = false;
			} else if (!pressed && server->super_held) {
				server->super_held = false;
				if (!server->super_used) {
					ipc_event_super(server);
				}
			}
			continue; /* swallow the Super key itself */
		}
		if (pressed && server->super_held) {
			server->super_used = true; /* let Super+key combos pass through */
		}
		bool is_alt = (sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R);
		if (is_alt) {
			if (pressed && !server->alt_held) {
				server->alt_held = true;
				ipc_event_alt_tab(server, true);
			} else if (!pressed && server->alt_held) {
				server->alt_held = false;
				ipc_event_alt_tab(server, false);
			}
		}
		if (pressed && (modifiers & WLR_MODIFIER_ALT)) {
			if (handle_keybinding(server, sym)) {
				handled = true;
			}
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, kb->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct keyboard *kb = wl_container_of(listener, kb, destroy);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
}

static void server_new_keyboard(struct server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct keyboard *kb = calloc(1, sizeof(*kb));
	kb->server = server;
	kb->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	kb->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &kb->modifiers);
	kb->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &kb->key);
	kb->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &kb->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	wl_list_insert(&server->keyboards, &kb->link);
}

static void server_new_pointer(struct server *server,
		struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

/* =============================== cursor =============================== */

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static struct view *desktop_view_at(struct server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}
	if (surface) *surface = scene_surface->surface;

	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	if (tree == NULL) return NULL;
	/* only nodes tagged with a struct view count; layer trees stay untagged */
	struct view *v = tree->node.data;
	if (tree == server->tree_toplevels) return NULL;
	return v;
}

static void reset_cursor_mode(struct server *server) {
	server->cursor_mode = CURSOR_PASSTHROUGH;
	server->grabbed_view = NULL;
}

static void process_cursor_move(struct server *server, uint32_t time) {
	struct view *v = server->grabbed_view;
	if (!v) return;
	int nx = (int)(server->cursor->x - server->grab_x);
	int ny = (int)(server->cursor->y - server->grab_y);
	struct wlr_box geo;
	view_get_geometry(v, &geo);
	wlr_scene_node_set_position(&v->scene_tree->node, nx, ny);
	if (v->type == VIEW_XWAYLAND) {
		wlr_xwayland_surface_configure(v->xws, nx, ny,
			v->xws->width, v->xws->height);
	}
	if (v->maximized) {
		v->maximized = false;
		if (v->type == VIEW_XDG) {
			wlr_xdg_toplevel_set_maximized(v->xdg_toplevel, false);
		} else {
			wlr_xwayland_surface_set_maximized(v->xws, false);
		}
		ipc_event_window_state(server, v);
	}
}

static void process_cursor_resize(struct server *server, uint32_t time) {
	struct view *v = server->grabbed_view;
	if (!v) return;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) new_top = new_bottom - 1;
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) new_bottom = new_top + 1;
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) new_left = new_right - 1;
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) new_right = new_left + 1;
	}

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	if (v->type == VIEW_XDG) {
		struct wlr_box geo_box;
		wlr_xdg_surface_get_geometry(v->xdg_toplevel->base, &geo_box);
		wlr_scene_node_set_position(&v->scene_tree->node,
			new_left - geo_box.x, new_top - geo_box.y);
		wlr_xdg_toplevel_set_size(v->xdg_toplevel, new_width, new_height);
	} else {
		wlr_scene_node_set_position(&v->scene_tree->node, new_left, new_top);
		wlr_xwayland_surface_configure(v->xws, new_left, new_top,
			new_width, new_height);
	}
}

static void process_cursor_motion(struct server *server, uint32_t time) {
	if (server->cursor_mode == CURSOR_MOVE) {
		process_cursor_move(server, time);
		return;
	} else if (server->cursor_mode == CURSOR_RESIZE) {
		process_cursor_resize(server, time);
		return;
	}

	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct view *v = desktop_view_at(server,
		server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!v && !surface) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	wlr_seat_pointer_notify_button(server->seat,
		event->time_msec, event->button, event->state);
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct view *v = desktop_view_at(server,
		server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (event->state == WLR_BUTTON_RELEASED) {
		reset_cursor_mode(server);
	} else if (surface) {
		struct wlr_layer_surface_v1 *ls =
			wlr_layer_surface_v1_try_from_wlr_surface(surface);
		if (ls && ls->current.keyboard_interactive !=
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
			struct layer_surface *lsurf = ls->data;
			if (lsurf) focus_layer(server, lsurf);
		} else if (v) {
			focus_view(v, surface);
		}
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete, event->source);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* =============================== output =============================== */

static void output_frame(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		output->server->scene, output->wlr_output);
	wlr_scene_output_commit(scene_output, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
	output->usable_area.width = wlr_output->width;
	output->usable_area.height = wlr_output->height;

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
}

/* ============================ layer shell ============================= */

static struct wlr_scene_tree *layer_tree_for(struct server *server, uint32_t layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return server->tree_bg;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return server->tree_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return server->tree_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY: return server->tree_overlay;
	}
	return server->tree_top;
}

static void layer_surface_map(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, map);
	wlr_log(WLR_INFO, "layer surface mapped: ns=%s buf=%dx%d node=%d,%d",
		ls->layer_surface->namespace ? ls->layer_surface->namespace : "?",
		ls->layer_surface->surface->current.width,
		ls->layer_surface->surface->current.height,
		ls->scene_tree->node.x, ls->scene_tree->node.y);
	struct server *server = ls->server;
	if (!ls->layer_surface->output) {
		struct output *o = primary_output(server);
		if (o) ls->layer_surface->output = o->wlr_output;
	}
	arrange_all_outputs(server);
	wlr_scene_node_raise_to_top(&ls->scene_tree->node);
	if (ls->layer_surface->current.keyboard_interactive !=
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		focus_layer(server, ls);
	}
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, unmap);
	struct server *server = ls->server;
	if (server->focused_layer == ls) {
		server->focused_layer = NULL;
		if (server->focused_view && server->focused_view->mapped) {
			focus_view(server->focused_view, NULL);
		} else {
			wlr_seat_keyboard_clear_focus(server->seat);
		}
	}
	arrange_all_outputs(server);
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, destroy);
	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->new_popup.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->link);
	/* The scene node is owned by wlr_scene_layer_surface_v1, which tears
	 * it down itself when the layer surface is destroyed. */
	free(ls);
}

static void layer_surface_new_popup(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, new_popup);
	struct wlr_xdg_popup *popup = data;
	struct wlr_scene_tree *tree =
		wlr_scene_xdg_surface_create(ls->scene_tree, popup->base);
	popup->base->data = tree;
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	struct layer_surface *ls = wl_container_of(listener, ls, commit);
	struct wlr_layer_surface_v1 *l = ls->layer_surface;
	if (l->initial_commit) {
		if (!l->output) {
			struct output *o = primary_output(ls->server);
			if (o) l->output = o->wlr_output;
		}
		arrange_all_outputs(ls->server);
		return;
	}
	uint32_t geo_fields =
		WLR_LAYER_SURFACE_V1_STATE_DESIRED_SIZE |
		WLR_LAYER_SURFACE_V1_STATE_ANCHOR |
		WLR_LAYER_SURFACE_V1_STATE_EXCLUSIVE_ZONE |
		WLR_LAYER_SURFACE_V1_STATE_MARGIN |
		WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY |
		WLR_LAYER_SURFACE_V1_STATE_LAYER;
	if (l->current.committed & geo_fields) {
		arrange_all_outputs(ls->server);
	}
}

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *wlr_ls = data;

	if (!wlr_ls->output) {
		struct output *o = primary_output(server);
		if (o) wlr_ls->output = o->wlr_output;
	}

	struct layer_surface *ls = calloc(1, sizeof(*ls));
	ls->server = server;
	ls->layer_surface = wlr_ls;
	wlr_ls->data = ls;

	struct wlr_scene_layer_surface_v1 *scene_ls =
		wlr_scene_layer_surface_v1_create(
			layer_tree_for(server, wlr_ls->pending.layer), wlr_ls);
	ls->scene_tree = scene_ls->tree;

	ls->map.notify = layer_surface_map;
	wl_signal_add(&wlr_ls->surface->events.map, &ls->map);
	ls->unmap.notify = layer_surface_unmap;
	wl_signal_add(&wlr_ls->surface->events.unmap, &ls->unmap);
	ls->destroy.notify = layer_surface_destroy;
	wl_signal_add(&wlr_ls->events.destroy, &ls->destroy);
	ls->new_popup.notify = layer_surface_new_popup;
	wl_signal_add(&wlr_ls->events.new_popup, &ls->new_popup);
	ls->commit.notify = layer_surface_commit;
	wl_signal_add(&wlr_ls->surface->events.commit, &ls->commit);

	wl_list_insert(&server->layer_surfaces, &ls->link);

	wlr_log(WLR_INFO, "layer surface created: ns=%s layer=%u anchor=%u w=%u h=%u excl=%d",
		wlr_ls->namespace ? wlr_ls->namespace : "?",
		wlr_ls->pending.layer, wlr_ls->pending.anchor,
		wlr_ls->pending.desired_width, wlr_ls->pending.desired_height,
		wlr_ls->pending.exclusive_zone);

	/* wlroots emits new_surface during the first commit, after it was
	 * already processed — arrange right away so the client receives its
	 * initial configure and can attach a buffer. */
	arrange_all_outputs(server);
}

/* ============================== xdg-shell ============================= */

static void begin_interactive(struct view *v, enum cursor_mode mode,
		uint32_t edges) {
	struct server *server = v->server;
	struct wlr_surface *focused_surface =
		server->seat->pointer_state.focused_surface;
	struct wlr_surface *view_root = NULL;
	if (v->type == VIEW_XDG) {
		view_root = v->xdg_toplevel->base->surface;
	} else if (v->xws->surface) {
		view_root = v->xws->surface;
	}
	if (view_root && focused_surface &&
			view_root != wlr_surface_get_root_surface(focused_surface)) {
		return;
	}
	server->grabbed_view = v;
	server->cursor_mode = mode;

	if (mode == CURSOR_MOVE) {
		server->grab_x = server->cursor->x - v->scene_tree->node.x;
		server->grab_y = server->cursor->y - v->scene_tree->node.y;
	} else {
		struct wlr_box geo_box;
		view_get_geometry(v, &geo_box);
		double border_x = (v->scene_tree->node.x + geo_box.x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
		double border_y = (v->scene_tree->node.y + geo_box.y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;
		server->grab_geobox = geo_box;
		server->grab_geobox.x += v->scene_tree->node.x;
		server->grab_geobox.y += v->scene_tree->node.y;
		server->resize_edges = edges;
	}
}

static void xdg_view_map(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, map);
	v->mapped = true;
	wl_list_insert(&v->server->views, &v->link);
	if (v->type == VIEW_XDG && v->xdg_toplevel->parent == NULL) {
		/* cascade placement for new top-level windows */
		int offset = 48 * (v->id % 6);
		view_set_position(v, 96 + offset, 64 + offset);
	}
	snprintf(v->app_id, sizeof(v->app_id), "%s",
		(v->type == VIEW_XDG && v->xdg_toplevel->app_id)
			? v->xdg_toplevel->app_id
			: ((v->type == VIEW_XWAYLAND && v->xws->class) ? v->xws->class : ""));
	if (v->type == VIEW_XDG && v->xdg_toplevel->title) {
		snprintf(v->title, sizeof(v->title), "%s", v->xdg_toplevel->title);
	} else if (v->type == VIEW_XWAYLAND && v->xws->title) {
		snprintf(v->title, sizeof(v->title), "%s", v->xws->title);
	}
	ipc_event_window_added(v->server, v);
	if (!(v->type == VIEW_XWAYLAND && v->xws->override_redirect)) {
		focus_view(v, NULL);
	}
}

static void xdg_view_unmap(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, unmap);
	struct server *server = v->server;
	if (!v->mapped) return;
	v->mapped = false;
	if (server->grabbed_view == v) reset_cursor_mode(server);
	wl_list_remove(&v->link);
	wl_list_init(&v->link);
	ipc_event_window_removed(server, v->id);
	if (server->focused_view == v) {
		server->focused_view = NULL;
		struct view *next;
		bool found = false;
		wl_list_for_each(next, &server->views, link) {
			if (next->mapped && !next->minimized) {
				focus_view(next, NULL);
				found = true;
				break;
			}
		}
		if (!found) {
			wlr_seat_keyboard_clear_focus(server->seat);
			ipc_event_window_focused(server, 0);
		}
	}
}

static void xdg_view_destroy(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, destroy);
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->set_title.link);
	wl_list_remove(&v->set_app_id.link);
	wl_list_remove(&v->request_move.link);
	wl_list_remove(&v->request_resize.link);
	wl_list_remove(&v->request_maximize.link);
	wl_list_remove(&v->request_fullscreen.link);
	if (v->link.next) wl_list_remove(&v->link);
	free(v);
}

static void xdg_view_set_title(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, set_title);
	const char *title = NULL;
	if (v->type == VIEW_XDG) title = v->xdg_toplevel->title;
	else title = v->xws->title;
	snprintf(v->title, sizeof(v->title), "%s", title ? title : "");
	ipc_event_window_title(v->server, v);
}

static void xdg_view_set_app_id(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, set_app_id);
	const char *app = NULL;
	if (v->type == VIEW_XDG) app = v->xdg_toplevel->app_id;
	else app = v->xws->class;
	snprintf(v->app_id, sizeof(v->app_id), "%s", app ? app : "");
}

static void xdg_view_request_move(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, request_move);
	begin_interactive(v, CURSOR_MOVE, 0);
}

static void xdg_view_request_resize(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct view *v = wl_container_of(listener, v, request_resize);
	begin_interactive(v, CURSOR_RESIZE, event->edges);
}

static void xdg_view_request_maximize(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, request_maximize);
	if (v->type == VIEW_XDG) {
		bool want = v->xdg_toplevel->requested.maximized;
		if (want != v->maximized) view_toggle_maximize(v);
		else wlr_xdg_surface_schedule_configure(v->xdg_toplevel->base);
	} else {
		view_toggle_maximize(v);
	}
}

static void xdg_view_request_fullscreen(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, request_fullscreen);
	bool want = false;
	if (v->type == VIEW_XDG) want = v->xdg_toplevel->requested.fullscreen;
	else want = v->xws->fullscreen;
	if (want != v->fullscreen) view_set_fullscreen(v, want);
	else if (v->type == VIEW_XDG) {
		wlr_xdg_surface_schedule_configure(v->xdg_toplevel->base);
	}
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, new_xdg_surface);
	struct wlr_xdg_surface *xdg_surface = data;

	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_surface *parent =
			wlr_xdg_surface_try_from_wlr_surface(xdg_surface->popup->parent);
		assert(parent != NULL);
		struct wlr_scene_tree *parent_tree = parent->data;
		xdg_surface->data = wlr_scene_xdg_surface_create(parent_tree, xdg_surface);
		return;
	}
	assert(xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);

	struct view *v = calloc(1, sizeof(*v));
	v->server = server;
	v->type = VIEW_XDG;
	v->id = server->next_view_id++;
	v->xdg_toplevel = xdg_surface->toplevel;
	wl_list_init(&v->link);
	v->scene_tree = wlr_scene_xdg_surface_create(
		server->tree_toplevels, xdg_surface);
	v->scene_tree->node.data = v;
	xdg_surface->data = v->scene_tree;

	v->map.notify = xdg_view_map;
	wl_signal_add(&xdg_surface->surface->events.map, &v->map);
	v->unmap.notify = xdg_view_unmap;
	wl_signal_add(&xdg_surface->surface->events.unmap, &v->unmap);
	v->destroy.notify = xdg_view_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &v->destroy);
	v->set_title.notify = xdg_view_set_title;
	wl_signal_add(&xdg_surface->toplevel->events.set_title, &v->set_title);
	v->set_app_id.notify = xdg_view_set_app_id;
	wl_signal_add(&xdg_surface->toplevel->events.set_app_id, &v->set_app_id);
	v->request_move.notify = xdg_view_request_move;
	wl_signal_add(&xdg_surface->toplevel->events.request_move, &v->request_move);
	v->request_resize.notify = xdg_view_request_resize;
	wl_signal_add(&xdg_surface->toplevel->events.request_resize, &v->request_resize);
	v->request_maximize.notify = xdg_view_request_maximize;
	wl_signal_add(&xdg_surface->toplevel->events.request_maximize, &v->request_maximize);
	v->request_fullscreen.notify = xdg_view_request_fullscreen;
	wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen, &v->request_fullscreen);
}

/* ============================== XWayland ============================== */

static void xw_view_request_configure(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, request_configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	wlr_xwayland_surface_configure(v->xws, event->x, event->y,
		event->width, event->height);
	if (v->mapped) {
		wlr_scene_node_set_position(&v->scene_tree->node, event->x, event->y);
	}
}

static void xw_view_request_minimize(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, request_minimize);
	struct wlr_xwayland_minimize_event *event = data;
	view_minimize(v, event->minimize);
}

static void xw_view_request_activate(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, request_activate);
	focus_view(v, NULL);
}

static void xw_view_surface_map(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, surface_map);
	if (!v->scene_content && v->xws->surface) {
		v->scene_content = wlr_scene_subsurface_tree_create(
			v->scene_tree, v->xws->surface);
	}
	wlr_scene_node_set_position(&v->scene_tree->node, v->xws->x, v->xws->y);
	xdg_view_map(&v->map, NULL);
}

static void xw_view_surface_unmap(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, surface_unmap);
	xdg_view_unmap(&v->unmap, NULL);
	if (v->scene_content) {
		wlr_scene_node_destroy(&v->scene_content->node);
		v->scene_content = NULL;
	}
}

static void xw_view_associate(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, associate);
	if (v->surface_listeners_bound) return;
	v->surface_listeners_bound = true;
	v->surface_map.notify = xw_view_surface_map;
	wl_signal_add(&v->xws->surface->events.map, &v->surface_map);
	v->surface_unmap.notify = xw_view_surface_unmap;
	wl_signal_add(&v->xws->surface->events.unmap, &v->surface_unmap);
}

static void xw_view_dissociate(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, dissociate);
	if (v->mapped) {
		xdg_view_unmap(&v->unmap, NULL);
	}
	if (v->scene_content) {
		wlr_scene_node_destroy(&v->scene_content->node);
		v->scene_content = NULL;
	}
	if (v->surface_listeners_bound) {
		wl_list_remove(&v->surface_map.link);
		wl_list_remove(&v->surface_unmap.link);
		v->surface_listeners_bound = false;
	}
}

static void xw_view_destroy(struct wl_listener *listener, void *data) {
	struct view *v = wl_container_of(listener, v, destroy);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->set_title.link);
	wl_list_remove(&v->set_app_id.link);
	wl_list_remove(&v->request_move.link);
	wl_list_remove(&v->request_resize.link);
	wl_list_remove(&v->request_maximize.link);
	wl_list_remove(&v->request_fullscreen.link);
	wl_list_remove(&v->request_minimize.link);
	wl_list_remove(&v->request_configure.link);
	wl_list_remove(&v->request_activate.link);
	wl_list_remove(&v->associate.link);
	wl_list_remove(&v->dissociate.link);
	if (v->surface_listeners_bound) {
		wl_list_remove(&v->surface_map.link);
		wl_list_remove(&v->surface_unmap.link);
	}
	if (v->link.next) wl_list_remove(&v->link);
	free(v);
}

static void server_new_xwayland_surface(struct wl_listener *listener, void *data) {
	struct server *server =
		wl_container_of(listener, server, xwayland_new_surface);
	struct wlr_xwayland_surface *xws = data;

	struct view *v = calloc(1, sizeof(*v));
	v->server = server;
	v->type = VIEW_XWAYLAND;
	v->id = server->next_view_id++;
	v->xws = xws;
	xws->data = v;
	wl_list_init(&v->link);
	v->scene_tree = wlr_scene_tree_create(server->tree_toplevels);
	v->scene_tree->node.data = v;

	v->destroy.notify = xw_view_destroy;
	wl_signal_add(&xws->events.destroy, &v->destroy);
	v->set_title.notify = xdg_view_set_title;
	wl_signal_add(&xws->events.set_title, &v->set_title);
	v->set_app_id.notify = xdg_view_set_app_id;
	wl_signal_add(&xws->events.set_class, &v->set_app_id);
	v->request_move.notify = xdg_view_request_move;
	wl_signal_add(&xws->events.request_move, &v->request_move);
	v->request_resize.notify = xdg_view_request_resize;
	wl_signal_add(&xws->events.request_resize, &v->request_resize);
	v->request_maximize.notify = xdg_view_request_maximize;
	wl_signal_add(&xws->events.request_maximize, &v->request_maximize);
	v->request_fullscreen.notify = xdg_view_request_fullscreen;
	wl_signal_add(&xws->events.request_fullscreen, &v->request_fullscreen);
	v->request_minimize.notify = xw_view_request_minimize;
	wl_signal_add(&xws->events.request_minimize, &v->request_minimize);
	v->request_configure.notify = xw_view_request_configure;
	wl_signal_add(&xws->events.request_configure, &v->request_configure);
	v->request_activate.notify = xw_view_request_activate;
	wl_signal_add(&xws->events.request_activate, &v->request_activate);
	v->associate.notify = xw_view_associate;
	wl_signal_add(&xws->events.associate, &v->associate);
	v->dissociate.notify = xw_view_dissociate;
	wl_signal_add(&xws->events.dissociate, &v->dissociate);
}

static void xwayland_ready(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, xwayland_ready);
	setenv("DISPLAY", server->xwayland->display_name, true);
	wlr_log(WLR_INFO, "XWayland ready on DISPLAY=%s",
		server->xwayland->display_name);
}

/* =========================== xdg-decoration =========================== */

static void new_decorator(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *dec = data;
	wlr_xdg_toplevel_decoration_v1_set_mode(dec,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

/* =========================== xdg-activation =========================== */

static void activation_request(struct wl_listener *listener, void *data) {
	struct server *server = wl_container_of(listener, server, activation_request);
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct wlr_xdg_surface *xdg_surface =
		wlr_xdg_surface_try_from_wlr_surface(event->surface);
	if (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		struct wlr_scene_tree *tree = xdg_surface->data;
		if (tree && tree->node.data) {
			focus_view((struct view *)tree->node.data, event->surface);
		}
		return;
	}
	struct wlr_xwayland_surface *xws =
		wlr_xwayland_surface_try_from_wlr_surface(event->surface);
	if (xws && xws->data) {
		focus_view((struct view *)xws->data, event->surface);
	}
}

/* ============================ IPC handlers ============================ */

static void ipc_client_destroy(struct ipc_client *client) {
	wl_list_remove(&client->link);
	wl_event_source_remove(client->src);
	close(client->fd);
	free(client);
}

static void ipc_handle_line(struct server *server, struct ipc_client *client,
		const char *line) {
	char cmd[32] = {0};
	if (!json_get_str(line, "cmd", cmd, sizeof(cmd))) return;
	uint32_t id = (uint32_t)json_get_int(line, "id", 0);
	struct view *v = id ? view_by_id(server, id) : NULL;

	if (strcmp(cmd, "hello") == 0 || strcmp(cmd, "list") == 0) {
		ipc_send_hello(server, client);
	} else if (strcmp(cmd, "focus") == 0 && v) {
		if (v->minimized) view_minimize(v, false);
		else focus_view(v, NULL);
	} else if (strcmp(cmd, "close") == 0 && v) {
		view_close(v);
	} else if (strcmp(cmd, "minimize") == 0 && v) {
		view_minimize(v, true);
	} else if (strcmp(cmd, "restore") == 0 && v) {
		view_minimize(v, false);
	} else if (strcmp(cmd, "maximize") == 0 && v) {
		view_toggle_maximize(v);
	} else if (strcmp(cmd, "fullscreen") == 0 && v) {
		view_set_fullscreen(v, !v->fullscreen);
	} else if (strcmp(cmd, "quit") == 0) {
		wl_display_terminate(server->wl_display);
	}
}

static int ipc_client_readable(int fd, uint32_t mask, void *data) {
	struct ipc_client *client = data;
	struct server *server = client->server;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		ipc_client_destroy(client);
		return 0;
	}
	ssize_t n = recv(fd, client->buf + client->len,
		sizeof(client->buf) - client->len - 1, 0);
	if (n <= 0) {
		ipc_client_destroy(client);
		return 0;
	}
	client->len += (size_t)n;
	client->buf[client->len] = 0;
	char *start = client->buf;
	char *nl;
	while ((nl = strchr(start, '\n')) != NULL) {
		*nl = 0;
		ipc_handle_line(server, client, start);
		start = nl + 1;
	}
	client->len = strlen(start);
	memmove(client->buf, start, client->len + 1);
	return 0;
}

static int ipc_accept(int fd, uint32_t mask, void *data) {
	struct server *server = data;
	int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (cfd < 0) return 0;
	struct ipc_client *client = calloc(1, sizeof(*client));
	client->server = server;
	client->fd = cfd;
	client->src = wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display),
		cfd, WL_EVENT_READABLE, ipc_client_readable, client);
	wl_list_insert(&server->ipc_clients, &client->link);
	ipc_send_hello(server, client);
	return 0;
}

static bool ipc_init(struct server *server) {
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) runtime_dir = "/tmp";
	char path[IPC_PATH_MAX];
	snprintf(path, sizeof(path), "%s/elevende-ipc.sock", runtime_dir);
	unlink(path);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return false;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
			listen(fd, 8) < 0) {
		close(fd);
		return false;
	}
	server->ipc_fd = fd;
	server->ipc_src = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->wl_display),
		fd, WL_EVENT_READABLE, ipc_accept, server);
	wlr_log(WLR_INFO, "IPC listening on %s", path);
	return true;
}

/* ================================ main ================================ */

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_INFO, NULL);
	signal(SIGPIPE, SIG_IGN);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}

	struct server server = {0};
	server.wl_display = wl_display_create();
	server.backend = wlr_backend_autocreate(server.wl_display, NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	server.compositor = wlr_compositor_create(server.wl_display, 5,
		server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	/* protocol extras that mainstream apps expect */
	wlr_viewporter_create(server.wl_display);
	wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
	wlr_presentation_create(server.wl_display, server.backend);
	wlr_data_control_manager_v1_create(server.wl_display);
	wlr_primary_selection_v1_device_manager_create(server.wl_display);
	wlr_export_dmabuf_manager_v1_create(server.wl_display);
	wlr_screencopy_manager_v1_create(server.wl_display);
	wlr_gamma_control_manager_v1_create(server.wl_display);

	server.output_layout = wlr_output_layout_create();
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

	server.scene = wlr_scene_create();
	server.scene_layout =
		wlr_scene_attach_output_layout(server.scene, server.output_layout);
	server.tree_bg = wlr_scene_tree_create(&server.scene->tree);
	server.tree_bottom = wlr_scene_tree_create(&server.scene->tree);
	server.tree_toplevels = wlr_scene_tree_create(&server.scene->tree);
	server.tree_top = wlr_scene_tree_create(&server.scene->tree);
	server.tree_overlay = wlr_scene_tree_create(&server.scene->tree);

	/* xdg-shell */
	wl_list_init(&server.views);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_surface.notify = server_new_xdg_surface;
	wl_signal_add(&server.xdg_shell->events.new_surface,
		&server.new_xdg_surface);

	/* layer-shell */
	wl_list_init(&server.layer_surfaces);
	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface,
		&server.new_layer_surface);

	/* XWayland */
	server.xwayland = wlr_xwayland_create(server.wl_display,
		server.compositor, false);
	if (server.xwayland) {
		server.xwayland_ready.notify = xwayland_ready;
		wl_signal_add(&server.xwayland->events.ready, &server.xwayland_ready);
		server.xwayland_new_surface.notify = server_new_xwayland_surface;
		wl_signal_add(&server.xwayland->events.new_surface,
			&server.xwayland_new_surface);
	} else {
		wlr_log(WLR_ERROR, "failed to create XWayland; X11 apps unavailable");
	}

	/* decorations: prefer client-side (GTK/Qt native look) */
	server.deco_mgr = wlr_xdg_decoration_manager_v1_create(server.wl_display);
	server.new_decorator.notify = new_decorator;
	wl_signal_add(&server.deco_mgr->events.new_toplevel_decoration,
		&server.new_decorator);

	/* activation (taskbar launches etc.) */
	server.activation = wlr_xdg_activation_v1_create(server.wl_display);
	server.activation_request.notify = activation_request;
	wl_signal_add(&server.activation->events.request_activate,
		&server.activation_request);

	/* cursor */
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	server.cursor_mode = CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
		&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/* seat */
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
		&server.request_cursor);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
		&server.request_set_selection);

	/* IPC with the shell */
	wl_list_init(&server.ipc_clients);
	if (!ipc_init(&server)) {
		wlr_log(WLR_ERROR, "failed to initialize IPC socket");
	}

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
			_exit(1);
		}
	}
	wlr_log(WLR_INFO, "ElevenDE compositor running on WAYLAND_DISPLAY=%s", socket);
	ipc_broadcast(&server, "{\"event\":\"ready\"}\n");
	wl_display_run(server.wl_display);

	wl_display_destroy_clients(server.wl_display);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_output_layout_destroy(server.output_layout);
	wl_display_destroy(server.wl_display);
	return 0;
}
