/*
 * Input processor: clean button state + software drag lock + misread-gesture
 * filter for the tap button (INPUT_BTN_TOUCH) of a tap-capable touchpad such
 * as the Cirque Pinnacle in relative mode. Its one parameter is the button
 * code to emit (0 = INPUT_BTN_TOUCH); the code is latched at the press and
 * reused for the release, so a layer (processor chain) change mid-gesture
 * cannot leave ZMK's per-button press count unbalanced. A tap while the
 * button is held ends the drag.
 *
 * Why: Zephyr's cirque,pinnacle driver reports BTN_TOUCH with every packet
 * while ZMK's HID layer reference-counts mouse button presses, so a drag of
 * N packets needed N release packets to end (stuck drags, a tap can't clear
 * it). This processor forwards only real transitions.
 *
 * Drag lock: when a drag (press + movement) ends because the finger was
 * lifted, the release is held back for `timeout-ms`. If the finger comes back
 * and moves within that time, the drag simply continues; otherwise the release
 * is sent. Plain taps (press + release without movement) are forwarded
 * immediately so clicks and double-clicks are not delayed.
 *
 * Accidental clicks and drags: the chip's gesture engine takes a short cursor
 * stroke for a tap (reported ~200-300 ms after the lift), and the next touch
 * then becomes a tap-and-drag with the button held. A chip press that arrives
 * within `tap-after-stroke-ms` after a stroke shorter than `short-stroke-ms`
 * that moved at least `tap-max-stroke` counts is therefore swallowed (no
 * click, no drag, motion passes). Presses within `tap-drag-window-ms` after a
 * forwarded tap are always accepted (double tap, tap-and-hold).
 *
 * Packet model: in relative mode the chip only sends packets when something
 * changed (motion or button); a lifted finger produces silence (no Z-idle
 * packets). While the chip's own tap-drag is active it does report the lift
 * (a packet without motion), so that is used to arm the lock. In the
 * continued, software-held drag (DL_SOFT_DRAG) there is no lift signal at
 * all, so an inactivity timer is used instead: no motion for
 * `soft-drag-idle-ms` releases the button.
 *
 * Events are neutralised (code set to KEY_RESERVED, which the ZMK input
 * listener ignores) instead of returning ZMK_INPUT_PROC_STOP, because the
 * button event carries the packet's sync flag: stopping it would also discard
 * the X/Y movement of the same packet.
 */

#define DT_DRV_COMPAT zmk_input_processor_drag_lock

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>

/* Own log module, so ZMK's level can be lowered (its input split warns on
 * every touchpad packet) while TRACE / STATS lines stay visible. */
LOG_MODULE_REGISTER(drag_lock, LOG_LEVEL_INF);

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_DRAG_LOCK_TRACE)
#define TRACE(...) LOG_INF(__VA_ARGS__)
#else
#define TRACE(...) LOG_DBG(__VA_ARGS__)
#endif

static inline uint32_t dl_abs(int32_t v) { return v < 0 ? (uint32_t)(-v) : (uint32_t)v; }

#define DL_INJECT_MAX_RETRIES 100 /* x 2 ms */

enum dl_state {
    DL_IDLE,      /* button up */
    DL_PRESSED,   /* chip reports the button down, accepted (tap or tap-and-drag) */
    DL_SOFT_DRAG, /* chip released, finger back on the pad: we hold the button */
    DL_LOCKED,    /* finger lifted during a drag: holding the button until timeout */
    DL_SUPPRESS,  /* chip button right after a short cursor stroke: swallowed, motion passes */
};

static const char *const dl_state_names[] = {"IDLE", "PRESSED", "SOFT_DRAG", "LOCKED",
                                             "SUPPRESS"};

