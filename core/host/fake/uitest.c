// uitest.c: testing an app's UI on the fake host (uitest.h), and the
// test.* builtins `asuka test` runs test blocks with.
#include "fake/uitest.h"
#include "vm/vm.h"
#include "rt/sched.h"
#include "base/thread.h"
#include "ui/ui_ext.h"
#include "signal/signal.h"
#include "native/native.h"

#include <math.h>
#include <stdio.h>

// A List's rows, bound as a collection view binds them: those that fit
// the list's height from where it's scrolled (or back from its end, for a
// list anchored there), and a line more.
typedef struct {
	int64_t index;
	ak_fake_view *cell;
} bound;

typedef struct {
	ak_id list;
	AK_VEC(bound) cells;  // by index, sorted
	AK_VEC(bound) headers;  // the sections' headers, by section, sorted: those whose rows show
	ak_fake_view_vec free;
	int64_t first;
	bool end;
	bool scrolled;  // a test scrolled it (scroll_to): it stays where that left it
	bool seen;
	AK_VEC(int64_t) items;  // the list's items as last bound
	int64_t count;
} list_state;

// A scope narrows what a text names (app.in(…), app.nextTo(…)): a part
// of the screen (IN_*), what's in a view, what's next to a label.
typedef struct {
	uint8_t region;
	ak_fake_view *under;
	ak_str *next_to;  // borrowed from the scoped handle
} scope;

struct ak_uitest {
	ak_opaque h;
	ak_fake *f;
	bool mounted;
	ak_fake_view *focused;  // the field tapped or typed in last
	ak_id focused_id, menu_id;  // their IDs: a view that went is forgotten
	// The open menu: its view, the prop it's in, the base its items'
	// numbers have in Action events, and the submenu showing (-1: none).
	ak_fake_view *menu;
	ak_prop menu_prop;
	int64_t menu_base, menu_sub;
	AK_VEC(list_state) lists;
	AK_VEC(ak_id) sheets;  // the sheets whose environment was reported
	ak_buf fails;  // the fake host's misuses
	uint32_t ops_seen;  // the ops test.ops returned already
	scope scope;  // the call's (a scoped handle's)
};

// The cap on a list's bound rows: rows of no height would bind them all.
#define MAX_BOUND 200

static ak_uitest *active;  // the app posts wake

static ak_span sp(const char *s) { return ak_cstr(s); }

// ---- The fake host, connected ----

static void ev_cb(void *ctx, const uint8_t *buf, size_t len) {
	AK_UNUSED(ctx);
	ak_ui_dispatch(buf, len);
}
static int64_t count_cb(void *ctx, ak_id list) {
	AK_UNUSED(ctx);
	return ak_ui_list_count(list);
}
static void bind_cb(void *ctx, ak_id list, int64_t index, ak_id cell) {
	AK_UNUSED(ctx);
	ak_ui_list_bind(list, index, cell);
}
static void unbind_cb(void *ctx, ak_id list, ak_id cell) {
	AK_UNUSED(ctx);
	ak_ui_list_unbind(list, cell);
}
static void posted_cb(void *ctx) {
	AK_UNUSED(ctx);
	ak_ui_run_posted();
}
static void apply_cb(void *ctx, const uint8_t *buf, size_t len) { ak_fake_apply(ctx, buf, len); }
static void measure_cb(void *ctx, uint32_t id, float w, float h, float aw, float ah, float *ow, float *oh) {
	ak_fake_measure(ctx, id, w, h, aw, ah, ow, oh);
}
static void fail_cb(void *ctx, const char *msg) {
	ak_buf *b = ctx;
	ak_buf_put(b, msg, strlen(msg));
	ak_buf_u8(b, '\n');
}
static void wake(void) {
	if (active) ak_fake_post(active->f);
}

static void free_lists(ak_uitest *a) {
	for (uint32_t i = 0; i < a->lists.len; i++) {
		ak_vec_free(&a->lists.data[i].cells);
		ak_vec_free(&a->lists.data[i].headers);
		ak_vec_free(&a->lists.data[i].free);
		ak_vec_free(&a->lists.data[i].items);
	}
	ak_vec_free(&a->lists);
}

static void uitest_drop(ak_opaque *o) {
	ak_uitest *a = (ak_uitest *)o;
	if (a->mounted) ak_uitest_unmount(a, NULL);
	free_lists(a);
	ak_vec_free(&a->sheets);
	ak_fake_free(a->f);
	ak_buf_free(&a->fails);
	if (active == a) active = NULL;
}

static uint8_t default_platform = AK_PLATFORM_IOS;

void ak_uitest_set_platform(uint8_t platform) { default_platform = platform; }

static ak_fake_measure_fn default_measure;
static void *default_measure_ctx;

void ak_uitest_set_measure(ak_fake_measure_fn fn, void *ctx) { default_measure = fn, default_measure_ctx = ctx; }

ak_uitest *ak_uitest_mount(ak_closure *build, const ak_environment *env) { return ak_uitest_mount_at(build, env, -1, sp("")); }

ak_uitest *ak_uitest_mount_at(ak_closure *build, const ak_environment *env, int platform, ak_span href) {
	if (active && active->mounted) ak_uitest_unmount(active, NULL);
	ak_uitest *a = ak_opaque_new(AK_TAG_OPAQUE, sizeof(ak_uitest), uitest_drop);
	a->f = ak_fake_new();
	ak_fake *f = a->f;
	if (env) f->env = *env;
	else f->env.platform = default_platform;
	if (platform >= 0) f->env.platform = (uint8_t)platform;
	f->core = (ak_fake_core){a, ev_cb, count_cb, bind_cb, unbind_cb, posted_cb};
	f->fail_fn = fail_cb;
	f->fail_ctx = &a->fails;
	f->measure_fn = default_measure, f->measure_ctx = default_measure_ctx;
	active = a;
	ak_zone_set_utc(true);  // dates are UTC's: the same on every computer
	ak_locale_forget_choice();  // the user's languages, whatever a test before chose
	ak_sched_set_wake(wake);
	ak_ui_init(&(ak_host){f, apply_cb, measure_cb});
	ak_ui_set_environment(&f->env);
	if (href.len) ak_router_set_start(href);
	ak_ui_mount(build);
	ak_router_set_start(sp(""));
	a->mounted = true;
	ak_uitest_settle(a);
	return a;
}

bool ak_uitest_unmount(ak_uitest *a, ak_buf *err) {
	if (!a->mounted) return true;
	a->mounted = false;
	ak_ui_unmount();
	ak_fake_run_posted(a->f);
	// Every view the core made goes with the app.
	ak_fake_view_vec left = {0};
	ak_fake *f = a->f;
	for (uint32_t i = 0; i < f->slots_cap; i++) {
		ak_fake_view *v = f->slots[i];
		if (v && !(v->id & AK_HOST_ID_BIT)) ak_vec_push(&left, v);
	}
	bool ok = left.len == 0;
	if (!ok && err) {
		ak_buf_printf(err, "%u views outlived the app (an owner left undisposed):\n", left.len);
		for (uint32_t i = 0; i < left.len; i++) {
			ak_buf_put(err, "  ", 2);
			ak_fake_kind_name(err, left.data[i]->kind);
			ak_buf_printf(err, " %u\n", left.data[i]->id);
		}
	}
	ak_vec_free(&left);
	return ok;
}

ak_fake *ak_uitest_host(ak_uitest *a) { return a->f; }

// ---- What the screen shows ----
//
// The layers (the root, then what's presented over
// it), the views in each that show (a Stack's top screen, a Tabs' chosen
// page, a List's bound rows), and the targets a text names in them.

static bool hidden(const ak_fake_view *v) { return ak_fake_bool(v, AK_PROP_HIDDEN); }

static list_state *list_of(ak_uitest *a, ak_id id) {
	for (uint32_t i = 0; i < a->lists.len; i++)
		if (a->lists.data[i].list == id) return &a->lists.data[i];
	return NULL;
}

typedef bool (*walk_fn)(ak_uitest *a, ak_fake_view *v, void *ctx);

// header_start is the first row of a list's section s.
static int64_t header_start(const ak_fake_view *list, int64_t s) {
	return s >= 0 && s < list->sections.len ? list->sections.data[s].start : INT64_MAX;
}

// walk calls f on each view under v that shows, depth first, parents
// first; f returning false skips the view's children.
static void walk(ak_uitest *a, ak_fake_view *v, walk_fn f, void *ctx) {
	if (hidden(v) || !f(a, v, ctx)) return;
	if (v->kind == AK_KIND_STACK) {
		if (v->children.len) walk(a, v->children.data[v->children.len - 1], f, ctx);
		return;
	}
	if (v->kind == AK_KIND_TABS) {
		int64_t i = ak_fake_int(v, AK_PROP_TABS_SELECTED);
		if (i >= 0 && i < v->children.len) walk(a, v->children.data[i], f, ctx);
		return;
	}
	for (uint32_t i = 0; i < v->children.len; i++) walk(a, v->children.data[i], f, ctx);
	if (v->kind == AK_KIND_LIST) {
		list_state *st = list_of(a, v->id);
		uint32_t h = 0;
		for (uint32_t i = 0; st && i < st->cells.len; i++) {
			// A section's header, before its first row.
			for (; h < st->headers.len && header_start(v, st->headers.data[h].index) <= st->cells.data[i].index; h++)
				walk(a, st->headers.data[h].cell, f, ctx);
			walk(a, st->cells.data[i].cell, f, ctx);
		}
	}
}

// modal reports whether a presented view takes every touch until it
// goes: a sheet, an alert, a popover, a modal overlay with what's in it.
static bool modal(const ak_fake_view *v) {
	if (v->kind == AK_KIND_OVERLAY) return v->children.len && ak_fake_bool(v, AK_PROP_OVERLAY_MODAL) && !ak_fake_bool(v, AK_PROP_HIDDEN);
	return v->kind == AK_KIND_SHEET || v->kind == AK_KIND_ALERT || v->kind == AK_KIND_POPOVER;
}

// layers are the root and the views presented over it (sheets, alerts,
// popovers, windows, a snackbar), bottom to top.
static void layers(ak_uitest *a, ak_fake_view_vec *out) {
	ak_fake_view *r = ak_fake_root(a->f);
	if (r) ak_vec_push(out, r);
	ak_fake_presented(a->f, out);
}

// touchable are the layers a touch reaches: the top modal one (and a
// snackbar over it), else all of them.
static void touchable(ak_uitest *a, ak_fake_view_vec *out) {
	ak_fake_view_vec ls = {0};
	layers(a, &ls);
	int64_t top = -1;
	for (uint32_t i = 0; i < ls.len; i++)
		if (modal(ls.data[i])) top = i;
	for (uint32_t i = 0; i < ls.len; i++)
		if (top < 0 || i == top || (i > top && !modal(ls.data[i]))) ak_vec_push(out, ls.data[i]);
	ak_vec_free(&ls);
}

static bool collect_fn(ak_uitest *a, ak_fake_view *v, void *ctx) {
	AK_UNUSED(a);
	ak_vec_push((ak_fake_view_vec *)ctx, v);
	return true;
}

// visible are the views that show in the layers.
static void visible_in(ak_uitest *a, const ak_fake_view_vec *ls, ak_fake_view_vec *out) {
	for (uint32_t i = 0; i < ls->len; i++) walk(a, ls->data[i], collect_fn, out);
}

static void visible(ak_uitest *a, ak_fake_view_vec *out) {
	ak_fake_view_vec ls = {0};
	layers(a, &ls);
	visible_in(a, &ls, out);
	ak_vec_free(&ls);
}

static void visible_touchable(ak_uitest *a, ak_fake_view_vec *out) {
	ak_fake_view_vec ls = {0};
	touchable(a, &ls);
	visible_in(a, &ls, out);
	ak_vec_free(&ls);
}

typedef void (*text_fn)(ak_span s, void *ctx);

// rich_paragraphs calls f with each paragraph of a rich text's content.
static void rich_paragraphs(ak_span content, text_fn f, void *ctx) {
	ak_rich_run_vec runs = {0};
	ak_decode_rich((const uint8_t *)content.p, content.len, &runs);
	ak_buf p = {0};
	for (uint32_t i = 0; i <= runs.len; i++) {
		if (i == runs.len || (i > 0 && runs.data[i].text.len && runs.data[i].text.p[0] == '\n')) {
			if (p.len) f(ak_strn((const char *)p.data, p.len), ctx);
			ak_buf_reset(&p);
			if (i == runs.len) break;
			ak_span t = runs.data[i].text;
			ak_buf_put(&p, t.p + 1, t.len - 1);
			continue;
		}
		ak_buf_put(&p, runs.data[i].text.p, runs.data[i].text.len);
	}
	ak_buf_free(&p);
	ak_vec_free(&runs);
}

// shows calls f with each text v shows of its own (not its children's):
// a Text's text, a Button's title, a field's text and placeholder, a
// rich text's paragraphs, an alert's title and message, a picker's
// chosen item, a native view's label, an accessibility label.
static void shows(const ak_fake_view *v, text_fn f, void *ctx) {
	ak_span s;
#define ADD(x) do { s = (x); if (s.len) f(s, ctx); } while (0)
	switch (v->kind) {
	case AK_KIND_TEXT: ADD(ak_fake_string(v, AK_PROP_TEXT_TEXT)); break;
	case AK_KIND_BUTTON: ADD(ak_fake_string(v, AK_PROP_BUTTON_TITLE)); break;
	case AK_KIND_NATIVE: {
		ADD(ak_fake_label(v));
		const ak_fake_native *l = ak_fake_native_value(v, "label");
		if (!l) l = ak_fake_native_value(v, "Label");
		if (l && l->type == 4) ADD(l->s);
		break;
	}
	case AK_KIND_TEXT_FIELD:
		ADD(ak_fake_string(v, AK_PROP_TEXT_FIELD_VALUE));
		ADD(ak_fake_string(v, AK_PROP_TEXT_FIELD_PLACEHOLDER));
		break;
	case AK_KIND_RICH_TEXT: rich_paragraphs(ak_fake_string(v, AK_PROP_RICH_TEXT_CONTENT), f, ctx); break;
	case AK_KIND_ALERT:
		ADD(ak_fake_string(v, AK_PROP_ALERT_TITLE));
		ADD(ak_fake_string(v, AK_PROP_ALERT_MESSAGE));
		break;
	case AK_KIND_PICKER: {
		ak_str_vec items = {0};
		ak_fake_strings(v, AK_PROP_PICKER_ITEMS, &items);
		int64_t i = ak_fake_int(v, AK_PROP_PICKER_SELECTED);
		if (i >= 0 && i < items.len) ADD(items.data[i]);
		else ADD(ak_fake_string(v, AK_PROP_PICKER_LABEL));
		ak_vec_free(&items);
		break;
	}
	case AK_KIND_WHEEL: {
		ak_str_vec items = {0};
		ak_fake_strings(v, AK_PROP_WHEEL_ITEMS, &items);
		int64_t i = ak_fake_int(v, AK_PROP_WHEEL_SELECTED);
		if (i >= 0 && i < items.len) ADD(items.data[i]);
		ak_vec_free(&items);
		break;
	}
	}
	ADD(ak_fake_string(v, AK_PROP_A11Y_LABEL));
#undef ADD
}

