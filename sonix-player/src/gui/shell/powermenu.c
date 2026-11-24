#include "powermenu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/browser.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/device/clock.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/remote/dlna.h"
#include "src/system/streaming/radio.h"

// ---------------------------------------------------------------------------
// The power menu, iOS-style: two slide-to-confirm pills over a near-opaque
// black veil, and a round cancel button at the bottom that turns red under the
// finger. Sliding is the point: powering off a music player in a pocket must
// take a deliberate gesture, not a stray tap.
// ---------------------------------------------------------------------------

#define SLIDE_WIDTH 380
#define SLIDE_HEIGHT 76
#define SLIDE_KNOB 64
// How far along (as a fraction of the track) the knob has to be on release.
#define SLIDE_COMMIT_PCT 88

// Decorative slide-to-confirm tutorial.
#define GHOST_DURATION_MS 1500
#define GHOST_REBOOT_DELAY_MS 750
#define GHOST_CYCLE_MS 4000
#define GHOST_TIMER_MS 16
#define GHOST_MAX_OPA 191

#define SLIDE_LABEL_LEFT 90
#define SLIDE_LABEL_RIGHT 20

// Horizontal inset of the knob's travel: LVGL runs the knob's CENTRE from
// edge to edge, which hangs half of it outside the pill at both ends. Padding
// the track by half a knob (plus a hair of margin) keeps it inside.
#define SLIDE_INSET (SLIDE_KNOB / 2 + 6)

#define CANCEL_SIZE 76

static lv_obj_t *panel;

typedef struct {
	lv_obj_t *pill; // the visible track
	lv_obj_t *slider;
	lv_obj_t *ghost_knob;
        lv_obj_t *knob_icon;
	int knob_icon_w; // real bitmap width: the two action icons are 56 px
	lv_obj_t *label;
	void (*action)(void);
} slide_t;

static slide_t slide_off;
static slide_t slide_reboot;

static lv_timer_t *ghost_timer = NULL;
static uint32_t ghost_start_tick = 0;

bool powermenu_is_open(void) { return panel && !lv_obj_is_hidden(panel); }

// ---------------------------------------------------------------------------
// opening and closing
// ---------------------------------------------------------------------------

static void slide_reset(slide_t *s);

static void ghost_hide(slide_t *s) {
    if (s->ghost_knob &&
        !lv_obj_is_hidden(s->ghost_knob)) {
        lv_obj_set_hidden(s->ghost_knob, true);
    }
}

static void ghost_draw(slide_t *s, int32_t elapsed) {
    if (!s->ghost_knob) {
        return;
    }

    if (elapsed < 0 || elapsed >= GHOST_DURATION_MS) {
        ghost_hide(s);
        return;
    }

    int32_t travel = SLIDE_WIDTH - SLIDE_HEIGHT;

    int32_t center = SLIDE_HEIGHT / 2
        + travel * elapsed / GHOST_DURATION_MS;

    // Quadratic fade, matching the approved preview.
    int32_t duration_sq =
        GHOST_DURATION_MS * GHOST_DURATION_MS;

    int32_t elapsed_sq = elapsed * elapsed;

    int32_t opacity = GHOST_MAX_OPA
        * (duration_sq - elapsed_sq) / duration_sq;

    lv_obj_align(
        s->ghost_knob,
        LV_ALIGN_LEFT_MID,
        center - SLIDE_KNOB / 2,
        0
    );

    lv_obj_set_style_opa_layered(
        s->ghost_knob,
        (lv_opa_t)opacity,
        0
    );

    lv_obj_set_hidden(s->ghost_knob, false);
}

static void ghost_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (!powermenu_is_open()) {
        return;
    }

    int32_t phase =
        lv_tick_elaps(ghost_start_tick) % GHOST_CYCLE_MS;

    ghost_draw(&slide_off, phase);

    ghost_draw(
        &slide_reboot,
        phase - GHOST_REBOOT_DELAY_MS
    );
}

static void ghost_stop(void) {
    if (ghost_timer) {
        lv_timer_delete(ghost_timer);
        ghost_timer = NULL;
    }

    ghost_hide(&slide_off);
    ghost_hide(&slide_reboot);
}

static void ghost_start(void) {
    ghost_stop();

    if (!powermenu_is_open()) {
        return;
    }

    ghost_start_tick = lv_tick_get();

    ghost_timer = lv_timer_create(
        ghost_timer_cb,
        GHOST_TIMER_MS,
        NULL
    );
}