struct dl_config {
    uint32_t timeout_ms;
    uint32_t soft_drag_idle_ms;
    uint32_t drag_threshold;     /* motion (sum |dx|+|dy|) while pressed before it is a drag */
    uint32_t tap_drag_window_ms; /* a press this soon after a tap is always accepted */
    uint32_t tap_after_drag_ms;  /* a press this soon after a timed-out drag end is its late tap */
    uint32_t stroke_gap_ms;      /* a pause this long between packets ends a stroke */
    uint32_t tap_after_stroke_ms; /* a press this soon after a stroke may be the chip's misread */
    uint32_t short_stroke_ms;    /* ...if that stroke lasted at most this long */
    uint32_t tap_max_stroke;     /* ...and moved at least this much (0 disables the check) */
};

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_DRAG_LOCK_STATS)
#define DL_STATS_PERIOD_MS 5000
#define DL_STATS_BIG_DELTA 60      /* |dx| or |dy| >= this in one packet is "big" */
#define DL_STATS_STALL_MS 40       /* gap >= this between motion packets is a delivery stall */
#define DL_STATS_STROKE_GAP_MS 150 /* gap >= this between motion packets is a new stroke */
#define DL_STATS_PAUSE_MAX_MS 500  /* ...and below this it is a "pause" (rim loss, link stall) */

struct dl_stats {
    struct k_spinlock lock;
    /* interval counters */
    uint32_t pkts;     /* complete packets (ended by a sync event) */
    uint32_t torn;     /* packets missing X or Y, or with duplicates */
    uint32_t bare;     /* sync events with neither X nor Y (e.g. injected release) */
    uint32_t big;      /* packets with |dx| or |dy| >= DL_STATS_BIG_DELTA */
    uint32_t stalls;   /* gaps in [STALL, STROKE_GAP) between consecutive motion packets */
    uint32_t pauses;   /* gaps in [STROKE_GAP, PAUSE_MAX) between consecutive motion packets */
    uint32_t flips;    /* direction reversals on X or Y inside a stroke */
    uint32_t btn;      /* packets with the button bit set */
    uint32_t max_gap_ms;   /* longest gap between consecutive motion packets of a stroke */
    uint32_t max_pause_ms; /* longest pause */
    uint32_t max_dx, max_dy; /* largest |dx|, |dy| */
    uint32_t sum_dx, sum_dy; /* sum of |dx|, |dy| */
    bool torn_logged, big_logged;
    /* per-packet scratch */
    uint8_t x_seen, y_seen;
    bool btn_seen;
    int32_t pkt_dx, pkt_dy;
    int8_t last_sx, last_sy; /* sign of the last non-zero dx / dy in this stroke */
    int64_t last_motion_ms;  /* last packet that carried motion */
};
#endif

struct dl_data {
    const struct device *src_dev; /* device whose events we see (for injection) */
    struct k_work_delayable release_work;
    enum dl_state state;
    uint16_t out_code;     /* button code of the gesture in progress (latched at the press) */
    uint32_t pkt_motion;   /* sum |dx|+|dy| of the current packet */
    uint32_t press_motion; /* motion accumulated since the press (drag detection) */
    uint32_t stroke_motion; /* motion of the current stroke (packets < stroke_gap_ms apart) */
    int64_t stroke_start_ms; /* when the current stroke started */
    int64_t last_motion_ms; /* when the last packet with motion arrived */
    int64_t last_tap_ms;   /* when the last tap (click) was forwarded */
    int64_t release_ms;    /* when the lock timeout released a drag */
    uint8_t inject_retries; /* failed release injections in a row */
    bool drag_moved;       /* enough movement since the press: it is a drag */
    bool release_due;      /* timer fired, synthetic release in flight */
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_DRAG_LOCK_STATS)
    struct k_work_delayable stats_work;
    struct dl_stats stats;