typedef struct {
	ak_span text;
	bool found;
} has_ctx;

static void has_fn(ak_span s, void *ctx) {
	has_ctx *h = ctx;
	if (ak_span_eq(s, h->text)) h->found = true;
}

static bool shows_text(const ak_fake_view *v, ak_span text) {
	has_ctx h = {text, false};
	shows(v, has_fn, &h);
	return h.found;
}

// screen_title is a Screen's title in its navigation bar: none for a
// tab's page, whose title is its tab's.
static ak_span screen_title(const ak_fake_view *v) {
	if (v->parent && v->parent->kind == AK_KIND_TABS) return sp("");
	return ak_fake_string(v, AK_PROP_SCREEN_TITLE);
}

// What a text names: a view showing it, or an item (a tab, a bar button,
// an alert's button) a view shows.
enum {
	BY_VIEW,      // the view's own text: a Text, a Button's title, a field's
	BY_TITLE,     // a Screen's title, in its navigation bar
	BY_TAB,       // a Tabs' tab: its Screen's title
	BY_DEST,      // a Split's destination, in its sidebar
	BY_SEGMENT,   // a Segmented's segment
	BY_ITEM,      // an item of a Screen's bar buttons, toolbar or primary action, or an alert's buttons
	BY_FIELD,     // an alert's text field, by its placeholder
	BY_SNACKBAR,  // a snackbar's message
	BY_SNACK_ACT, // a snackbar's action
};

typedef struct {
	ak_fake_view *v;
	int how;
	int64_t i;  // the item's number (with its base), a tab's or a segment's index, a field's
} target;

typedef AK_VEC(target) target_vec;

// The parts of the screen a scope (app.in(…)) narrows to: TestScope's
// cases, in order.
enum { IN_ANYWHERE, IN_CONTENT, IN_NAVIGATION_BAR, IN_TOOLBAR, IN_TAB_BAR, IN_SIDEBAR };

static int region_of(const target *t) {
	switch (t->how) {
	case BY_TITLE: return IN_NAVIGATION_BAR;
	case BY_TAB: return IN_TAB_BAR;
	case BY_DEST: return IN_SIDEBAR;
	case BY_ITEM:
		if (t->v->kind == AK_KIND_SCREEN) return t->i >= AK_SCREEN_TOOLBAR_ACTIONS ? IN_TOOLBAR : IN_NAVIGATION_BAR;
	}
	return IN_CONTENT;
}

// cell_of is the list cell v is in (its top view, bound to a list), or
// NULL.
static ak_fake_view *cell_of(ak_uitest *a, ak_fake_view *v, ak_fake_view **list) {
	while (v->parent) v = v->parent;
	if (v->kind != AK_FAKE_KIND_CELL) return NULL;
	for (uint32_t i = 0; i < a->lists.len; i++) {
		list_state *st = &a->lists.data[i];
		for (uint32_t j = 0; j < st->cells.len; j++)
			if (st->cells.data[j].cell == v) {
				if (list) *list = ak_fake_view_of(a->f, st->list);
				return v;
			}
	}
	return NULL;
}

// contains reports whether v is u or in it: through its parents, and
// from a list's row to the list.
static bool contains(ak_uitest *a, const ak_fake_view *u, ak_fake_view *v) {
	while (v) {
		if (v == u) return true;
		if (!v->parent && v->kind == AK_FAKE_KIND_CELL) {
			ak_fake_view *list = NULL;
			if (!cell_of(a, v, &list)) return false;
			v = list;
			continue;
		}
		v = v->parent;
	}
	return false;
}

static bool in_scope(ak_uitest *a, const target *t) {
	if (a->scope.region != IN_ANYWHERE && region_of(t) != a->scope.region) return false;
	if (a->scope.under && !contains(a, a->scope.under, t->v)) return false;
	return true;
}

// top_frame is v's frame in the coordinates of the top of its tree (the
// root, a sheet, a list's cell), which it returns.
static const ak_fake_view *top_frame(const ak_fake_view *v, float f[4]) {
	f[0] = v->frame[0], f[1] = v->frame[1], f[2] = v->frame[2], f[3] = v->frame[3];
	while (v->parent) {
		v = v->parent;
		f[0] += v->frame[0], f[1] += v->frame[1];
	}
	return v;
}

// next_to picks the control of cs that's next to the view showing a
// label, as a user reads it: on its line, the nearest (after it, in
// reading order, before one before it); else below it, the nearest (a
// form's label over its field). Only controls in the same tree count.
// NULL if none is, or two are as near.
static ak_fake_view *next_to(const ak_fake_view *label, ak_fake_view *const *cs, uint32_t n) {
	float lf[4], cf[4];
	const ak_fake_view *lt = top_frame(label, lf);
	ak_fake_view *best = NULL;
	double best_score = INFINITY;
	bool tie = false;
	for (uint32_t i = 0; i < n; i++) {
		ak_fake_view *c = cs[i];
		if (top_frame(c, cf) != lt) continue;
		double score;
		if (cf[1] < lf[1] + lf[3] && lf[1] < cf[1] + cf[3]) {  // on the label's line
			double after = cf[0] - (lf[0] + lf[2]), before = lf[0] - (cf[0] + cf[2]);
			score = after >= 0 || before < 0 ? fabs(after) : 1e4 + before;
		} else if (cf[1] >= lf[1] + lf[3]) {  // below it
			score = 1e5 + (cf[1] - (lf[1] + lf[3]));
		} else {
			continue;
		}
		if (score < best_score) best = c, best_score = score, tie = false;
		else if (score == best_score) tie = true;
	}
	return tie ? NULL : best;
}

static void targets_in(ak_uitest *a, const ak_fake_view_vec *ls, ak_span text, target_vec *out);

// besides keeps, of the targets, those next to the scope's label
// (app.nextTo(label)): for each view showing it, the nearest.
static void besides(ak_uitest *a, const ak_fake_view_vec *ls, target_vec *ts) {
	if (!a->scope.next_to || !ts->len) return;
	ak_span label = ak_strn(a->scope.next_to->data, a->scope.next_to->len);
	ak_fake_view_vec views = {0}, keep = {0}, others = {0};
	for (uint32_t i = 0; i < ts->len; i++) {
		if (ts->data[i].how != BY_VIEW) continue;
		bool dup = false;
		for (uint32_t j = 0; j < views.len; j++) dup = dup || views.data[j] == ts->data[i].v;
		if (!dup) ak_vec_push(&views, ts->data[i].v);
	}
	scope saved = a->scope;
	a->scope = (scope){0};
	target_vec ls_ts = {0};
	targets_in(a, ls, label, &ls_ts);
	a->scope = saved;
	for (uint32_t i = 0; i < ls_ts.len; i++) {
		if (ls_ts.data[i].how != BY_VIEW) continue;
		others.len = 0;
		for (uint32_t j = 0; j < views.len; j++)
			if (views.data[j] != ls_ts.data[i].v) ak_vec_push(&others, views.data[j]);
		ak_fake_view *v = next_to(ls_ts.data[i].v, others.data, others.len);
		if (v) ak_vec_push(&keep, v);
	}
	uint32_t w = 0;
	for (uint32_t i = 0; i < ts->len; i++) {
		bool k = false;
		for (uint32_t j = 0; j < keep.len && ts->data[i].how == BY_VIEW; j++) k = k || keep.data[j] == ts->data[i].v;
		if (k) ts->data[w++] = ts->data[i];
	}
	ts->len = w;
	ak_vec_free(&views);
	ak_vec_free(&keep);
	ak_vec_free(&others);
	ak_vec_free(&ls_ts);
}

static void add_target(ak_uitest *a, target_vec *out, ak_fake_view *v, int how, int64_t i) {
	target t = {v, how, i};
	if (in_scope(a, &t)) ak_vec_push(out, t);
}

// items adds the items of v's prop p titled text, numbered from base.
static void items_titled(ak_uitest *a, target_vec *out, ak_fake_view *v, ak_prop p, int64_t base, ak_span text) {
	ak_action_vec items = {0};
	ak_fake_action_items(v, p, &items);
	for (uint32_t i = 0; i < items.len; i++)
		if (ak_span_eq(items.data[i].title, text)) add_target(a, out, v, BY_ITEM, base + items.data[i].index);
	ak_vec_free(&items);
}

// primary_title is a Screen's primary action's title (its item 0).
static ak_span primary_title(const ak_fake_view *v, ak_action_vec *items) {
	ak_fake_action_items(v, AK_PROP_SCREEN_PRIMARY, items);
	return items->len ? items->data[0].title : sp("");
}

static void targets_in(ak_uitest *a, const ak_fake_view_vec *ls, ak_span text, target_vec *out) {
	ak_fake_view_vec vs = {0};
	visible_in(a, ls, &vs);
	for (uint32_t i = 0; i < vs.len; i++) {
		ak_fake_view *v = vs.data[i];
		if (shows_text(v, text)) add_target(a, out, v, BY_VIEW, 0);
		switch (v->kind) {
		case AK_KIND_SCREEN: {
			if (text.len && ak_span_eq(screen_title(v), text)) add_target(a, out, v, BY_TITLE, 0);
			items_titled(a, out, v, AK_PROP_SCREEN_ACTIONS, 0, text);
			items_titled(a, out, v, AK_PROP_SCREEN_TOOLBAR, AK_SCREEN_TOOLBAR_ACTIONS, text);
			ak_action_vec items = {0};
			if (text.len && ak_span_eq(primary_title(v, &items), text)) add_target(a, out, v, BY_ITEM, AK_SCREEN_PRIMARY_ACTION);
			ak_vec_free(&items);
			break;
		}
		case AK_KIND_TABS:
			// A tab, by its title or its badge (on the tab: a tap there picks it).
			for (uint32_t j = 0; j < v->children.len; j++)
				if (text.len && (ak_span_eq(ak_fake_string(v->children.data[j], AK_PROP_SCREEN_TITLE), text) ||
				                 ak_span_eq(ak_fake_string(v->children.data[j], AK_PROP_SCREEN_BADGE), text)))
					add_target(a, out, v, BY_TAB, j);
			break;
		case AK_KIND_SPLIT: {
			if (!text.len) break;
			ak_sidebar_heading_vec secs = {0};
			ak_sidebar_item_vec items = {0};
			ak_fake_sidebar(v, &secs, &items);
			for (uint32_t j = 0; j < items.len; j++)
				if (ak_span_eq(items.data[j].title, text)) add_target(a, out, v, BY_DEST, j);
			ak_vec_free(&secs);
			ak_vec_free(&items);
			break;
		}
		case AK_KIND_SEGMENTED: {
			ak_str_vec segs = {0};
			ak_fake_strings(v, AK_PROP_SEGMENTED_SEGMENTS, &segs);
			for (uint32_t j = 0; j < segs.len; j++)
				if (ak_span_eq(segs.data[j], text)) add_target(a, out, v, BY_SEGMENT, j);
			ak_vec_free(&segs);
			break;
		}
		case AK_KIND_ALERT: {
			items_titled(a, out, v, AK_PROP_ALERT_BUTTONS, 0, text);
			ak_alert_field_vec fs = {0};
			ak_fake_alert_fields(v, &fs);
			for (uint32_t j = 0; j < fs.len; j++)
				if (text.len && ak_span_eq(fs.data[j].placeholder, text)) add_target(a, out, v, BY_FIELD, j);
			ak_vec_free(&fs);
			break;
		}
		case AK_KIND_SNACKBAR:
			if (text.len && ak_span_eq(ak_fake_string(v, AK_PROP_SNACKBAR_MESSAGE), text)) add_target(a, out, v, BY_SNACKBAR, 0);
			if (text.len && ak_span_eq(ak_fake_string(v, AK_PROP_SNACKBAR_ACTION), text)) add_target(a, out, v, BY_SNACK_ACT, 0);
			break;
		}
	}
	ak_vec_free(&vs);
	besides(a, ls, out);
}

// targets are what shows text anywhere; reach, in the layers a touch
// reaches.
static void targets(ak_uitest *a, ak_span text, target_vec *out) {
	ak_fake_view_vec ls = {0};
	layers(a, &ls);
	targets_in(a, &ls, text, out);
	ak_vec_free(&ls);
}

static void targets_in_reach(ak_uitest *a, ak_span text, target_vec *out) {
	ak_fake_view_vec ls = {0};
	touchable(a, &ls);
	targets_in(a, &ls, text, out);
	ak_vec_free(&ls);
}

// targets_of calls f with each text v shows: its own, its navigation
// bar's, its tabs', its items'.
static void items_text(const ak_fake_view *v, ak_prop p, text_fn f, void *ctx) {
	ak_action_vec items = {0};
	ak_fake_action_items(v, p, &items);
	for (uint32_t i = 0; i < items.len; i++)
		if (items.data[i].title.len) f(items.data[i].title, ctx);
	ak_vec_free(&items);
}

static void targets_of(const ak_fake_view *v, text_fn f, void *ctx) {
	shows(v, f, ctx);
	switch (v->kind) {
	case AK_KIND_SCREEN: {
		ak_span t = screen_title(v);
		if (t.len) f(t, ctx);
		items_text(v, AK_PROP_SCREEN_ACTIONS, f, ctx);
		items_text(v, AK_PROP_SCREEN_TOOLBAR, f, ctx);
		ak_action_vec items = {0};
		t = primary_title(v, &items);
		if (t.len) f(t, ctx);
		ak_vec_free(&items);
		break;
	}
	case AK_KIND_TABS:
		// Each tab's title, then its badge (a count), as the tab bar shows
		// and a screen reader reads them.
		for (uint32_t i = 0; i < v->children.len; i++) {
			ak_span t = ak_fake_string(v->children.data[i], AK_PROP_SCREEN_TITLE);
			if (t.len) f(t, ctx);
			ak_span badge = ak_fake_string(v->children.data[i], AK_PROP_SCREEN_BADGE);
			if (badge.len) f(badge, ctx);
		}
		break;
	case AK_KIND_SEGMENTED: {
		ak_str_vec segs = {0};
		ak_fake_strings(v, AK_PROP_SEGMENTED_SEGMENTS, &segs);
		for (uint32_t i = 0; i < segs.len; i++) f(segs.data[i], ctx);
		ak_vec_free(&segs);
		break;
	}
	case AK_KIND_ALERT: items_text(v, AK_PROP_ALERT_BUTTONS, f, ctx); break;
	case AK_KIND_SNACKBAR: {
		ak_span m = ak_fake_string(v, AK_PROP_SNACKBAR_MESSAGE), act = ak_fake_string(v, AK_PROP_SNACKBAR_ACTION);
		if (m.len) f(m, ctx);
		if (act.len) f(act, ctx);
		break;
	}
	}
}