static void ghost_panel_delete_cb(lv_event_t *e) {
    (void)e;

    if (ghost_timer) {
        lv_timer_delete(ghost_timer);
        ghost_timer = NULL;
    }

    slide_off.ghost_knob = NULL;
    slide_reboot.ghost_knob = NULL;
    panel = NULL;
}

void powermenu_show(void) {
	if (!panel) {
		return;
	}

	slide_reset(&slide_off);
	slide_reset(&slide_reboot);

	lv_obj_set_hidden(panel, false);
	lv_obj_move_foreground(panel);
	power_hold_screen_on(true);
        ghost_start();
}

static void hide(void) {
        ghost_stop();
	if (panel) {
		lv_obj_set_hidden(panel, true);
	}
	power_hold_screen_on(false);
	power_notify_activity();
}

static void async_show_cb(void *unused) {
	(void)unused;
	powermenu_show();
}

void powermenu_request_show(void) { gui_post(async_show_cb, NULL); }

// Both actions flush first: the settings file and the thumbnail cache are
// written lazily, and a DAP that loses them on every shutdown would be worse
// than one that takes an extra moment to stop.
static void do_power_off(void) {
#ifdef HOST_BUILD
        printf("[HOST TEST] Power off confirmed.\\n");
        hide();
        return;
#endif

	printf("power: shutting down\n");
	power_shutdown();
}

static void do_reboot(void) {
#ifdef HOST_BUILD
        printf("[HOST TEST] Restart confirmed.\\n");
        hide();
        return;
#endif

	printf("power: rebooting\n");
	device_state_remember_flush();
	browser_state_flush();
	clock_shutdown();
	radio_store_close();
	qobuzcache_clear_on_exit();
	tidalcache_clear_on_exit();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();
	power_screen_off(); // same instant-feedback trick as the shutdown
	storage_release_for_shutdown();
	sync();

	// Through init, the same way down as power_shutdown(). `reboot(RB_AUTOBOOT)`
	// restarts the machine from inside this process: init's shutdown hooks never
	// run, so the daemons are not stopped and nothing is remounted read-only or
	// unmounted, apart from the card, which is released above either way.
	//
	// The syscall stays as the last resort, on the same eight-second guard, for
	// a firmware whose init never gets there.
	int rc = system("reboot");
	(void)rc;
	sleep(8);
	reboot(RB_AUTOBOOT);
	execl("/bin/sh", "sh", "-c", "reboot", (char *)NULL);
}

static void cancel_cb(lv_event_t *e) {
	(void)e;
	hide();
}

// ---------------------------------------------------------------------------
// the slide-to-confirm pills
// ---------------------------------------------------------------------------

// Keeps the knob icon riding on the knob: the slider's knob is drawn by LVGL
// at a position derived from the value, so the icon just mirrors that maths.
static void slide_track_knob(slide_t *s) {
	int value = lv_slider_get_value(s->slider);

	// The slider is SLIDE_HEIGHT narrower than the pill and centred in it, so
	// the knob's centre -- which LVGL runs from slider edge to slider edge --
	// travels [SLIDE_HEIGHT/2, SLIDE_WIDTH - SLIDE_HEIGHT/2] in pill
	// coordinates: always fully inside. The icon mirrors that in the pill.
	int travel = SLIDE_WIDTH - SLIDE_HEIGHT;
	int knob_center = SLIDE_HEIGHT / 2 + (travel * value) / 100;
	lv_obj_align(s->knob_icon, LV_ALIGN_LEFT_MID, knob_center - s->knob_icon_w / 2, 0);

	// The hint fades as the knob approaches, exactly like the real thing.
	lv_obj_set_style_opa(s->label, (lv_opa_t)(LV_OPA_COVER - (LV_OPA_COVER * value) / 100), 0);
}

static void slide_reset(slide_t *s) {
	lv_slider_set_value(s->slider, 0, LV_ANIM_OFF);
	slide_track_knob(s);
}

static void slide_event_cb(lv_event_t *e) {
	slide_t *s = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
                ghost_stop();
                return;
        }

        if (code == LV_EVENT_VALUE_CHANGED) {
		slide_track_knob(s);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		if (lv_slider_get_value(s->slider) >= SLIDE_COMMIT_PCT) {
			s->action();
		} else {
			slide_reset(s); // not far enough: snap home
                        ghost_start();
		}
	}
}