#endif
};

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_DRAG_LOCK_STATS)
/* Called for every event before the drag-lock logic (events are still raw). */
static void dl_stats_on_event(struct dl_data *data, const struct input_event *ev) {
    struct dl_stats *s = &data->stats;

    if (ev->type == INPUT_EV_REL) {
        if (ev->code == INPUT_REL_X) {
            s->x_seen++;
            s->pkt_dx = ev->value;
        } else if (ev->code == INPUT_REL_Y) {
            s->y_seen++;
            s->pkt_dy = ev->value;
        }
    } else if (ev->type == INPUT_EV_KEY && ev->code == INPUT_BTN_TOUCH && ev->value > 0) {
        s->btn_seen = true;
    }

    if (!ev->sync) {
        return;
    }

    /* End of a packet. */
    bool log_torn = false, log_big = false;
    uint8_t x_seen, y_seen;
    int32_t dx, dy;
    int64_t now = k_uptime_get();

    k_spinlock_key_t key = k_spin_lock(&s->lock);
    x_seen = s->x_seen;
    y_seen = s->y_seen;
    dx = s->pkt_dx;
    dy = s->pkt_dy;

    if (x_seen == 0 && y_seen == 0) {
        s->bare++;
    } else {
        s->pkts++;
        if (s->btn_seen) {
            s->btn++;
        }
        if (x_seen != 1 || y_seen != 1) {
            s->torn++;
            if (!s->torn_logged) {
                s->torn_logged = true;
                log_torn = true;
            }
        }

        uint32_t ax = dl_abs(dx), ay = dl_abs(dy);
        s->sum_dx += ax;
        s->sum_dy += ay;
        if (ax > s->max_dx) {
            s->max_dx = ax;
        }
        if (ay > s->max_dy) {
            s->max_dy = ay;
        }
        if (ax >= DL_STATS_BIG_DELTA || ay >= DL_STATS_BIG_DELTA) {
            s->big++;
            if (!s->big_logged) {
                s->big_logged = true;
                log_big = true;
            }
        }

        if (ax != 0 || ay != 0) {
            /* Gaps are measured between consecutive packets that both carry
             * motion; button-only packets (taps, lifts) are not stalls. */
            if (s->last_motion_ms != 0) {
                uint32_t gap = (uint32_t)(now - s->last_motion_ms);
                if (gap >= DL_STATS_STROKE_GAP_MS) {
                    /* New stroke: forget the previous direction. */
                    s->last_sx = s->last_sy = 0;
                    if (gap < DL_STATS_PAUSE_MAX_MS) {
                        s->pauses++;
                        if (gap > s->max_pause_ms) {
                            s->max_pause_ms = gap;
                        }
                    }
                } else {
                    if (gap > s->max_gap_ms) {
                        s->max_gap_ms = gap;
                    }
                    if (gap >= DL_STATS_STALL_MS) {
                        s->stalls++;
                    }
                }
            }
            if (dx != 0) {
                int8_t sx = dx > 0 ? 1 : -1;
                if (s->last_sx != 0 && sx != s->last_sx) {
                    s->flips++;
                }
                s->last_sx = sx;
            }
            if (dy != 0) {
                int8_t sy = dy > 0 ? 1 : -1;
                if (s->last_sy != 0 && sy != s->last_sy) {
                    s->flips++;
                }
                s->last_sy = sy;
            }
            s->last_motion_ms = now;
        }
    }

    s->x_seen = s->y_seen = 0;
    s->btn_seen = false;
    s->pkt_dx = s->pkt_dy = 0;
    k_spin_unlock(&s->lock, key);

    if (log_torn) {
        LOG_INF("drag_lock stats: torn packet (x_seen=%u y_seen=%u dx=%d dy=%d)", x_seen, y_seen,
                dx, dy);
    }
    if (log_big) {
        LOG_INF("drag_lock stats: big packet dx=%d dy=%d", dx, dy);
    }
}