// The open menu: a view's Menu, or a Screen's bar button's, toolbar's
// or primary action's, whose items tap chooses. Its items that show are
// a submenu's (sub, its number) or the whole menu's (-1), and the items
// of the sections among them, in place.
typedef AK_VEC(uint32_t) index_vec;

// shown_under adds parent's items, and in place those of the sections
// among them.
static void shown_under(const ak_action_vec *all, uint32_t parent, index_vec *out) {
	for (uint32_t i = 0; i < all->len; i++) {
		if (all->data[i].parent != parent) continue;
		ak_vec_push(out, i);
		if ((all->data[i].flags & AK_MENU_INLINE) && (all->data[i].flags & AK_MENU_SUBMENU)) shown_under(all, all->data[i].index, out);
	}
}

static void menu_items(ak_uitest *a, ak_action_vec *all, index_vec *out) {
	ak_fake_action_items(a->menu, a->menu_prop, all);
	uint32_t parent = UINT32_MAX;
	if (a->menu_prop == AK_PROP_SCREEN_PRIMARY && all->len) parent = 0;  // item 0 is the button
	if (a->menu_sub >= 0) parent = (uint32_t)a->menu_sub;
	shown_under(all, parent, out);
}

static bool menu_has(ak_uitest *a, ak_span text) {
	if (!a->menu) return false;
	ak_action_vec all = {0};
	index_vec shown = {0};
	menu_items(a, &all, &shown);
	bool found = false;
	for (uint32_t i = 0; i < shown.len; i++) found = found || ak_span_eq(all.data[shown.data[i]].title, text);
	ak_vec_free(&all);
	ak_vec_free(&shown);
	return found;
}

bool ak_uitest_has(ak_uitest *a, ak_span text) {
	if (menu_has(a, text)) return true;
	scope saved = a->scope;
	target_vec ts = {0};
	targets(a, text, &ts);
	a->scope = saved;
	bool found = ts.len > 0;
	ak_vec_free(&ts);
	return found;
}

static void text_push_fn(ak_span s, void *ctx) {
	ak_arr **out = ctx;
	ak_val v = {.p = ak_str_new(s.p, s.len)};
	ak_arr_push(out, v);
}

typedef struct {
	text_fn f;
	void *ctx;
} texts_ctx;

static bool texts_fn(ak_uitest *a, ak_fake_view *v, void *ctx) {
	AK_UNUSED(a);
	texts_ctx *t = ctx;
	targets_of(v, t->f, t->ctx);
	return true;
}

// ak_uitest_texts calls f with the texts the screen shows, in the order
// a screen reader reads them: each layer's views, depth first.
void ak_uitest_texts(ak_uitest *a, void (*f)(ak_span s, void *ctx), void *ctx) {
	ak_fake_view_vec ls = {0};
	layers(a, &ls);
	texts_ctx t = {f, ctx};
	for (uint32_t i = 0; i < ls.len; i++) walk(a, ls.data[i], texts_fn, &t);
	ak_vec_free(&ls);
}

// ---- The snapshot ----

static void num(ak_buf *b, float f) {
	double r = round((double)f * 100) / 100;
	ak_fmt_float(b, r, 64);
}

static void quote_fn(ak_span s, void *ctx) {
	ak_buf *b = ctx;
	ak_buf_u8(b, ' ');
	ak_fmt_quote(b, s);
}

static void describe(ak_uitest *a, ak_buf *b, ak_fake_view *v, int depth) {
	if (hidden(v)) return;
	for (int i = 0; i < depth; i++) ak_buf_put(b, "  ", 2);
	if (v->kind == AK_FAKE_KIND_CELL) ak_buf_printf(b, "Cell");
	else ak_fake_kind_name(b, v->kind);
	ak_buf_u8(b, ' ');
	num(b, v->frame[0]);
	ak_buf_u8(b, ',');
	num(b, v->frame[1]);
	ak_buf_u8(b, ' ');
	num(b, v->frame[2]);
	ak_buf_u8(b, 'x');
	num(b, v->frame[3]);
	targets_of(v, quote_fn, b);
	switch (v->kind) {
	case AK_KIND_TOGGLE: ak_buf_printf(b, ak_fake_bool(v, AK_PROP_TOGGLE_ON) ? " on" : " off"); break;
	case AK_KIND_SLIDER: ak_buf_put(b, " =", 2), ak_fmt_float(b, ak_fake_float(v, AK_PROP_SLIDER_VALUE), 64); break;
	case AK_KIND_STEPPER: ak_buf_put(b, " =", 2), ak_fmt_float(b, ak_fake_float(v, AK_PROP_STEPPER_VALUE), 64); break;
	case AK_KIND_PROGRESS:
		if (ak_fake_bool(v, AK_PROP_PROGRESS_INDETERMINATE)) ak_buf_printf(b, " indeterminate");
		else ak_buf_put(b, " =", 2), ak_fmt_float(b, ak_fake_float(v, AK_PROP_PROGRESS_VALUE), 64);
		break;
	case AK_KIND_TABS: ak_buf_printf(b, " selected=%lld", (long long)ak_fake_int(v, AK_PROP_TABS_SELECTED)); break;
	case AK_KIND_SEGMENTED: ak_buf_printf(b, " selected=%lld", (long long)ak_fake_int(v, AK_PROP_SEGMENTED_SELECTED)); break;
	case AK_KIND_TEXT_FIELD:
		if (ak_fake_bool(v, AK_PROP_TEXT_FIELD_SECURE)) ak_buf_printf(b, " secure");
		break;
	}
	if (v->kind == AK_KIND_NATIVE) {
		// Its kind, and its values: a typed view's encoded (in hex).
		ak_span k = ak_fake_string(v, AK_PROP_NATIVE_TYPE);
		ak_buf_printf(b, " %.*s", (int)k.len, k.p);
		for (uint32_t i = 0; i < v->native.len; i++) {
			const ak_fake_native *n = &v->native.data[i];
			ak_buf_printf(b, " %.*s=", (int)n->name.len, n->name.p);
			switch (n->type) {
			case 1:
			case 2: ak_buf_printf(b, "%lld", (long long)n->i); break;
			case 3: ak_fmt_float(b, n->f, 64); break;
			case 4: ak_fmt_quote(b, n->s); break;
			default:
				// The first bytes: a snapshot stays readable (and diffs) when a
				// value is long (a shape's paths).
				for (uint32_t j = 0; j < n->s.len && j < 24; j++) ak_buf_printf(b, "%02x", (uint8_t)n->s.p[j]);
				if (n->s.len > 24) ak_buf_printf(b, "…(%u bytes)", (unsigned)n->s.len);
			}
		}
	}
	if (ak_fake_bool(v, AK_PROP_SELECTED)) ak_buf_printf(b, " selected");
	if (ak_fake_bool(v, AK_PROP_DISABLED)) ak_buf_printf(b, " disabled");
	ak_buf_u8(b, '\n');
	if (v->kind == AK_KIND_STACK) {
		if (v->children.len) describe(a, b, v->children.data[v->children.len - 1], depth + 1);
		return;
	}
	if (v->kind == AK_KIND_TABS) {
		int64_t i = ak_fake_int(v, AK_PROP_TABS_SELECTED);
		if (i >= 0 && i < v->children.len) describe(a, b, v->children.data[i], depth + 1);
		return;
	}
	for (uint32_t i = 0; i < v->children.len; i++) describe(a, b, v->children.data[i], depth + 1);
	if (v->kind == AK_KIND_LIST) {
		list_state *st = list_of(a, v->id);
		uint32_t h = 0;
		for (uint32_t i = 0; st && i < st->cells.len; i++) {
			for (; h < st->headers.len && header_start(v, st->headers.data[h].index) <= st->cells.data[i].index; h++)
				describe(a, b, st->headers.data[h].cell, depth + 1);
			describe(a, b, st->cells.data[i].cell, depth + 1);
		}
	}
}

void ak_uitest_snapshot(ak_uitest *a, ak_buf *out) {
	ak_fake_view_vec ls = {0};
	layers(a, &ls);
	for (uint32_t i = 0; i < ls.len; i++) {
		if (i) ak_buf_u8(out, '\n');
		describe(a, out, ls.data[i], 0);
	}
	ak_vec_free(&ls);
	if (a->menu) {
		ak_buf_printf(out, "\nMenu\n");
		ak_action_vec all = {0};
		index_vec shown = {0};
		menu_items(a, &all, &shown);
		for (uint32_t i = 0; i < shown.len; i++) {
			const ak_action *it = &all.data[shown.data[i]];
			ak_buf_put(out, "  ", 2);
			ak_fmt_quote(out, it->title);
			if (it->flags & AK_MENU_CHECKED) ak_buf_printf(out, " checked");
			if (it->flags & AK_MENU_DISABLED) ak_buf_printf(out, " disabled");
			ak_buf_u8(out, '\n');
		}
		ak_vec_free(&all);
		ak_vec_free(&shown);
	}
}

static bool failure(ak_uitest *a, ak_buf *err, const char *fmt, ak_span text) {
	if (err) {
		ak_buf_printf(err, fmt, (int)text.len, text.p);
		ak_buf_printf(err, "\nthe screen:\n");
		ak_uitest_snapshot(a, err);
	}
	return false;
}

// ---- Lists ----

static bool items_equal(const list_state *st, const ak_fake_view *list) {
	if (st->items.len != list->items.len) return false;
	return st->items.len == 0 || memcmp(st->items.data, list->items.data, st->items.len * sizeof(int64_t)) == 0;
}

static int32_t cell_at(const list_state *st, int64_t i) {
	for (uint32_t j = 0; j < st->cells.len; j++)
		if (st->cells.data[j].index == i) return (int32_t)j;
	return -1;
}

static void insert_bound(list_state *st, int64_t i, ak_fake_view *c) {
	uint32_t at = 0;
	while (at < st->cells.len && st->cells.data[at].index < i) at++;
	ak_vec_reserve(&st->cells, 1);
	memmove(st->cells.data + at + 1, st->cells.data + at, (st->cells.len - at) * sizeof(bound));
	st->cells.data[at] = (bound){i, c};
	st->cells.len++;
}

typedef struct {
	ak_uitest *a;
	list_state *st;
	ak_fake_view *list;
	bool stale, changed;
} bind_ctx;

static float bind_row(bind_ctx *b, int64_t i) {
	list_state *st = b->st;
	int32_t j = cell_at(st, i);
	ak_fake_view *c;
	if (j < 0) {
		c = st->free.len ? ak_vec_pop(&st->free) : ak_fake_new_cell(b->a->f);
		insert_bound(st, i, c);
		ak_fake_bind(b->a->f, b->list, i, c);
		b->changed = true;
	} else {
		c = st->cells.data[j].cell;
		if (b->stale) ak_fake_bind(b->a->f, b->list, i, c);
	}
	return c->frame[3];
}

static bool sync_list(ak_uitest *a, ak_fake_view *list) {
	list_state *st = list_of(a, list->id);
	if (!st) {
		ak_vec_push(&a->lists, ((list_state){.list = list->id}));
		st = &a->lists.data[a->lists.len - 1];
		st->end = ak_fake_int(list, AK_PROP_LIST_ANCHOR) == AK_SCROLL_ANCHOR_BOTTOM;
	}
	st->seen = true;
	if (!st->scrolled) st->end = ak_fake_int(list, AK_PROP_LIST_ANCHOR) == AK_SCROLL_ANCHOR_BOTTOM;  // its anchor may come after it
	int64_t count = ak_fake_list_count(a->f, list);
	bind_ctx b = {a, st, list, count != st->count || !items_equal(st, list), false};
	st->count = count;
	st->items.len = 0;
	for (uint32_t i = 0; i < list->items.len; i++) ak_vec_push(&st->items, list->items.data[i]);
	float height = list->frame[3] > 0 ? list->frame[3] : a->f->env.height;
	int64_t cols = ak_fake_int(list, AK_PROP_LIST_COLUMNS);
	if (cols < 1) cols = 1;
	int64_t lo, hi;
	if (st->end) {
		hi = count;
		lo = hi;
		for (float filled = 0; lo > 0 && filled <= height && hi - lo < MAX_BOUND;) {
			int64_t line = lo - 1 - (lo - 1) % cols;
			float h = 0;
			for (int64_t i = line; i < lo; i++) h = ak_maxf(h, bind_row(&b, i));
			filled += h;
			lo = line;
		}
	} else {
		lo = st->first < count - 1 ? st->first : (count - 1 > 0 ? count - 1 : 0);
		lo -= lo % cols;
		hi = lo;
		for (float filled = 0; hi < count && filled <= height && hi - lo < MAX_BOUND;) {
			float h = 0;
			int64_t end = hi + cols < count ? hi + cols : count;
			for (int64_t i = hi; i < end; i++) h = ak_maxf(h, bind_row(&b, i));
			filled += h;
			hi = end;
		}
	}
	for (uint32_t j = 0; j < st->cells.len;) {
		bound c = st->cells.data[j];
		if (c.index < lo || c.index >= hi) {
			ak_fake_unbind(a->f, list, c.cell);
			memmove(st->cells.data + j, st->cells.data + j + 1, (st->cells.len - j - 1) * sizeof(bound));
			st->cells.len--;
			ak_vec_push(&st->free, c.cell);
			b.changed = true;
			continue;
		}
		j++;
	}
	// The headers of the sections whose rows show, bound at -1 - the
	// section, as a collection view binds a section's header.
	for (uint32_t s = 0; s < list->sections.len; s++) {
		int64_t start = list->sections.data[s].start, end = s + 1 < list->sections.len ? list->sections.data[s + 1].start : count;
		bool shows = start < hi && end > lo;
		int32_t at = -1;
		for (uint32_t j = 0; j < st->headers.len; j++)
			if (st->headers.data[j].index == s) at = (int32_t)j;
		if (shows && at < 0) {
			ak_fake_view *c = st->free.len ? ak_vec_pop(&st->free) : ak_fake_new_cell(a->f);
			uint32_t k = 0;
			while (k < st->headers.len && st->headers.data[k].index < s) k++;
			ak_vec_reserve(&st->headers, 1);
			memmove(st->headers.data + k + 1, st->headers.data + k, (st->headers.len - k) * sizeof(bound));
			st->headers.data[k] = (bound){s, c};
			st->headers.len++;
			ak_fake_bind(a->f, list, -1 - (int64_t)s, c);
			b.changed = true;
		} else if (shows && b.stale) {
			ak_fake_bind(a->f, list, -1 - (int64_t)s, st->headers.data[at].cell);
		}
	}
	for (uint32_t j = 0; j < st->headers.len;) {
		bound c = st->headers.data[j];
		int64_t s = c.index;
		int64_t start = header_start(list, s), end = s + 1 < list->sections.len ? list->sections.data[s + 1].start : count;
		if (s < list->sections.len && start < hi && end > lo) {
			j++;
			continue;
		}
		ak_fake_unbind(a->f, list, c.cell);
		memmove(st->headers.data + j, st->headers.data + j + 1, (st->headers.len - j - 1) * sizeof(bound));
		st->headers.len--;
		ak_vec_push(&st->free, c.cell);
		b.changed = true;
	}
	return b.changed;
}