// One pill: a rounded container carrying the hint text, with a transparent
// slider inside it that is half a knob narrower on each side -- LVGL runs the
// knob's centre from slider edge to slider edge, so insetting the slider is
// what keeps the white disc inside the pill at both extremes.
static void make_slide(lv_obj_t *parent, slide_t *s, const lv_image_dsc_t *icon, lv_color_t icon_tint,
					   const char *text, void (*action)(void)) {
	s->action = action;

	// The visible pill: translucent white over the blur, like iOS.
	s->pill = lv_obj_create(parent);
	lv_obj_set_size(s->pill, SLIDE_WIDTH, SLIDE_HEIGHT);
	lv_obj_set_style_radius(s->pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(s->pill, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(s->pill, LV_OPA_20, 0);
	lv_obj_set_style_border_width(s->pill, 0, 0);
	lv_obj_set_style_pad_all(s->pill, 0, 0);
	lv_obj_set_scrollable(s->pill, false);

	// The instruction, centred on the pill; it fades as the knob advances.
	s->label = lv_label_create(s->pill);
	lv_label_set_text(s->label, tr(text));
	lv_obj_set_style_text_font(s->label, &font_ui_24, 0);
	lv_obj_set_style_text_color(s->label, lv_color_white(), 0);
	lv_obj_set_width(
                s->label,
                SLIDE_WIDTH - SLIDE_LABEL_LEFT
                    - SLIDE_LABEL_RIGHT
        );

        lv_label_set_long_mode(
                s->label,
                LV_LABEL_LONG_DOT
        );

        lv_obj_set_style_text_align(
                s->label,
                LV_TEXT_ALIGN_LEFT,
                0
        );

        lv_obj_align(
                s->label,
                LV_ALIGN_LEFT_MID,
                SLIDE_LABEL_LEFT,
                0
        );

	// The working part: invisible track, white disc knob.
	s->slider = lv_slider_create(s->pill);
	lv_obj_set_size(s->slider, SLIDE_WIDTH - SLIDE_HEIGHT, SLIDE_HEIGHT);
	lv_obj_center(s->slider);
	lv_slider_set_range(s->slider, 0, 100);
	lv_slider_set_value(s->slider, 0, LV_ANIM_OFF);
	// Zero padding, or the theme's default insets shift the knob's travel and
	// the icon riding it ends up off the disc's centre.
	lv_obj_set_style_pad_all(s->slider, 0, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(s->slider, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(s->slider, LV_OPA_TRANSP, LV_PART_INDICATOR);

	lv_obj_set_style_radius(s->slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(s->slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_bg_opa(s->slider, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_pad_all(s->slider, (SLIDE_KNOB - SLIDE_HEIGHT) / 2, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(s->slider, 10, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(s->slider, LV_OPA_40, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(s->slider, lv_color_black(), LV_PART_KNOB);

	// Drag only, never tap. A plain LVGL slider jumps its value to wherever the
	// finger presses, so one tap near the far end of the track would pass the
	// commit threshold -- exactly the careless gesture the pill exists to
	// prevent. With ADV_HITTEST the slider only answers a press that begins on
	// the knob, so the value can only be dragged to the end. A tap on the rest
	// of the pill does nothing.
	lv_obj_set_adv_hittest(s->slider, true);
	// The knob stays easy to grab: the extended area applies to its hit-test,
	// not to the track.
	lv_obj_set_ext_click_area(s->slider, SLIDE_HEIGHT / 2);

	lv_obj_add_event_cb(s->slider, slide_event_cb, LV_EVENT_PRESSED, s);
        lv_obj_add_event_cb(s->slider, slide_event_cb, LV_EVENT_VALUE_CHANGED, s);
	lv_obj_add_event_cb(s->slider, slide_event_cb, LV_EVENT_RELEASED, s);
	lv_obj_add_event_cb(s->slider, slide_event_cb, LV_EVENT_PRESS_LOST, s);

	// The icon rides the knob, drawn in the pill's coordinate space (the knob
	// itself cannot hold children).
	s->knob_icon = lv_image_create(s->pill);
	lv_image_set_src(s->knob_icon, icon);
	// A touch smaller than the raw 56 px bitmap, so the disc keeps a clear
	// rim around the glyph. Scaling happens about the centre, so the
	// centring maths below is unaffected.
	lv_image_set_scale(s->knob_icon, 200); // 200/256 = ~44 px
	s->knob_icon_w = (int)icon->header.w;
	lv_obj_set_style_image_recolor(s->knob_icon, icon_tint, 0);
	lv_obj_set_style_image_recolor_opa(s->knob_icon, LV_OPA_COVER, 0);
	lv_obj_set_ignore_layout(s->knob_icon, true);

        // A visual-only copy. Never change the real slider.
        s->ghost_knob = lv_obj_create(s->pill);

        lv_obj_remove_style_all(s->ghost_knob);

        lv_obj_set_size(
                s->ghost_knob,
                SLIDE_KNOB,
                SLIDE_KNOB
        );

        lv_obj_set_style_radius(
                s->ghost_knob,
                LV_RADIUS_CIRCLE,
                0
        );

        lv_obj_set_style_bg_color(
                s->ghost_knob,
                lv_color_white(),
                0
        );

        lv_obj_set_style_bg_opa(
                s->ghost_knob,
                56,
                0
        );

        // Translucent glass outline.
        lv_obj_set_style_border_width(
                s->ghost_knob, 2, 0
        );

        lv_obj_set_style_border_color(
                s->ghost_knob, lv_color_white(), 0
        );

        lv_obj_set_style_border_opa(
                s->ghost_knob, 166, 0
        );

        lv_obj_set_style_pad_all(
                s->ghost_knob, 0, 0
        );

        lv_obj_set_clickable(s->ghost_knob, false);
        lv_obj_set_scrollable(s->ghost_knob, false);
        lv_obj_set_ignore_layout(s->ghost_knob, true);

        lv_obj_t *ghost_icon =
                lv_image_create(s->ghost_knob);

        lv_image_set_src(ghost_icon, icon);
        lv_image_set_scale(ghost_icon, 200);

        lv_obj_set_style_image_recolor(
                ghost_icon,
                lv_color_hex(0xE4E4E4),
                0
        );

        lv_obj_set_style_image_recolor_opa(
                ghost_icon,
                LV_OPA_COVER,
                0
        );

        lv_obj_set_clickable(ghost_icon, false);
        lv_obj_set_scrollable(ghost_icon, false);
        lv_obj_center(ghost_icon);

        lv_obj_align(
                s->ghost_knob,
                LV_ALIGN_LEFT_MID,
                (SLIDE_HEIGHT - SLIDE_KNOB) / 2,
                0
        );

        lv_obj_set_style_opa_layered(
                s->ghost_knob,
                GHOST_MAX_OPA,
                0
        );

        lv_obj_set_hidden(s->ghost_knob, true);

	slide_track_knob(s);
}

void powermenu_init(gui_config_t *cfg) {
	panel = lv_obj_create(lv_layer_top());

        lv_obj_add_event_cb(
                panel,
                ghost_panel_delete_cb,
                LV_EVENT_DELETE,
                NULL
        );
	lv_obj_set_size(panel, cfg->screen_width, cfg->screen_height);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
	// Nearly-opaque black: the page underneath is still just visible, but the
	// menu clearly owns the screen.
	lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
	lv_obj_set_style_border_width(panel, 0, 0);
	lv_obj_set_style_radius(panel, 0, 0);
	lv_obj_set_style_pad_all(panel, 0, 0);
	lv_obj_set_scrollable(panel, false);
	lv_obj_set_hidden(panel, true);

	// The two pills, upper third of the screen, like the real thing.
	lv_obj_t *pills = lv_obj_create(panel);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_align(pills, LV_ALIGN_TOP_MID, 0, cfg->top_bar_height + 70);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 26, 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	make_slide(pills, &slide_off, &icon_power_off, lv_color_make(224, 27, 36), "powermenu_slide_to_power_off", do_power_off);
	// A fixed blue, deliberately not the accent: the two slides are power off
	// (red) and reboot (blue), and an accent set to red or orange would make
	// them impossible to tell apart at a glance.
	make_slide(pills, &slide_reboot, &icon_reboot, lv_color_make(0, 122, 255), "powermenu_slide_to_restart", do_reboot);

	// Cancel: a round button at the bottom centre that goes red under the
	// finger, with its label floating underneath.
	lv_obj_t *cancel = lv_btn_create(panel);
	lv_obj_set_size(cancel, CANCEL_SIZE, CANCEL_SIZE);
	lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -84);
	lv_obj_set_style_radius(cancel, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(cancel, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_30, 0);
	lv_obj_set_style_bg_color(cancel, lv_color_make(224, 27, 36), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_set_style_image_recolor(cancel_icon, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(cancel_icon, LV_OPA_COVER, 0);
	lv_obj_center(cancel_icon);

	lv_obj_t *cancel_label = lv_label_create(panel);
	lv_label_set_text(cancel_label, tr("cancel"));
	lv_obj_set_style_text_font(cancel_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
	lv_obj_align(cancel_label, LV_ALIGN_BOTTOM_MID, 0, -44);

}