static void dl_stats_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct dl_data *data = CONTAINER_OF(dwork, struct dl_data, stats_work);
    struct dl_stats *s = &data->stats;

    k_spinlock_key_t key = k_spin_lock(&s->lock);
    struct dl_stats snap = *s;
    s->pkts = s->torn = s->bare = s->big = s->stalls = s->pauses = s->flips = s->btn = 0;
    s->max_gap_ms = s->max_pause_ms = s->max_dx = s->max_dy = s->sum_dx = s->sum_dy = 0;
    s->torn_logged = s->big_logged = false;
    k_spin_unlock(&s->lock, key);

    if (snap.pkts || snap.bare) {
        LOG_INF("drag_lock stats: pkts=%u torn=%u bare=%u big=%u stalls=%u maxgap=%ums "
                "pauses=%u maxpause=%ums flips=%u btn=%u max|dx|=%u max|dy|=%u sum|dx|=%u "
                "sum|dy|=%u",
                snap.pkts, snap.torn, snap.bare, snap.big, snap.stalls, snap.max_gap_ms,
                snap.pauses, snap.max_pause_ms, snap.flips, snap.btn, snap.max_dx, snap.max_dy,
                snap.sum_dx, snap.sum_dy);
    }

    k_work_reschedule(&data->stats_work, K_MSEC(DL_STATS_PERIOD_MS));
}
#endif /* CONFIG_ZMK_INPUT_PROCESSOR_DRAG_LOCK_STATS */

static void dl_release_timeout(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct dl_data *data = CONTAINER_OF(dwork, struct dl_data, release_work);

    if (data->src_dev == NULL) {
        return;
    }

    data->release_due = true;
    TRACE("drag_lock: timeout, injecting release");
    /* Delivered through the input queue back into this processor. */
    int ret = input_report_key(data->src_dev, INPUT_BTN_TOUCH, 0, true, K_NO_WAIT);
    if (ret < 0) {
        /* Input queue full: retry shortly, the button must not stay held.
         * Bounded, so a wedged input thread cannot log every 2 ms forever. */
        if (data->inject_retries < DL_INJECT_MAX_RETRIES) {
            if (data->inject_retries++ == 0) {
                LOG_WRN("drag_lock: failed to inject release (%d), retrying", ret);
            }
            k_work_reschedule(&data->release_work, K_MSEC(2));
        } else {
            LOG_ERR("drag_lock: giving up injecting the release (%d)", ret);
        }
        return;
    }
    data->inject_retries = 0;
}

static inline void neutralise(struct input_event *event) {
    event->code = INPUT_KEY_RESERVED;
    event->value = 0;
}

static inline void dl_lock(struct dl_data *data, const struct dl_config *cfg) {
    data->state = DL_LOCKED;
    data->release_due = false;
    k_work_reschedule(&data->release_work, K_MSEC(cfg->timeout_ms));
}

/* Button up: no timer may survive into IDLE (it would end a later click early). */
static inline void dl_idle(struct dl_data *data) {
    data->state = DL_IDLE;
    data->release_due = false;
    k_work_cancel_delayable(&data->release_work);
}

static inline void dl_continue_drag(struct dl_data *data, const struct dl_config *cfg,
                                    enum dl_state next) {
    data->release_due = false;
    data->drag_moved = true;
    data->state = next;
    if (next == DL_SOFT_DRAG) {
        /* No lift signal in this state: release after inactivity. */
        k_work_reschedule(&data->release_work, K_MSEC(cfg->soft_drag_idle_ms));
    } else {
        k_work_cancel_delayable(&data->release_work);
    }
}

static int dl_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                           uint32_t param2, struct zmk_input_processor_state *state) {
    const struct dl_config *cfg = dev->config;
    struct dl_data *data = dev->data;

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_DRAG_LOCK_STATS)
    dl_stats_on_event(data, event);