static bool lists_fn(ak_uitest *a, ak_fake_view *v, void *ctx) {
	if (v->kind == AK_KIND_LIST && !list_of(a, v->id)) {
		ak_vec_push(&a->lists, ((list_state){.list = v->id, .end = ak_fake_int(v, AK_PROP_LIST_ANCHOR) == AK_SCROLL_ANCHOR_BOTTOM}));
	}
	if (v->kind == AK_KIND_LIST) {
		list_state *st = list_of(a, v->id);
		if (!st->seen && sync_list(a, v)) *(bool *)ctx = true;
	}
	return true;
}

// sync_lists binds what each visible list shows, and lets go of the
// lists that went; it reports whether it bound anything.
static bool sync_lists(ak_uitest *a) {
	for (uint32_t i = 0; i < a->lists.len; i++) a->lists.data[i].seen = false;
	bool changed = false;
	ak_fake_view_vec ls = {0};
	layers(a, &ls);
	for (uint32_t i = 0; i < ls.len; i++) walk(a, ls.data[i], lists_fn, &changed);
	ak_vec_free(&ls);
	for (uint32_t i = 0; i < a->lists.len;) {
		list_state *st = &a->lists.data[i];
		if (st->seen || ak_fake_view_of(a->f, st->list)) {  // hidden: kept, as a collection view keeps its cells
			i++;
			continue;
		}
		ak_vec_free(&st->cells);
		ak_vec_free(&st->headers);
		ak_vec_free(&st->free);
		ak_vec_free(&st->items);
		a->lists.data[i] = a->lists.data[--a->lists.len];
	}
	return changed;
}

// report_sheets reports each newly presented sheet's environment, as a
// phone's host sizes it, so its content lays out.
static bool report_sheets(ak_uitest *a) {
	uint8_t p = a->f->env.platform;
	if (p != AK_PLATFORM_IOS && p != AK_PLATFORM_ANDROID) return false;
	for (uint32_t i = 0; i < a->sheets.len;) {
		if (!ak_fake_view_of(a->f, a->sheets.data[i])) a->sheets.data[i] = a->sheets.data[--a->sheets.len];
		else i++;
	}
	bool changed = false;
	ak_fake_view_vec ps = {0};
	ak_fake_presented(a->f, &ps);
	for (uint32_t i = 0; i < ps.len; i++) {
		ak_fake_view *v = ps.data[i];
		if (v->kind != AK_KIND_SHEET) continue;
		bool seen = false;
		for (uint32_t j = 0; j < a->sheets.len; j++) seen = seen || a->sheets.data[j] == v->id;
		if (seen) continue;
		ak_vec_push(&a->sheets, v->id);
		ak_fake_report_sheet(a->f, v);
		changed = true;
	}
	ak_vec_free(&ps);
	return changed;
}

static void settle(ak_uitest *a);

// ak_uitest_settle runs the app until nothing's left to do. A test drives
// the app as a host does, so it doesn't stay in the batch of whatever
// resumed it (a host call's answer, delivered in one): each of its writes
// runs its effects before it settles.
void ak_uitest_settle(ak_uitest *a) {
	if (ak_signal_flushing()) {
		ak_buf_printf(&a->fails, "the test drives the app from inside an effect\n");
		return;
	}
	int depth = ak_batch_suspend();
	settle(a);
	ak_batch_resume(depth);
}

static void settle(ak_uitest *a) {
	for (int round = 0; round < 1000; round++) {
		ak_fake_run_posted(a->f);
		bool changed = false;
		if (ak_sched_has_ready()) {
			ak_ui_run_posted();
			changed = true;
		}
		ak_ui_flush();
		changed = report_sheets(a) || changed;
		changed = sync_lists(a) || changed;
		if (!changed && ak_fake_pending(a->f) == 0 && ak_sched_held()) {
			// Another thread works for the app (spawn, a database's
			// statement): wait for what it posts back.
			ak_sched_wait_posted();
			ak_sched_run_posted();
			continue;
		}
		if (!changed && ak_fake_pending(a->f) == 0) {
			if (a->focused && ak_fake_view_of(a->f, a->focused_id) != a->focused) a->focused = NULL;
			if (a->menu && ak_fake_view_of(a->f, a->menu_id) != a->menu) a->menu = NULL;
			return;
		}
	}
	ak_buf_printf(&a->fails, "the app never settles (it posts or rebinds forever)\n");
}

bool ak_uitest_pump(void) {
	if (!active || !active->mounted) return false;
	ak_ui_flush();  // a call waits in the batch until it's sent
	if (ak_fake_pending(active->f) == 0) return false;
	// What's delivered may end the test, which lets the app go: it's
	// held until its settle returns.
	ak_uitest *a = ak_retain(active);
	ak_uitest_settle(a);
	ak_release(a);
	return true;
}

// ak_uitest_idle settles, then moves the clock (a manual one, under
// test) to each timer that's due and settles again, until no timer is
// left. Ten minutes of the app's time is the most it moves: a timer that
// keeps firing past that (a ticker) is a failure.
void ak_uitest_idle(ak_uitest *a) {
	int64_t start = ak_sched_now();
	for (;;) {
		ak_uitest_settle(a);
		int64_t at;
		if (!ak_sched_next_timer(&at)) return;
		if (at - start > 600 * (int64_t)1000000000) {
			ak_buf_printf(&a->fails, "the app never idles: a timer keeps firing (a ticker?)\n");
			return;
		}
		int64_t now = ak_sched_now();
		ak_sched_advance(at > now ? at - now : 0);
	}
}

// ---- Actions ----
//
// What a user does, named by what the screen
// shows. Each action settles, then reports the fake host's misuses.

// disabled_view reports whether v or a view around it is disabled.
static bool disabled_view(const ak_fake_view *v) {
	for (const ak_fake_view *p = v; p; p = p->parent)
		if (ak_fake_bool(p, AK_PROP_DISABLED)) return true;
	return false;
}

// has_menu reports whether v has a menu of trigger with items in it: an
// empty one is none, as on every host.
static bool has_menu(const ak_fake_view *v, int64_t trigger) {
	if (ak_fake_prop_of(v, AK_PROP_MENU) == NULL || ak_fake_int(v, AK_PROP_MENU_TRIGGER) != trigger) return false;
	ak_action_vec items = {0};
	ak_fake_action_items(v, AK_PROP_MENU, &items);
	bool any = items.len > 0;
	ak_vec_free(&items);
	return any;
}

// pressable is the view a touch on v goes to: v or the nearest around it
// that takes it, a list's row; NULL if none does.
static ak_fake_view *pressable(ak_fake_view *v, bool long_press) {
	ak_fake_view *idle = NULL;  // a button that does nothing, unless a menu around it opens
	ak_fake_view *focusable = NULL;  // a view a press only focuses (on a desktop)
	for (ak_fake_view *p = v; p; p = p->parent) {
		if (long_press) {
			if (ak_fake_listens(p, AK_EVENT_LONG_PRESS) || has_menu(p, AK_MENU_TRIGGER_LONG_PRESS)) return p;
			continue;
		}
		if (ak_fake_listens(p, AK_EVENT_PRESS) || ak_fake_listens(p, AK_EVENT_PRESS_POINT) || has_menu(p, AK_MENU_TRIGGER_TAP)) return p;
		if (p->kind == AK_KIND_TOGGLE || p->kind == AK_KIND_TEXT_FIELD) return p;
		if (p->kind == AK_KIND_NATIVE && ak_fake_listens(p, AK_EVENT_NATIVE)) return p;
		if (p->kind == AK_KIND_BUTTON && !idle) idle = p;
		if (p->kind == AK_FAKE_KIND_CELL && ak_fake_listens(p, AK_EVENT_ROW_SELECTED)) return p;
		if (!focusable && ak_fake_bool(p, AK_PROP_FOCUSABLE)) focusable = p;
	}
	return idle ? idle : focusable;
}

static bool check_fails(ak_uitest *a, ak_buf *err) {
	if (!a->fails.len) return true;
	if (err) ak_buf_put(err, a->fails.data, a->fails.len);
	ak_buf_reset(&a->fails);
	return false;
}

static bool settled(ak_uitest *a, ak_buf *err) {
	ak_uitest_settle(a);
	return check_fails(a, err);
}

// dedupe drops the targets that press the same thing: a Text in a
// Button, in a row, names it once.
static void dedupe(target_vec *ts) {
	uint32_t w = 0;
	for (uint32_t i = 0; i < ts->len; i++) {
		target t = ts->data[i];
		ak_fake_view *k = t.v;
		int64_t ki = t.i;
		if (t.how == BY_VIEW) {
			ak_fake_view *p = pressable(t.v, false);
			if (p) k = p, ki = -1;
		}
		bool dup = false;
		for (uint32_t j = 0; j < w && !dup; j++) {
			target u = ts->data[j];
			ak_fake_view *k2 = u.v;
			int64_t k2i = u.i;
			if (u.how == BY_VIEW) {
				ak_fake_view *p = pressable(u.v, false);
				if (p) k2 = p, k2i = -1;
			}
			dup = k2 == k && k2i == ki;
		}
		if (!dup) ts->data[w++] = t;
	}
	ts->len = w;
}

typedef bool (*target_ok)(const target *t);

static bool not_label(const target *t) { return t->how != BY_SNACKBAR && t->how != BY_TITLE && t->how != BY_FIELD; }
static bool by_view(const target *t) { return t->how == BY_VIEW; }
static bool not_shown(const target *t) { return t->how != BY_SNACKBAR && t->how != BY_TITLE; }

// one finds the one target in reach that shows text and passes ok.
static bool one(ak_uitest *a, ak_span text, target_ok ok, target *out, ak_buf *err) {
	target_vec ts = {0};
	targets_in_reach(a, text, &ts);
	uint32_t w = 0;
	for (uint32_t i = 0; i < ts.len; i++)
		if (ok(&ts.data[i])) ts.data[w++] = ts.data[i];
	ts.len = w;
	dedupe(&ts);
	uint32_t n = ts.len;
	if (n == 1) *out = ts.data[0];
	ak_vec_free(&ts);
	if (n == 1) return true;
	if (n == 0) {
		if (ak_uitest_has(a, text)) return failure(a, err, "\"%.*s\" shows, but under what's over it, out of reach", text);
		return failure(a, err, "the screen doesn't show \"%.*s\"", text);
	}
	if (err) ak_buf_printf(err, "%u things show ", n);
	return failure(a, err, "\"%.*s\": narrow it with app.in(.content), app.in(.navigationBar), app.in(.bottomToolbar), app.in(.sidebar) or app.nextTo(label), or give it a label", text);
}

// focus gives the field the keyboard, as a tap does: the field focused
// before blurs, and each hears it if it listens.
static void focus(ak_uitest *a, ak_fake_view *p) {
	a->focused = p, a->focused_id = p->id;
	ak_fake_focus(a->f, p, false);
}

// desktop reports whether the app runs where a pointer's press moves the
// keyboard's focus (a Mac, a PC, a browser), not a phone's touch.
static bool desktop(const ak_uitest *a) {
	uint8_t p = a->f->env.platform;
	return p == AK_PLATFORM_MACOS || p == AK_PLATFORM_WINDOWS || p == AK_PLATFORM_LINUX || p == AK_PLATFORM_WEB;
}

// outside_presses reports a press on v to the shown Overlays it isn't
// in (the top first), as hosts do: OutsidePress at its middle.
static void outside_presses(ak_uitest *a, ak_fake_view *v) {
	ak_fake_view_vec ps = {0};
	ak_fake_presented(a->f, &ps);
	float fr[4];
	ak_fake_abs_frame(a->f, v, fr);
	for (uint32_t i = ps.len; i-- > 0;) {
		ak_fake_view *o = ps.data[i];
		if (o->kind != AK_KIND_OVERLAY || hidden(o) || !o->children.len) continue;
		bool in = false;
		for (ak_fake_view *x = v; x && !in; x = x->parent) in = x == o;
		if (!in) ak_fake_outside_press(a->f, o, fr[0] + fr[2] / 2, fr[1] + fr[3] / 2);
	}
	ak_vec_free(&ps);
}

static void open_menu(ak_uitest *a, ak_fake_view *v, ak_prop p, int64_t base, int64_t sub) {
	a->menu = v, a->menu_id = v->id, a->menu_prop = p, a->menu_base = base, a->menu_sub = sub;
}

static bool press(ak_uitest *a, ak_fake_view *v, ak_span text, bool long_press, ak_buf *err) {
	ak_fake_view *p = pressable(v, long_press);
	if (!p) return failure(a, err, long_press ? "nothing at \"%.*s\" takes a long press" : "nothing at \"%.*s\" takes a tap", text);
	if (disabled_view(p)) return failure(a, err, "\"%.*s\" is disabled", text);
	outside_presses(a, p);
	if (!long_press && desktop(a) && p->kind != AK_KIND_TEXT_FIELD && ak_fake_takes_focus(p) && !ak_fake_bool(p, AK_PROP_NO_FOCUS_ON_PRESS)) ak_fake_focus(a->f, p, false);
	if (long_press) {
		if (ak_fake_listens(p, AK_EVENT_LONG_PRESS)) ak_fake_long_press(a->f, p);
		else open_menu(a, p, AK_PROP_MENU, 0, -1);
		return true;
	}
	if (p->kind == AK_KIND_TEXT_FIELD) {
		focus(a, p);
	} else if (p->kind == AK_KIND_TOGGLE) {
		ak_fake_flip(a->f, p);
	} else if (has_menu(p, AK_MENU_TRIGGER_TAP) && (p->kind == AK_KIND_BUTTON || !ak_fake_listens(p, AK_EVENT_PRESS))) {
		// A button's menu shows instead of its press, as every host's does;
		// a view's tap menu only when nothing else takes the press.
		open_menu(a, p, AK_PROP_MENU, 0, -1);
	} else if (p->kind == AK_FAKE_KIND_CELL) {
		ak_fake_view *list = NULL;
		cell_of(a, p, &list);
		bool on = true;
		if (list && ak_fake_bool(list, AK_PROP_LIST_SELECTING)) on = !ak_fake_bool(p, AK_PROP_SELECTED);  // a selecting list's tap toggles the row
		ak_fake_select_row(a->f, p, on);
	} else if (ak_fake_listens(p, AK_EVENT_PRESS) || ak_fake_listens(p, AK_EVENT_PRESS_POINT)) {
		if (ak_fake_listens(p, AK_EVENT_PRESS_POINT)) {
			// Where it began: the view's middle.
			ak_fake_dispatch(a->f, &(ak_event){.id = p->id, .kind = AK_EVENT_PRESS_POINT, .x = p->frame[2] / 2, .y = p->frame[3] / 2});
		}
		if (ak_fake_listens(p, AK_EVENT_PRESS)) ak_fake_tap(a->f, p);
	} else if (p->kind == AK_KIND_NATIVE) {
		// A native package's view (a pay button): its Press event.
		ak_fake_emit_native(a->f, p, "Press", NULL, 0);
	}
	return true;
}

// item_prop is the prop an item numbered i (with its base) of v is in,
// and its base.
static ak_prop item_prop(const ak_fake_view *v, int64_t i, int64_t *base) {
	*base = 0;
	if (v->kind == AK_KIND_ALERT) return AK_PROP_ALERT_BUTTONS;
	if (i >= AK_SCREEN_PRIMARY_ACTION) return *base = AK_SCREEN_PRIMARY_ACTION, AK_PROP_SCREEN_PRIMARY;
	if (i >= AK_SCREEN_TOOLBAR_ACTIONS) return *base = AK_SCREEN_TOOLBAR_ACTIONS, AK_PROP_SCREEN_TOOLBAR;
	return AK_PROP_SCREEN_ACTIONS;
}

// choose_item presses the item numbered i of a screen's bar buttons,
// toolbar or primary action, or an alert's buttons: one with a submenu
// opens it.
static bool choose_item(ak_uitest *a, ak_fake_view *v, int64_t i, ak_span text, ak_buf *err) {
	int64_t base;
	ak_prop p = item_prop(v, i, &base);
	int64_t n = i - base;
	ak_action_vec items = {0};
	ak_fake_action_items(v, p, &items);
	bool off = false, menu = false;
	int64_t sub = -1;
	if (n >= 0 && n < items.len) {
		const ak_action *it = &items.data[n];
		off = it->flags & AK_MENU_DISABLED;
		if (p == AK_PROP_SCREEN_PRIMARY && n == 0 && it->count > 0) menu = true;
		else if ((it->flags & AK_MENU_SUBMENU) && !(it->flags & AK_MENU_INLINE)) menu = true, sub = n;
	}
	ak_vec_free(&items);
	if (off) return failure(a, err, "\"%.*s\" is disabled", text);
	if (menu) {
		open_menu(a, v, p, base, sub);
		return true;
	}
	ak_fake_choose(a->f, v, i);
	return true;
}

// choose_menu_item taps the open menu's item: a submenu opens; another
// closes the menu, chosen.
static bool choose_menu_item(ak_uitest *a, ak_span text, ak_buf *err) {
	ak_action_vec all = {0};
	index_vec shown = {0};
	menu_items(a, &all, &shown);
	int64_t found = -1;
	for (uint32_t i = 0; i < shown.len && found < 0; i++) {
		const ak_action *it = &all.data[shown.data[i]];
		if (ak_span_eq(it->title, text) && !(it->flags & AK_MENU_INLINE)) found = shown.data[i];
	}
	bool ok = true;
	if (found < 0) {
		if (err) {
			ak_buf_printf(err, "the open menu has no \"%.*s\": it has", (int)text.len, text.p);
			for (uint32_t i = 0; i < shown.len; i++) quote_fn(all.data[shown.data[i]].title, err);
		}
		ok = false;
	} else if (all.data[found].flags & AK_MENU_DISABLED) {
		if (err) ak_buf_printf(err, "the menu's \"%.*s\" is disabled", (int)text.len, text.p);
		ok = false;
	} else if (all.data[found].flags & AK_MENU_SUBMENU) {
		a->menu_sub = found;
	} else {
		ak_fake_view *m = a->menu;
		int64_t base = a->menu_base;
		a->menu = NULL;
		ak_fake_choose(a->f, m, base + found);
	}
	ak_vec_free(&all);
	ak_vec_free(&shown);
	return ok;
}

bool ak_uitest_tap(ak_uitest *a, ak_span text, ak_buf *err) {
	if (a->menu) return choose_menu_item(a, text, err) && settled(a, err);
	target t;
	if (!one(a, text, not_label, &t, err)) return false;
	bool ok = true;
	switch (t.how) {
	case BY_VIEW: ok = press(a, t.v, text, false, err); break;
	case BY_TAB:
	case BY_DEST:
	case BY_SEGMENT:
		if (disabled_view(t.v)) return failure(a, err, "\"%.*s\" is disabled", text);
		ak_fake_select(a->f, t.v, t.i);
		break;
	case BY_ITEM: ok = choose_item(a, t.v, t.i, text, err); break;
	case BY_SNACK_ACT: ak_fake_press_snackbar_action(a->f, t.v); break;
	}
	return ok && settled(a, err);
}

bool ak_uitest_long_press(ak_uitest *a, ak_span text, ak_buf *err) {
	target t;
	if (!one(a, text, by_view, &t, err)) return false;
	return press(a, t.v, text, true, err) && settled(a, err);
}

// ak_uitest_disabled reports whether what shows text is disabled: a
// control (or a view around it), a bar button, an alert's button, an
// open menu's item. It fails unless one thing shows text.
bool ak_uitest_disabled(ak_uitest *a, ak_span text, bool *out, ak_buf *err) {
	if (a->menu) {
		ak_action_vec all = {0};
		index_vec shown = {0};
		menu_items(a, &all, &shown);
		bool found = false;
		for (uint32_t i = 0; i < shown.len && !found; i++) {
			const ak_action *it = &all.data[shown.data[i]];
			if (ak_span_eq(it->title, text)) found = true, *out = it->flags & AK_MENU_DISABLED;
		}
		ak_vec_free(&all);
		ak_vec_free(&shown);
		if (found) return true;
	}
	target_vec ts = {0};
	targets(a, text, &ts);
	uint32_t w = 0;
	for (uint32_t i = 0; i < ts.len; i++)
		if (not_shown(&ts.data[i])) ts.data[w++] = ts.data[i];
	ts.len = w;
	dedupe(&ts);
	uint32_t n = ts.len;
	target t = n ? ts.data[0] : (target){0};
	ak_vec_free(&ts);
	if (n != 1) {
		if (err) ak_buf_printf(err, "%u things show ", n);
		return failure(a, err, "\"%.*s\"", text);
	}
	if (t.how == BY_ITEM) {
		int64_t base;
		ak_prop p = item_prop(t.v, t.i, &base);
		ak_action_vec items = {0};
		ak_fake_action_items(t.v, p, &items);
		int64_t k = t.i - base;
		*out = k >= 0 && k < items.len && (items.data[k].flags & AK_MENU_DISABLED);
		ak_vec_free(&items);
		return true;
	}
	if (t.how == BY_VIEW) {
		ak_fake_view *p = pressable(t.v, false);
		if (p) {
			*out = disabled_view(p);
			return true;
		}
	}
	*out = disabled_view(t.v);
	return true;
}

// near is the view of kind k that label names: one of kind k that shows
// label itself (its accessibility label, a field's placeholder), else the
// one next to a view that shows it, as a user reads a form: on its line,
// or below it; or the one in a view that shows it (a view labelled for
// the control it holds).
static ak_fake_view *near(ak_uitest *a, ak_span label, ak_view_kind k, ak_buf *err) {
	target_vec ts = {0};
	targets_in_reach(a, label, &ts);
	ak_fake_view *found = NULL;
	for (uint32_t i = 0; i < ts.len && !found; i++)
		if (ts.data[i].how == BY_VIEW && ts.data[i].v->kind == k) found = ts.data[i].v;
	if (found) {
		ak_vec_free(&ts);
		return found;
	}
	ak_fake_view_vec vs = {0}, cs = {0}, hits = {0}, in = {0};
	visible_touchable(a, &vs);
	for (uint32_t i = 0; i < vs.len; i++)
		if (vs.data[i]->kind == k && (!a->scope.under || contains(a, a->scope.under, vs.data[i]))) ak_vec_push(&cs, vs.data[i]);
	for (uint32_t i = 0; i < ts.len; i++) {
		if (ts.data[i].how != BY_VIEW) continue;
		ak_fake_view *l = ts.data[i].v;
		in.len = 0;
		for (uint32_t j = 0; j < cs.len; j++)
			if (cs.data[j] != l && contains(a, l, cs.data[j])) ak_vec_push(&in, cs.data[j]);
		ak_fake_view *c = in.len == 1 ? in.data[0] : next_to(l, cs.data, cs.len);
		bool dup = false;
		for (uint32_t j = 0; j < hits.len; j++) dup = dup || hits.data[j] == c;
		if (c && !dup) ak_vec_push(&hits, c);
	}
	uint32_t n = hits.len;
	if (n == 1) found = hits.data[0];
	ak_vec_free(&ts);
	ak_vec_free(&vs);
	ak_vec_free(&cs);
	ak_vec_free(&hits);
	ak_vec_free(&in);
	if (n != 1) {
		if (err) {
			if (n) ak_buf_printf(err, "%u ", n);
			else ak_buf_printf(err, "no ");
			ak_fake_kind_name(err, k);
		}
		failure(a, err, " next to \"%.*s\"", label);
	}
	return found;
}

static bool alert_field(ak_uitest *a, ak_span label, ak_fake_view **alert, uint32_t *field) {
	target_vec ts = {0};
	targets_in_reach(a, label, &ts);
	bool ok = false;
	for (uint32_t i = 0; i < ts.len && !ok; i++)
		if (ts.data[i].how == BY_FIELD) *alert = ts.data[i].v, *field = (uint32_t)ts.data[i].i, ok = true;
	ak_vec_free(&ts);
	return ok;
}

static char *cstr(ak_buf *b, ak_span s) {
	ak_buf_reset(b);
	ak_buf_put(b, s.p, s.len);
	ak_buf_u8(b, 0);
	return (char *)b->data;
}

bool ak_uitest_type(ak_uitest *a, ak_span text, ak_span label, ak_buf *err) {
	ak_buf s = {0};
	ak_fake_view *f = a->focused;
	ak_fake_view *alert;
	uint32_t field;
	if (label.len && alert_field(a, label, &alert, &field)) {
		ak_fake_type_in_alert(a->f, alert, field, cstr(&s, text));
		ak_buf_free(&s);
		return settled(a, err);
	}
	if (label.len) {
		f = near(a, label, AK_KIND_TEXT_FIELD, err);
		if (!f) return false;
	} else if (!f) {
		return failure(a, err, "type: no field to type in%.*s; tap one, or name it", sp(""));
	}
	if (disabled_view(f)) return failure(a, err, "the field \"%.*s\" is disabled", label);
	focus(a, f);
	ak_fake_type(a->f, f, cstr(&s, text));
	ak_buf_free(&s);
	return settled(a, err);
}

// ak_uitest_submit presses return in the field label names (as type
// finds it; "": the field typed into last), or an alert's.
bool ak_uitest_submit(ak_uitest *a, ak_span label, ak_buf *err) {
	ak_fake_view *alert;
	uint32_t field;
	if (label.len && alert_field(a, label, &alert, &field)) {
		if (!ak_fake_return_in_alert(a->f, alert)) return failure(a, err, "the alert's default button is disabled%.*s", sp(""));
		return settled(a, err);
	}
	if (label.len) {
		ak_fake_view *f = near(a, label, AK_KIND_TEXT_FIELD, err);
		if (!f) return false;
		focus(a, f);
	}
	if (!a->focused) {
		// The alert typed into last: return in its last field.
		ak_fake_view_vec ls = {0};
		touchable(a, &ls);
		ak_fake_view *top = ls.len ? ls.data[ls.len - 1] : NULL;
		ak_vec_free(&ls);
		if (top && top->kind == AK_KIND_ALERT) {
			if (!ak_fake_return_in_alert(a->f, top)) return failure(a, err, "the alert's default button is disabled%.*s", sp(""));
			return settled(a, err);
		}
		return failure(a, err, "submit with no label: type into a field first%.*s", sp(""));
	}
	ak_fake_submit(a->f, a->focused);
	return settled(a, err);
}

// ak_uitest_toggle flips the switch (or checkbox) label names: one with
// that accessibility label, or the one next to a text.
bool ak_uitest_toggle(ak_uitest *a, ak_span label, ak_buf *err) {
	ak_fake_view *v = near(a, label, AK_KIND_TOGGLE, err);
	if (!v) return false;
	if (disabled_view(v)) return failure(a, err, "the switch \"%.*s\" is disabled", label);
	ak_fake_flip(a->f, v);
	return settled(a, err);
}

// ak_uitest_slide moves the slider label names (as toggle) to value.
bool ak_uitest_slide(ak_uitest *a, ak_span label, double value, ak_buf *err) {
	ak_fake_view *v = near(a, label, AK_KIND_SLIDER, err);
	if (!v) return false;
	if (disabled_view(v)) return failure(a, err, "the slider \"%.*s\" is disabled", label);
	ak_fake_slide(a->f, v, value);
	return settled(a, err);
}

// ak_uitest_pick_color chooses color in the ColorPicker label names (as
// toggle finds it); pick_date an instant (Unix milliseconds) in the
// DatePicker.
bool ak_uitest_pick_color(ak_uitest *a, ak_span label, ak_color color, ak_buf *err) {
	ak_fake_view *v = near(a, label, AK_KIND_COLOR_PICKER, err);
	if (!v) return false;
	if (disabled_view(v)) return failure(a, err, "the color picker \"%.*s\" is disabled", label);
	ak_fake_pick_color(a->f, v, color);
	return settled(a, err);
}