#endif

    if (event->type == INPUT_EV_REL) {
        if (event->code == INPUT_REL_X || event->code == INPUT_REL_Y) {
            data->pkt_motion += dl_abs(event->value);
        }
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (event->type != INPUT_EV_KEY || event->code != INPUT_BTN_TOUCH) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    data->src_dev = event->dev;

    const bool pressed = event->value > 0;
    const uint32_t motion = data->pkt_motion;
    const bool moved = motion > 0;
    const int64_t now = k_uptime_get();
    data->pkt_motion = 0;

    /* The stroke that ended before this packet: the chip reports a short
     * cursor stroke as a tap, and then the next touch as a tap-and-drag. */
    const uint32_t since_motion =
        data->last_motion_ms ? (uint32_t)(now - data->last_motion_ms) : UINT32_MAX;
    const uint32_t prev_stroke_motion = data->stroke_motion;
    const uint32_t prev_stroke_len_ms =
        data->last_motion_ms ? (uint32_t)(data->last_motion_ms - data->stroke_start_ms) : 0;
    if (moved) {
        if (since_motion >= cfg->stroke_gap_ms) {
            data->stroke_motion = 0; /* new stroke */
            data->stroke_start_ms = now;
        }
        data->stroke_motion += motion;
        data->last_motion_ms = now;
    }

    const enum dl_state before = data->state;
    bool forward = false;

    /* A motion-less press while the button is held is the user's tap to end
     * the drag only if no motion is flowing; during motion it is the chip's
     * late tap verdict on a short reposition segment and must not end it. */
    const bool quiet_press = pressed && !moved && since_motion >= cfg->stroke_gap_ms;

    switch (data->state) {
    case DL_IDLE:
        if (pressed) {
            const bool after_tap =
                data->last_tap_ms != 0 && (now - data->last_tap_ms) <= cfg->tap_drag_window_ms;
            const bool after_short_stroke =
                cfg->tap_max_stroke != 0 && since_motion <= cfg->tap_after_stroke_ms &&
                prev_stroke_len_ms <= cfg->short_stroke_ms &&
                prev_stroke_motion >= cfg->tap_max_stroke;
            /* The tap meant to end a drag, reported by the chip after the
             * lock timer had already released it. */
            const bool late_drag_tap = !moved && data->release_ms != 0 &&
                                       (now - data->release_ms) <= cfg->tap_after_drag_ms;

            TRACE("drag_lock: press: last stroke %u counts / %u ms, ended %u ms ago; last tap "
                  "%d ms ago%s%s",
                  prev_stroke_motion, prev_stroke_len_ms,
                  since_motion == UINT32_MAX ? 0 : since_motion,
                  data->last_tap_ms ? (int)(now - data->last_tap_ms) : -1,
                  (!after_tap && after_short_stroke) ? " -> misread stroke, suppressed" : "",
                  (!after_tap && late_drag_tap) ? " -> late drag-ending tap, suppressed" : "");

            data->press_motion = motion;
            if (!after_tap && (after_short_stroke || late_drag_tap)) {
                /* Neither a click nor a drag, let the motion through. */
                data->state = DL_SUPPRESS;
                if (late_drag_tap) {
                    /* Only one press can be the late drag-ending tap, and the
                     * user did tap: a quick follow-up press is a double tap. */
                    data->release_ms = 0;
                    data->last_tap_ms = now;
                }
            } else {
                data->state = DL_PRESSED;
                data->drag_moved = motion >= cfg->drag_threshold;
                data->out_code = param1 ? (uint16_t)param1 : INPUT_BTN_TOUCH;
                forward = true; /* real press: tap, double tap or (double-)tap-and-hold */
            }
        }
        data->release_due = false;
        break;

    case DL_SUPPRESS:
        /* Misread stroke: motion only, until the chip lets the button go. */
        if (!pressed) {
            dl_idle(data);
        }
        break;

    case DL_PRESSED:
        if (!data->drag_moved) {
            /* So far a plain tap (a little finger roll is not a drag). */
            if (pressed) {
                data->press_motion += motion;
                data->drag_moved = data->press_motion >= cfg->drag_threshold;
            } else {
                /* Tap release: forward right away so clicks stay quick. */
                dl_idle(data);
                data->last_tap_ms = now;
                forward = true;
            }
        } else if (moved) {
            /* Dragging, finger down (button bit irrelevant). */
        } else {
            /* Drag has moved before, now a packet without motion: lifted. */
            dl_lock(data, cfg);
        }
        break;

    case DL_LOCKED:
        if (moved) {
            /* Finger is back and moving: the drag continues. */
            dl_continue_drag(data, cfg, pressed ? DL_PRESSED : DL_SOFT_DRAG);
        } else if (quiet_press) {
            /* A tap while the button is held: end the drag now. The tap's
             * press becomes the release; its own release packet is ignored. */
            dl_idle(data);
            data->last_tap_ms = now;
            event->value = 0;
            forward = true;
        } else if (!pressed && data->release_due) {
            /* Timeout: the synthetic (or an equivalent real) release. */
            dl_idle(data);
            data->release_ms = now;
            forward = true;
        }
        /* Other motion-less packets (stale chip verdicts) keep the lock. */
        break;

    case DL_SOFT_DRAG:
        if (moved) {
            if (pressed) {
                /* The chip started its own drag again: it will report the lift. */
                dl_continue_drag(data, cfg, DL_PRESSED);
            } else {
                /* Still dragging: push the inactivity timeout out. */
                data->release_due = false;
                k_work_reschedule(&data->release_work, K_MSEC(cfg->soft_drag_idle_ms));
            }
        } else if (quiet_press) {
            /* A tap while soft-dragging ends the drag right away. */
            dl_idle(data);
            data->last_tap_ms = now;
            event->value = 0;
            forward = true;
        } else if (pressed) {
            /* Stale chip verdict on a short reposition segment: keep dragging. */
        } else if (data->release_due) {
            /* Inactivity timeout: the synthetic release. (No late-tap window
             * here: a drag-ending tap after a silent lift arrives well inside
             * the idle timeout and ends the drag as a quiet press.) */
            dl_idle(data);
            forward = true;
        } else {
            /* Motion-less packet without the button: treat as a lift, short grace. */
            dl_lock(data, cfg);
        }
        break;
    }

    if (before != data->state || forward) {
        TRACE("drag_lock: btn=%d mv=%d %s -> %s%s", pressed, moved, dl_state_names[before],
              dl_state_names[data->state], forward ? " (fwd)" : "");
    } else if (!moved) {
        /* Motion packets are not traced (too chatty); motion-less ones are. */
        TRACE("drag_lock: btn=%d mv=0 %s", pressed, dl_state_names[before]);
    }

    if (forward) {
        /* The release always carries the code the press used, whatever layer
         * (processor chain) is active now: ZMK reference-counts per button. */
        event->code = data->out_code ? data->out_code : INPUT_BTN_TOUCH;
    } else {
        neutralise(event);
    }
    return ZMK_INPUT_PROC_CONTINUE;
}