bool ak_uitest_pick_date(ak_uitest *a, ak_span label, int64_t ms, ak_buf *err) {
	ak_fake_view *v = near(a, label, AK_KIND_DATE_PICKER, err);
	if (!v) return false;
	if (disabled_view(v)) return failure(a, err, "the date picker \"%.*s\" is disabled", label);
	ak_fake_pick_date(a->f, v, ms);
	return settled(a, err);
}

// ak_uitest_select chooses option in the picker, wheel or segmented
// control that has it.
bool ak_uitest_select(ak_uitest *a, ak_span option, ak_buf *err) {
	ak_fake_view_vec vs = {0};
	visible_touchable(a, &vs);
	ak_fake_view *found = NULL;
	int64_t index = -1;
	uint32_t n = 0;
	for (uint32_t i = 0; i < vs.len; i++) {
		ak_fake_view *v = vs.data[i];
		ak_prop p;
		switch (v->kind) {
		case AK_KIND_PICKER: p = AK_PROP_PICKER_ITEMS; break;
		case AK_KIND_WHEEL: p = AK_PROP_WHEEL_ITEMS; break;
		case AK_KIND_SEGMENTED: p = AK_PROP_SEGMENTED_SEGMENTS; break;
		default: continue;
		}
		ak_str_vec items = {0};
		ak_fake_strings(v, p, &items);
		for (uint32_t j = 0; j < items.len; j++)
			if (ak_span_eq(items.data[j], option)) {
				found = v, index = j, n++;
				break;
			}
		ak_vec_free(&items);
	}
	ak_vec_free(&vs);
	if (n != 1) {
		if (err) ak_buf_printf(err, "%u pickers have ", n);
		return failure(a, err, "\"%.*s\"", option);
	}
	if (disabled_view(found)) return failure(a, err, "the picker with \"%.*s\" is disabled", option);
	ak_fake_select(a->f, found, index);
	return settled(a, err);
}

// ak_uitest_swipe swipes the list row showing text to reveal its actions
// and presses the one titled action: a trailing one, or a leading one.
bool ak_uitest_swipe(ak_uitest *a, ak_span text, ak_span action, ak_buf *err) {
	target t;
	if (!one(a, text, by_view, &t, err)) return false;
	ak_fake_view *cell = cell_of(a, t.v, NULL);
	if (!cell) return failure(a, err, "\"%.*s\" isn't in a list's row", text);
	ak_buf s = {0};
	const char *name = cstr(&s, action);
	int64_t i = ak_fake_action_named(cell, AK_PROP_SWIPE_ACTIONS, name), j = -1;
	if (i < 0) j = ak_fake_action_named(cell, AK_PROP_LEADING_SWIPE_ACTIONS, name);
	ak_buf_free(&s);
	if (i < 0 && j < 0) {
		if (err) {
			ak_buf_printf(err, "the row \"%.*s\" has no swipe action \"%.*s\": it has", (int)text.len, text.p, (int)action.len, action.p);
			items_text(cell, AK_PROP_SWIPE_ACTIONS, quote_fn, err);
			items_text(cell, AK_PROP_LEADING_SWIPE_ACTIONS, quote_fn, err);
		}
		return false;
	}
	ak_fake_dispatch(a->f, &(ak_event){.id = cell->id, .kind = AK_EVENT_ACTION, .i = i >= 0 ? i : -1 - j});
	return settled(a, err);
}

// ak_uitest_back goes back as the user's back gesture or button does
// (Android's back, Escape): an open menu closes; else the top screen of
// the deepest stack in reach pops; else the top sheet, alert or popover
// is dismissed (onBack in it can keep it); else, on Android, the root
// takes Back if it intercepts it. It fails if there's nothing to go back
// from.
bool ak_uitest_back(ak_uitest *a, ak_buf *err) {
	if (a->menu) {
		a->menu = NULL;
		return true;
	}
	ak_fake_view_vec vs = {0}, ls = {0};
	visible_touchable(a, &vs);
	ak_fake_view *stack = NULL;
	for (uint32_t i = 0; i < vs.len; i++)
		if (vs.data[i]->kind == AK_KIND_STACK && vs.data[i]->children.len > 1) stack = vs.data[i];
	ak_vec_free(&vs);
	touchable(a, &ls);
	ak_fake_view *top = ls.len ? ls.data[0] : NULL;
	ak_vec_free(&ls);
	// An overlay that takes going back, on top, first.
	ak_fake_view_vec ps = {0};
	ak_fake_presented(a->f, &ps);
	ak_fake_view *ov = NULL;
	for (uint32_t i = ps.len; i-- > 0 && !ov;)
		if (ps.data[i]->kind == AK_KIND_OVERLAY && ps.data[i]->children.len && ak_fake_bool(ps.data[i], AK_PROP_INTERCEPT_BACK)) ov = ps.data[i];
	ak_vec_free(&ps);
	if (ov) ak_fake_dismiss(a->f, ov);
	else if (stack) ak_fake_back(a->f, stack);
	else if (top && modal(top)) ak_fake_dismiss(a->f, top);
	else if (!(a->f->env.platform == AK_PLATFORM_ANDROID && ak_fake_back_from_root(a->f)))
		return failure(a, err, "nothing to go back from%.*s", sp(""));
	return settled(a, err);
}

// ak_uitest_dismiss dismisses what's on top as the user does (a swipe
// down, a tap outside, Escape): an open menu, else the top alert, sheet
// or popover.
bool ak_uitest_dismiss(ak_uitest *a, ak_buf *err) {
	if (a->menu) {
		a->menu = NULL;
		return true;
	}
	ak_fake_view_vec ls = {0};
	touchable(a, &ls);
	ak_fake_view *top = ls.len ? ls.data[0] : NULL;
	ak_vec_free(&ls);
	// An overlay on top (modal or not): a press outside it.
	ak_fake_view_vec ps = {0};
	ak_fake_presented(a->f, &ps);
	ak_fake_view *ov = NULL;
	for (uint32_t i = ps.len; i-- > 0 && !ov;)
		if (ps.data[i]->kind == AK_KIND_OVERLAY && ps.data[i]->children.len && !hidden(ps.data[i])) ov = ps.data[i];
	ak_vec_free(&ps);
	if (ov && (!top || top == ov || !modal(top) || ov->present_seq > top->present_seq)) {
		ak_fake_outside_press(a->f, ov, -1, -1);
		return settled(a, err);
	}
	if (!top || !modal(top)) return failure(a, err, "nothing to dismiss%.*s", sp(""));
	ak_fake_dismiss(a->f, top);
	return settled(a, err);
}

// ak_uitest_refresh pulls the scroll view or list that shows text to
// refresh it.
bool ak_uitest_refresh(ak_uitest *a, ak_span text, ak_buf *err) {
	target t;
	if (!one(a, text, by_view, &t, err)) return false;
	ak_fake_view *v = t.v, *list = NULL;
	if (cell_of(a, v, &list) && list) v = list;
	for (ak_fake_view *p = v; p; p = p->parent)
		if ((p->kind == AK_KIND_SCROLL || p->kind == AK_KIND_LIST) && ak_fake_listens(p, AK_EVENT_REFRESH)) {
			ak_fake_pull_to_refresh(a->f, p);
			return settled(a, err);
		}
	return failure(a, err, "nothing around \"%.*s\" refreshes", text);
}

// ak_uitest_scroll_to scrolls the lists on the screen until one shows
// text, as a user would scroll looking for it, and fails if none does.
bool ak_uitest_scroll_to(ak_uitest *a, ak_span text, ak_buf *err) {
	if (ak_uitest_has(a, text)) return true;
	ak_fake_view_vec vs = {0};
	visible(a, &vs);
	bool ok = false;
	for (uint32_t i = 0; i < vs.len && !ok; i++) {
		if (vs.data[i]->kind != AK_KIND_LIST) continue;
		list_state *st = list_of(a, vs.data[i]->id);
		if (!st) continue;
		st->end = false;
		st->scrolled = true;
		for (int64_t first = 0; first < st->count && !ok;) {
			st->first = first;
			ak_uitest_settle(a);
			st = list_of(a, vs.data[i]->id);
			if (!st) break;
			if (ak_uitest_has(a, text)) ok = true;
			uint32_t n = st->cells.len;
			if (!n) break;
			first += n;
		}
	}
	ak_vec_free(&vs);
	if (!ok) return failure(a, err, "no list on the screen shows \"%.*s\", scrolled to its end", text);
	return check_fails(a, err);
}

// ak_uitest_until settles, then moves the clock (a manual one, under
// test) to each timer as it comes due and settles again, until cond
// holds; it fails once no timer is left, or ten minutes of the app's time
// have gone, and cond still doesn't.
bool ak_uitest_until(ak_uitest *a, bool (*cond)(void *ctx), void *ctx, ak_span what, ak_buf *err) {
	int64_t start = ak_sched_now();
	for (;;) {
		ak_uitest_settle(a);
		if (!check_fails(a, err)) return false;
		if (cond(ctx)) return true;
		if (ak_error_pending()) return false;
		int64_t at;
		if (!ak_sched_next_timer(&at) || at - start > 600 * (int64_t)1000000000)
			return failure(a, err, "waited for %.*s: it never came", what);
		int64_t now = ak_sched_now();
		ak_sched_advance(at > now ? at - now : 0);
	}
}

typedef struct {
	ak_uitest *a;
	ak_span text;
	bool gone;
} shows_ctx;

static bool shows_cond(void *ctx) {
	shows_ctx *s = ctx;
	return ak_uitest_has(s->a, s->text) != s->gone;
}

bool ak_uitest_wait_for(ak_uitest *a, ak_span text, bool gone, ak_buf *err) {
	shows_ctx c = {a, text, gone};
	ak_buf what = {0};
	ak_buf_printf(&what, gone ? "\"%.*s\" to go" : "\"%.*s\" to show", (int)text.len, text.p);
	bool ok = ak_uitest_until(a, shows_cond, &c, ak_strn((const char *)what.data, what.len), err);
	ak_buf_free(&what);
	return ok;
}

bool ak_uitest_frame(ak_uitest *a, ak_span text, float out[4], ak_buf *err) {
	target_vec ts = {0};
	targets(a, text, &ts);
	ak_fake_view *v = ts.len ? ts.data[0].v : NULL;
	ak_vec_free(&ts);
	if (!v) return failure(a, err, "nothing shows %.*s", text);
	out[0] = v->frame[0], out[1] = v->frame[1], out[2] = v->frame[2], out[3] = v->frame[3];
	for (ak_fake_view *p = v->parent; p; p = p->parent) out[0] += p->frame[0], out[1] += p->frame[1];
	return true;
}

static bool has_bytes(ak_span hay, ak_span needle) {
	if (needle.len == 0 || needle.len > hay.len) return false;
	for (size_t i = 0; i + needle.len <= hay.len; i++)
		if (memcmp(hay.p + i, needle.p, needle.len) == 0) return true;
	return false;
}

bool ak_uitest_link(ak_uitest *a, ak_span url, ak_buf *err) {
	ak_fake_view_vec vs = {0};
	ak_fake_of_kind(a->f, AK_KIND_RICH_TEXT, &vs);
	ak_fake_view *v = NULL;
	for (uint32_t i = 0; i < vs.len && !v; i++)
		if (has_bytes(ak_fake_string(vs.data[i], AK_PROP_RICH_TEXT_CONTENT), url)) v = vs.data[i];
	ak_vec_free(&vs);
	if (!v) return failure(a, err, "no rich text links to %.*s", url);
	ak_fake_link(a->f, v, url);
	return settled(a, err);
}

void ak_uitest_set_languages(ak_uitest *a, ak_span languages) {
	ak_environment env = a->f->env;
	env.languages = languages;
	ak_fake_set_environment(a->f, &env);
	ak_uitest_settle(a);
}

bool ak_uitest_expect(ak_uitest *a, ak_span text, ak_buf *err) {
	if (!check_fails(a, err)) return false;
	return ak_uitest_has(a, text) ? true : failure(a, err, "the screen doesn't show \"%.*s\"", text);
}

bool ak_uitest_expect_no(ak_uitest *a, ak_span text, ak_buf *err) {
	if (!check_fails(a, err)) return false;
	return !ak_uitest_has(a, text) ? true : failure(a, err, "the screen shows \"%.*s\"", text);
}

// ---- The test.* builtins ----
//
//	name             arguments                                     result
//	test.mount       build: fn() -> View, platform: int (AK_PLATFORM_*;
//	                 -1: asuka test's), at: string (where the Router
//	                 starts; "": its initial)                     App (an opaque handle)
//	test.tap         app: App, text: string                        -
//	test.type        app: App, text: string, into: string ("":
//	                 the field tapped or typed in last)            -
//	test.expect      app: App, texts: string or [string]           -
//	test.expect_no   app: App, texts: string or [string]           -
//	test.idle        app: App                                      Task<void> (settled; the
//	                                                               clock moved past every timer)
//	test.dump        app: App                                      string (what the screen shows)
//	test.ops         app: App                                      string (the ops applied since
//	                                                               the last test.ops, a line each)
//	test.press       app: App, text: string, down: bool            - (the pressed state of the
//	                                                               view showing text, or around it)
//	test.hover       app: App, text: string, over: bool            - (likewise, hovered)
//	test.frame       app: App, time: Duration                      - (a frame the display draws)
//	test.frame_of    app: App, text: string                        (x, y, width, height): the
//	                                                               view showing text, in the window
//	test.set_languages app: App, tags: string                      - (the user's languages changed)
//	test.open_url    app: App, url: string                         - (the app opened with it)
//	test.link        app: App, url: string                         - (the user taps a rich text's link)
//	test.share       app: App, items: [SharedItem]                 - (another app shares them: the
//	                                                               root's Shared event)
//	test.emit        app: App, type: string, event: string,
//	                 value: string                                 - (a native view's event, its
//	                                                               one value a string)
//
// A failed action or expectation throws an error saying what's wrong and
// what the screen shows. The app is unmounted when the test's owner is
// disposed (the test ends), and a native view that outlives it fails the
// test.

// A scoped handle (app.in(.toolbar), app.nextTo("Ada")): the app, and
// the scope its calls look in.
typedef struct {
	ak_opaque h;
	ak_uitest *app;
	uint8_t region;
	ak_str *next_to;
} scoped;

static void scoped_drop(ak_opaque *o) {
	scoped *s = (scoped *)o;
	ak_release(s->app);
	ak_release(s->next_to);
}