static int dl_init(const struct device *dev) {
    struct dl_data *data = dev->data;

    k_work_init_delayable(&data->release_work, dl_release_timeout);
    data->state = DL_IDLE;
#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_DRAG_LOCK_STATS)
    k_work_init_delayable(&data->stats_work, dl_stats_work_cb);
    k_work_schedule(&data->stats_work, K_MSEC(DL_STATS_PERIOD_MS));
#endif
    return 0;
}

static const struct zmk_input_processor_driver_api dl_driver_api = {
    .handle_event = dl_handle_event,
};

#define DL_INST(n)                                                                                 \
    static const struct dl_config dl_config_##n = {                                                \
        .timeout_ms = DT_INST_PROP(n, timeout_ms),                                                 \
        .soft_drag_idle_ms = DT_INST_PROP(n, soft_drag_idle_ms),                                   \
        .drag_threshold = DT_INST_PROP(n, drag_threshold),                                         \
        .tap_drag_window_ms = DT_INST_PROP(n, tap_drag_window_ms),                                 \
        .tap_after_drag_ms = DT_INST_PROP(n, tap_after_drag_ms),                                   \
        .stroke_gap_ms = DT_INST_PROP(n, stroke_gap_ms),                                           \
        .tap_after_stroke_ms = DT_INST_PROP(n, tap_after_stroke_ms),                               \
        .short_stroke_ms = DT_INST_PROP(n, short_stroke_ms),                                       \
        .tap_max_stroke = DT_INST_PROP(n, tap_max_stroke),                                         \
    };                                                                                             \
    static struct dl_data dl_data_##n = {};                                                        \
    DEVICE_DT_INST_DEFINE(n, dl_init, NULL, &dl_data_##n, &dl_config_##n, POST_KERNEL,             \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &dl_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DL_INST)