static ak_uitest *app_arg(ak_val *a, uint32_t n, const char *name) {
	ak_opaque *o = n >= 1 ? a[0].p : NULL;
	if (!o || o->h.tag != AK_TAG_OPAQUE || (o->drop != uitest_drop && o->drop != scoped_drop))
		ak_panic("%s: its first argument isn't an app from test.mount", name);
	if (o->drop == scoped_drop) {
		scoped *s = (scoped *)o;
		s->app->scope = (scope){.region = s->region, .next_to = s->next_to};
		return s->app;
	}
	ak_uitest *app = (ak_uitest *)o;
	app->scope = (scope){0};
	return app;
}

static ak_span span_of(ak_val v) {
	const ak_str *s = v.p;
	return s ? ak_strn(s->data, s->len) : sp("");
}

static void throw_buf(ak_buf *b) {
	ak_buf_u8(b, 0);
	ak_throw_msg("%s", (const char *)b->data);
	ak_buf_free(b);
}

static void unmount_cleanup(void *ctx) {
	ak_uitest *a = ctx;
	ak_buf err = {0};
	if (!ak_uitest_unmount(a, &err)) {
		// The test fails: to the catcher of what the test runs in.
		ak_buf_u8(&err, 0);
		ak_throw_msg("%s", (const char *)err.data);
		ak_raise(ak_error_take());
	}
	ak_buf_free(&err);
	ak_release(a);
}

static ak_val t_mount(ak_val *a, uint32_t n) {
	if (n < 1 || !a[0].p) {
		ak_throw_msg("mount: no view to mount");
		return (ak_val){0};
	}
	// The platform it runs on (-1: asuka test's), and where its Router
	// starts ("": its own initial).
	ak_uitest *app = ak_uitest_mount_at(a[0].p, NULL, n > 1 && a[1].i > 0 ? (int)a[1].i : -1, n > 2 ? span_of(a[2]) : sp(""));
	if (ak_current_owner()) ak_on_cleanup(unmount_cleanup, ak_retain(app));
	return (ak_val){.p = app};
}

static ak_val t_tap(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.tap");
	ak_buf err = {0};
	if (!ak_uitest_tap(app, span_of(a[1]), &err)) throw_buf(&err);
	ak_buf_free(&err);
	return (ak_val){0};
}

static ak_val t_type(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.type");
	ak_buf err = {0};
	if (!ak_uitest_type(app, span_of(a[1]), n > 2 ? span_of(a[2]) : sp(""), &err)) throw_buf(&err);
	ak_buf_free(&err);
	return (ak_val){0};
}

static ak_val expect(ak_val *a, uint32_t n, bool no, const char *name) {
	ak_uitest *app = app_arg(a, n, name);
	ak_buf err = {0};
	const ak_obj *o = n > 1 ? a[1].p : NULL;
	bool ok = true;
	if (o && o->tag == AK_TAG_ARR) {
		const ak_arr *arr = (const ak_arr *)o;
		for (uint32_t i = 0; ok && i < arr->len; i++)
			ok = no ? ak_uitest_expect_no(app, span_of(arr->data[i]), &err) : ak_uitest_expect(app, span_of(arr->data[i]), &err);
	} else {
		ok = no ? ak_uitest_expect_no(app, span_of(a[1]), &err) : ak_uitest_expect(app, span_of(a[1]), &err);
	}
	if (!ok) throw_buf(&err);
	ak_buf_free(&err);
	return (ak_val){0};
}

static ak_val t_expect(ak_val *a, uint32_t n) { return expect(a, n, false, "test.expect"); }
static ak_val t_expect_no(ak_val *a, uint32_t n) { return expect(a, n, true, "test.expect_no"); }

static ak_val t_idle(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.idle");
	ak_uitest_idle(app);
	ak_future *f = ak_future_new(AK_K_VOID);
	ak_future_resolve(f, (ak_val){0});
	return (ak_val){.p = f};
}

static const uint8_t frame_kinds[] = {AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT};
static const ak_shape *const no_shapes[4];
static const ak_shape frame_tuple = {"(float,float,float,float)", 4, frame_kinds, no_shapes, NULL, 0, NULL, false, NULL};

static ak_val t_frame_of(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.frame_of");
	ak_buf err = {0};
	float f[4];
	if (!ak_uitest_frame(app, span_of(a[1]), f, &err)) {
		throw_buf(&err);
		return (ak_val){0};
	}
	ak_buf_free(&err);
	ak_struct *s = ak_struct_new(&frame_tuple);
	for (int i = 0; i < 4; i++) s->f[i].f = f[i];
	return (ak_val){.p = s};
}

static ak_val t_set_languages(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.set_languages");
	ak_uitest_set_languages(app, span_of(a[1]));
	return (ak_val){0};
}

static ak_val t_dump(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.dump");
	ak_buf b = {0};
	ak_uitest_snapshot(app, &b);
	ak_str *s = ak_str_new((const char *)b.data, b.len);
	ak_buf_free(&b);
	return (ak_val){.p = s};
}

static ak_val t_ops(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.ops");
	ak_uitest_settle(app);
	ak_fake *f = app->f;
	ak_buf b = {0};
	for (uint32_t i = app->ops_seen; i < f->ops.len; i++) {
		ak_op op = ak_fake_op_at(f, i);
		if (op.code == AK_OP_SET_PROP) {
			const ak_fake_view *v = ak_fake_view_of(f, op.id);
			ak_op_format_kind(&b, &op, v ? v->kind : 0);
		} else {
			ak_op_format(&b, &op);
		}
		ak_buf_u8(&b, '\n');
	}
	app->ops_seen = f->ops.len;
	ak_str *s = ak_str_new((const char *)b.data, b.len);
	ak_buf_free(&b);
	return (ak_val){.p = s};
}

// state_target is the view showing text that reports kind (it, or the
// nearest around it), or NULL and a thrown error.
static ak_fake_view *state_target(ak_uitest *app, ak_span text, ak_event_kind kind, const char *name) {
	target_vec ts = {0};
	targets(app, text, &ts);
	ak_fake_view *found = NULL;
	for (uint32_t i = 0; i < ts.len && !found; i++)
		for (ak_fake_view *p = ts.data[i].v; p && !found; p = p->parent)
			if (ak_fake_listens(p, kind)) found = p;
	ak_vec_free(&ts);
	if (!found) {
		ak_buf err = {0};
		failure(app, &err, name, text);
		throw_buf(&err);
	}
	return found;
}

static ak_val state_event(ak_val *a, uint32_t n, ak_event_kind kind, const char *name, const char *msg) {
	ak_uitest *app = app_arg(a, n, name);
	ak_fake_view *v = state_target(app, span_of(a[1]), kind, msg);
	if (v) {
		ak_fake_dispatch(app->f, &(ak_event){.id = v->id, .kind = kind, .b = n > 2 && a[2].i});
		ak_uitest_settle(app);
	}
	return (ak_val){0};
}

static ak_val t_press(ak_val *a, uint32_t n) { return state_event(a, n, AK_EVENT_PRESSED, "test.press", "nothing at \"%.*s\" reports its pressed state"); }
static ak_val t_hover(ak_val *a, uint32_t n) { return state_event(a, n, AK_EVENT_HOVER, "test.hover", "nothing at \"%.*s\" reports hovering"); }

static ak_val t_frame(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.frame");
	ak_fake_view *root = ak_fake_root(app->f);
	ak_fake_dispatch(app->f, &(ak_event){.id = root ? root->id : 0, .kind = AK_EVENT_FRAME, .f = n > 1 ? (double)a[1].i / 1e9 : 0});
	ak_uitest_settle(app);
	return (ak_val){0};
}

static ak_val t_open_url(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.open_url");
	ak_buf u = {0};
	ak_span s = span_of(a[1]);
	ak_buf_put(&u, s.p, s.len);
	ak_buf_u8(&u, 0);
	ak_fake_open_url(app->f, (const char *)u.data);
	ak_buf_free(&u);
	ak_uitest_settle(app);
	return (ak_val){0};
}

static ak_val t_lifecycle(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.lifecycle");
	ak_fake_lifecycle(app->f, n > 1 ? a[1].i : 0);
	ak_uitest_settle(app);
	return (ak_val){0};
}

// t_share: another app shares items with the app, as a host sends them
// (the Shared event, to the root): onShare hears them, or the first
// that comes does.
static ak_val t_share(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.share");
	const ak_arr *xs = n > 1 ? a[1].p : NULL;
	uint32_t count = xs ? xs->len : 0;
	ak_item *items = ak_alloc((count ? count : 1) * sizeof *items);
	ak_str **made = ak_alloc((count ? count : 1) * sizeof *made);
	for (uint32_t i = 0; i < count; i++) ak_item_wire(xs->data[i].p, false, &items[i], &made[i]);
	ak_buf b = {0};
	ak_append_items(&b, items, count);
	for (uint32_t i = 0; i < count; i++) ak_release(made[i]);
	ak_free(made);
	ak_free(items);
	ak_fake_view *root = ak_fake_root(app->f);
	ak_fake_dispatch(app->f, &(ak_event){.id = root ? root->id : 0, .kind = AK_EVENT_SHARED, .s = ak_strn((const char *)b.data, b.len)});
	ak_buf_free(&b);
	ak_uitest_settle(app);
	return (ak_val){0};
}

static ak_val t_link(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.link");
	ak_buf err = {0};
	if (!ak_uitest_link(app, span_of(a[1]), &err)) throw_buf(&err);
	ak_buf_free(&err);
	return (ak_val){0};
}

static ak_val t_emit(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.emit");
	ak_span type = span_of(a[1]);
	ak_fake_view_vec vs = {0};
	ak_fake_of_kind(app->f, AK_KIND_NATIVE, &vs);
	ak_fake_view *v = NULL;
	for (uint32_t i = 0; i < vs.len && !v; i++)
		if (ak_span_eq(ak_fake_string(vs.data[i], AK_PROP_NATIVE_TYPE), type)) v = vs.data[i];
	ak_vec_free(&vs);
	if (!v) {
		ak_buf err = {0};
		failure(app, &err, "no native view of type \"%.*s\"", type);
		throw_buf(&err);
		return (ak_val){0};
	}
	ak_buf val = {0}, name = {0};
	ak_native_encode(&val, n > 3 ? a[3] : (ak_val){0}, "s", NULL);
	ak_span ev = span_of(a[2]);
	ak_buf_put(&name, ev.p, ev.len);
	ak_buf_u8(&name, 0);
	ak_fake_native x = {.name = ak_cstr("v"), .type = 5, .s = {(const char *)val.data, (uint32_t)val.len}};
	ak_fake_emit_native(app->f, v, (const char *)name.data, &x, 1);
	ak_buf_free(&val);
	ak_buf_free(&name);
	ak_uitest_settle(app);
	return (ak_val){0};
}


// done throws err's message when ok is false, and frees it.
static ak_val done(bool ok, ak_buf *err) {
	if (!ok) throw_buf(err);
	else ak_buf_free(err);
	return (ak_val){0};
}

static ak_future *resolved(void) {
	ak_future *f = ak_future_new(AK_K_VOID);
	ak_future_resolve(f, (ak_val){0});
	return f;
}

static ak_val t_long_press(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.long_press");
	ak_buf err = {0};
	return done(ak_uitest_long_press(app, span_of(a[1]), &err), &err);
}

static ak_val t_disabled(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.disabled");
	ak_buf err = {0};
	bool off = false;
	if (!ak_uitest_disabled(app, span_of(a[1]), &off, &err)) {
		throw_buf(&err);
		return (ak_val){0};
	}
	ak_buf_free(&err);
	return (ak_val){.i = off};
}

static ak_val t_has(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.has");
	return (ak_val){.i = ak_uitest_has(app, span_of(a[1]))};
}

static ak_val t_submit(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.submit");
	ak_buf err = {0};
	return done(ak_uitest_submit(app, n > 1 ? span_of(a[1]) : sp(""), &err), &err);
}

static ak_val t_toggle(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.toggle");
	ak_buf err = {0};
	return done(ak_uitest_toggle(app, span_of(a[1]), &err), &err);
}

static ak_val t_slide(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.slide");
	ak_buf err = {0};
	return done(ak_uitest_slide(app, span_of(a[1]), n > 2 ? a[2].f : 0, &err), &err);
}

static ak_val t_pick_color(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.pick_color");
	ak_buf err = {0};
	return done(ak_uitest_pick_color(app, span_of(a[1]), n > 2 ? (ak_color)a[2].i : 0, &err), &err);
}

static ak_val t_pick_date(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.pick_date");
	ak_buf err = {0};
	return done(ak_uitest_pick_date(app, span_of(a[1]), n > 2 ? a[2].i : 0, &err), &err);
}

static ak_val t_select(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.select");
	ak_buf err = {0};
	return done(ak_uitest_select(app, span_of(a[1]), &err), &err);
}

static ak_val t_swipe(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.swipe");
	ak_buf err = {0};
	return done(ak_uitest_swipe(app, span_of(a[1]), n > 2 ? span_of(a[2]) : sp(""), &err), &err);
}

static ak_val t_back(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.back");
	ak_buf err = {0};
	return done(ak_uitest_back(app, &err), &err);
}

static ak_val t_dismiss(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.dismiss");
	ak_buf err = {0};
	return done(ak_uitest_dismiss(app, &err), &err);
}

static ak_val t_refresh(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.refresh");
	ak_buf err = {0};
	return done(ak_uitest_refresh(app, span_of(a[1]), &err), &err);
}

static ak_val t_scroll_to(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.scroll_to");
	ak_buf err = {0};
	return done(ak_uitest_scroll_to(app, span_of(a[1]), &err), &err);
}

static ak_val t_texts(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.texts");
	ak_uitest_settle(app);
	ak_arr *out = ak_arr_new(AK_K_REF, NULL, 0);
	ak_uitest_texts(app, text_push_fn, &out);
	return (ak_val){.p = out};
}

typedef struct {
	ak_closure *cond;
} until_ctx;

static bool until_cond(void *ctx) {
	until_ctx *u = ctx;
	ak_val v = ak_call(u->cond, NULL, 0);
	return !ak_error_pending() && v.i;
}

static ak_val t_until(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.until");
	ak_buf err = {0};
	until_ctx u = {n > 2 ? a[2].p : NULL};
	if (!u.cond) ak_panic("test.until: no condition");
	scope sc = app->scope;
	bool ok = ak_uitest_until(app, until_cond, &u, span_of(a[1]), &err);
	app->scope = sc;
	if (ak_error_pending()) {
		ak_buf_free(&err);
		return (ak_val){0};
	}
	done(ok, &err);
	return (ak_val){.p = ok ? resolved() : NULL};
}

static ak_val wait_text(ak_val *a, uint32_t n, bool gone, const char *name) {
	ak_uitest *app = app_arg(a, n, name);
	ak_buf err = {0};
	bool ok = ak_uitest_wait_for(app, span_of(a[1]), gone, &err);
	done(ok, &err);
	return (ak_val){.p = ok ? resolved() : NULL};
}

static ak_val t_wait_for(ak_val *a, uint32_t n) { return wait_text(a, n, false, "test.wait_for"); }
static ak_val t_wait_gone(ak_val *a, uint32_t n) { return wait_text(a, n, true, "test.wait_gone"); }

static ak_val t_href(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.href");
	ak_uitest_settle(app);
	ak_fake_history *h = ak_fake_history_of(app->f, NULL);
	if (!h || h->index < 0 || h->index >= (int32_t)h->entries.len) return (ak_val){.p = ak_str_new("", 0)};
	ak_buf *e = &h->entries.data[h->index].href;
	return (ak_val){.p = ak_str_new((const char *)e->data, e->len)};
}

static const uint8_t history_kinds[] = {AK_K_REF, AK_K_INT};
static const ak_shape *const history_shapes[2];
static const ak_shape history_tuple = {"([string],int)", 2, history_kinds, history_shapes, NULL, 0, NULL, false, NULL};

static ak_val t_history(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.history");
	ak_uitest_settle(app);
	ak_fake_history *h = ak_fake_history_of(app->f, NULL);
	ak_arr *hrefs = ak_arr_new(AK_K_REF, NULL, h ? h->entries.len : 0);
	for (uint32_t i = 0; h && i < h->entries.len; i++) {
		ak_buf *e = &h->entries.data[i].href;
		ak_arr_push(&hrefs, (ak_val){.p = ak_str_new((const char *)e->data, e->len)});
	}
	ak_struct *s = ak_struct_new(&history_tuple);
	s->f[0].p = hrefs;
	s->f[1].i = h ? h->index : -1;
	return (ak_val){.p = s};
}

static ak_val go(ak_val *a, uint32_t n, int32_t by, const char *name) {
	ak_uitest *app = app_arg(a, n, name);
	ak_uitest_settle(app);
	ak_buf err = {0};
	bool ok = ak_fake_go(app->f, NULL, by) || failure(app, &err, by < 0 ? "no history to go back in%.*s" : "no history to go forward in%.*s", sp(""));
	if (ok) ok = settled(app, &err);
	return done(ok, &err);
}

static ak_val t_go_back(ak_val *a, uint32_t n) { return go(a, n, -1, "test.go_back"); }
static ak_val t_go_forward(ak_val *a, uint32_t n) { return go(a, n, 1, "test.go_forward"); }

static ak_val scoped_new(ak_uitest *app, uint8_t region, ak_str *next_to) {
	scoped *s = ak_opaque_new(AK_TAG_OPAQUE, sizeof(scoped), scoped_drop);
	s->app = ak_retain(app);
	s->region = region;
	s->next_to = next_to ? ak_retain(next_to) : NULL;
	return (ak_val){.p = s};
}

// test.in's region is TestScope's case: content, navigationBar, toolbar,
// tabBar, sidebar (IN_CONTENT...).
static ak_val t_in(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.in");
	return scoped_new(app, (uint8_t)(IN_CONTENT + (n > 1 ? a[1].i : 0)), NULL);
}

static ak_val t_next_to(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.next_to");
	return scoped_new(app, IN_ANYWHERE, n > 1 ? a[1].p : NULL);
}

// ---- The desktop ----

static ak_closure *app_build;  // the module's app function (ak_uitest_set_app)

void ak_uitest_set_app(ak_closure *build) {
	if (build) ak_retain(build);
	ak_release(app_build);
	app_build = build;
}

static ak_val t_mount_app(ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	if (!app_build) {
		ak_throw_msg("mountApp: the program has no app");
		return (ak_val){0};
	}
	ak_uitest *app = ak_uitest_mount(app_build, NULL);
	if (ak_current_owner()) ak_on_cleanup(unmount_cleanup, ak_retain(app));
	return (ak_val){.p = app};
}

static ak_val str_of(ak_buf *b) {
	ak_str *s = ak_str_new((const char *)b->data, b->len);
	ak_buf_free(b);
	return (ak_val){.p = s};
}

// window_arg is the window titled a[i] ("": the main one), or NULL and a
// thrown error.
static ak_fake_view *window_arg(ak_uitest *app, ak_val *a, uint32_t n, uint32_t i) {
	ak_span t = i < n ? span_of(a[i]) : sp("");
	ak_fake_view *v = ak_fake_window_titled(app->f, t);
	if (!v) {
		ak_buf err = {0};
		ak_buf_printf(&err, "no window titled \"%.*s\": there are\n", (int)t.len, t.p);
		ak_fake_windows(app->f, &err);
		throw_buf(&err);
	}
	return v;
}

static ak_val t_windows(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.windows");
	ak_uitest_settle(app);
	ak_buf b = {0};
	ak_fake_windows(app->f, &b);
	return str_of(&b);
}

static ak_val t_close_window(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.close_window");
	ak_uitest_settle(app);
	ak_fake_view *v = window_arg(app, a, n, 1);
	if (v) {
		ak_fake_close_window(app->f, v);
		ak_uitest_settle(app);
	}
	return (ak_val){0};
}

static ak_val t_window_actions(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.window_actions");
	ak_uitest_settle(app);
	ak_fake_view *v = window_arg(app, a, n, 1);
	ak_buf b = {0};
	if (v) ak_fake_window_actions(app->f, v, &b);
	return str_of(&b);
}

static ak_val t_set_window_state(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.set_window_state");
	ak_uitest_settle(app);
	ak_fake_view *v = window_arg(app, a, n, 1);
	const ak_struct *s = n > 2 ? a[2].p : NULL;
	if (v && s) {
		ak_window_state st = {(float)s->f[0].f, (float)s->f[1].f, (float)s->f[2].f, (float)s->f[3].f, 0};
		for (int i = 0; i < 7; i++)
			if (s->f[4 + i].i) st.flags |= (uint8_t)(1 << i);
		ak_fake_set_window_state(app->f, v, &st);
		ak_uitest_settle(app);
	}
	return (ak_val){0};
}

static ak_val t_focus_window(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.focus_window");
	ak_uitest_settle(app);
	ak_fake_view *v = window_arg(app, a, n, 1);
	if (v) {
		ak_fake_focus_window(app->f, v);
		ak_uitest_settle(app);
	}
	return (ak_val){0};
}

static ak_val t_press_key(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.press_key");
	ak_uitest_settle(app);
	ak_buf k = {0};
	ak_span s = span_of(a[1]);
	ak_buf_put(&k, s.p, s.len);
	ak_buf_u8(&k, 0);
	uint8_t mods = 0;
	for (uint32_t i = 2; i < n && i < 6; i++)
		if (a[i].i) mods |= (uint8_t)(1 << (i - 2));
	bool took = ak_fake_key(app->f, NULL, (const char *)k.data, mods);
	ak_buf_free(&k);
	ak_uitest_settle(app);
	return (ak_val){.i = took};
}

// first_text_fn keeps the first text it hears.
static void first_text_fn(ak_span s, void *ctx) {
	ak_buf *b = ctx;
	if (!b->len) ak_buf_put(b, s.p, s.len);
}

// focused_text is what the view with the keyboard's focus shows: its own
// text, else the first of the views in it ("": nothing has the focus).
static void focused_text(const ak_fake_view *v, ak_buf *out) {
	if (!v || out->len) return;
	shows(v, first_text_fn, out);
	for (uint32_t i = 0; i < v->children.len && !out->len; i++) focused_text(v->children.data[i], out);
}

static ak_val t_focused(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.focused");
	ak_uitest_settle(app);
	ak_buf b = {0};
	focused_text(ak_fake_focused(app->f), &b);
	ak_str *s = ak_str_new((const char *)b.data, (uint32_t)b.len);
	ak_buf_free(&b);
	return (ak_val){.p = s};
}

static ak_val t_tab(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.tab");
	ak_uitest_settle(app);
	ak_fake_tab(app->f, n > 1 && a[1].i);
	ak_uitest_settle(app);
	return (ak_val){0};
}

static ak_val t_menu_bar(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.menu_bar");
	ak_uitest_settle(app);
	ak_buf b = {0};
	ak_fake_menu_bar(app->f, &b);
	return str_of(&b);
}

static ak_val t_choose_menu_item(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.choose_menu_item");
	ak_uitest_settle(app);
	ak_buf err = {0};
	if (!ak_fake_choose_menu_item(app->f, span_of(a[1]), &err)) throw_buf(&err);
	else ak_buf_free(&err), ak_uitest_settle(app);
	return (ak_val){0};
}

static ak_val t_trays(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.trays");
	ak_uitest_settle(app);
	ak_buf b = {0};
	ak_fake_trays(app->f, &b);
	return str_of(&b);
}

static ak_val t_click_tray(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.click_tray");
	ak_uitest_settle(app);
	ak_buf err = {0};
	if (!ak_fake_click_tray(app->f, span_of(a[1]), &err)) throw_buf(&err);
	else ak_buf_free(&err), ak_uitest_settle(app);
	return (ak_val){0};
}

static ak_val t_choose_tray_item(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.choose_tray_item");
	ak_uitest_settle(app);
	ak_buf err = {0};
	if (!ak_fake_choose_tray_item(app->f, span_of(a[1]), n > 2 ? span_of(a[2]) : sp(""), &err)) throw_buf(&err);
	else ak_buf_free(&err), ak_uitest_settle(app);
	return (ak_val){0};
}

static ak_val t_dock_menu(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.dock_menu");
	ak_uitest_settle(app);
	ak_buf b = {0};
	ak_fake_dock_menu(app->f, &b);
	return str_of(&b);
}

static ak_val t_request_quit(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.request_quit");
	ak_uitest_settle(app);
	bool canceled = ak_ui_quit_request(n > 1 && a[1].i);
	ak_uitest_settle(app);
	return (ak_val){.i = !canceled};
}

static ak_val t_quits(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.quits");
	ak_uitest_settle(app);
	ak_buf b = {0};
	ak_fake_quits(app->f, &b);
	return str_of(&b);
}

static ak_val t_second_instance(ak_val *a, uint32_t n) {
	ak_uitest *app = app_arg(a, n, "test.second_instance");
	ak_uitest_settle(app);
	const ak_arr *args = n > 1 ? a[1].p : NULL;
	ak_buf b = {0};
	ak_buf_u32(&b, args ? args->len : 0);
	for (uint32_t i = 0; args && i < args->len; i++) {
		ak_span s = span_of(args->data[i]);
		ak_buf_str(&b, s.p, s.len);
	}
	ak_span d = n > 2 ? span_of(a[2]) : sp("");
	ak_buf_str(&b, d.p, d.len);
	ak_fake_view *r = ak_fake_root(app->f);
	ak_fake_dispatch(app->f, &(ak_event){.id = r ? r->id : 0, .kind = AK_EVENT_SECOND_INSTANCE, .s = {(const char *)b.data, (uint32_t)b.len}});
	ak_buf_free(&b);
	ak_uitest_settle(app);
	return (ak_val){0};
}

void ak_uitest_builtins_init(void) {
	ak_builtin_register("test.long_press", t_long_press);
	ak_builtin_register("test.disabled", t_disabled);
	ak_builtin_register("test.has", t_has);
	ak_builtin_register("test.submit", t_submit);
	ak_builtin_register("test.toggle", t_toggle);
	ak_builtin_register("test.slide", t_slide);
	ak_builtin_register("test.select", t_select);
	ak_builtin_register("test.pick_color", t_pick_color);
	ak_builtin_register("test.pick_date", t_pick_date);
	ak_builtin_register("test.swipe", t_swipe);
	ak_builtin_register("test.back", t_back);
	ak_builtin_register("test.dismiss", t_dismiss);
	ak_builtin_register("test.refresh", t_refresh);
	ak_builtin_register("test.scroll_to", t_scroll_to);
	ak_builtin_register("test.texts", t_texts);
	ak_builtin_register("test.until", t_until);
	ak_builtin_register("test.wait_for", t_wait_for);
	ak_builtin_register("test.wait_gone", t_wait_gone);
	ak_builtin_register("test.href", t_href);
	ak_builtin_register("test.history", t_history);
	ak_builtin_register("test.go_back", t_go_back);
	ak_builtin_register("test.go_forward", t_go_forward);
	ak_builtin_register("test.in", t_in);
	ak_builtin_register("test.next_to", t_next_to);
	ak_builtin_register("test.mount_app", t_mount_app);
	ak_builtin_register("test.windows", t_windows);
	ak_builtin_register("test.close_window", t_close_window);
	ak_builtin_register("test.window_actions", t_window_actions);
	ak_builtin_register("test.set_window_state", t_set_window_state);
	ak_builtin_register("test.focus_window", t_focus_window);
	ak_builtin_register("test.press_key", t_press_key);
	ak_builtin_register("test.focused", t_focused);
	ak_builtin_register("test.tab", t_tab);
	ak_builtin_register("test.menu_bar", t_menu_bar);
	ak_builtin_register("test.choose_menu_item", t_choose_menu_item);
	ak_builtin_register("test.trays", t_trays);
	ak_builtin_register("test.click_tray", t_click_tray);
	ak_builtin_register("test.choose_tray_item", t_choose_tray_item);
	ak_builtin_register("test.dock_menu", t_dock_menu);
	ak_builtin_register("test.request_quit", t_request_quit);
	ak_builtin_register("test.quits", t_quits);
	ak_builtin_register("test.second_instance", t_second_instance);
	ak_builtin_register("test.ops", t_ops);
	ak_builtin_register("test.press", t_press);
	ak_builtin_register("test.hover", t_hover);
	ak_builtin_register("test.frame", t_frame);
	ak_builtin_register("test.open_url", t_open_url);
	ak_builtin_register("test.link", t_link);
	ak_builtin_register("test.lifecycle", t_lifecycle);
	ak_builtin_register("test.share", t_share);
	ak_builtin_register("test.emit", t_emit);
	ak_builtin_register("test.mount", t_mount);
	ak_builtin_register("test.tap", t_tap);
	ak_builtin_register("test.type", t_type);
	ak_builtin_register("test.expect", t_expect);
	ak_builtin_register("test.expect_no", t_expect_no);
	ak_builtin_register("test.idle", t_idle);
	ak_builtin_register("test.dump", t_dump);
	ak_builtin_register("test.frame_of", t_frame_of);
	ak_builtin_register("test.set_languages", t_set_languages);
}
