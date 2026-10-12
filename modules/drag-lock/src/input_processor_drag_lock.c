/*
 * Input processor: clean button state + software drag lock for the primary
 * button (INPUT_BTN_TOUCH) of a tap-capable touchpad such as the Cirque
 * Pinnacle in relative mode.
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

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_DRAG_LOCK_TRACE)
#define TRACE(...) LOG_INF(__VA_ARGS__)
#else
#define TRACE(...) LOG_DBG(__VA_ARGS__)
#endif

enum dl_state {
    DL_IDLE,      /* button up */
    DL_PRESSED,   /* chip reports the button down (tap or tap-and-drag) */
    DL_SOFT_DRAG, /* chip released, finger back on the pad: we hold the button */
    DL_LOCKED,    /* finger lifted during a drag: holding the button until timeout */
};

static const char *const dl_state_names[] = {"IDLE", "PRESSED", "SOFT_DRAG", "LOCKED"};

struct dl_config {
    uint32_t timeout_ms;
    uint32_t soft_drag_idle_ms;
};

struct dl_data {
    const struct device *src_dev; /* device whose events we see (for injection) */
    struct k_work_delayable release_work;
    enum dl_state state;
    bool pkt_moved;   /* current packet carried non-zero X/Y */
    bool drag_moved;  /* movement happened since the press */
    bool release_due; /* timer fired, synthetic release in flight */
};

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
        LOG_ERR("drag_lock: failed to inject release (%d)", ret);
    }
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

    if (event->type == INPUT_EV_REL) {
        if ((event->code == INPUT_REL_X || event->code == INPUT_REL_Y) && event->value != 0) {
            data->pkt_moved = true;
        }
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (event->type != INPUT_EV_KEY || event->code != INPUT_BTN_TOUCH) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    data->src_dev = event->dev;

    const bool pressed = event->value > 0;
    const bool moved = data->pkt_moved;
    data->pkt_moved = false;

    const enum dl_state before = data->state;
    bool forward = false;

    switch (data->state) {
    case DL_IDLE:
        if (pressed) {
            data->state = DL_PRESSED;
            data->drag_moved = moved;
            forward = true; /* real press */
        }
        data->release_due = false;
        break;

    case DL_PRESSED:
        if (!data->drag_moved) {
            /* So far a plain tap. */
            if (pressed) {
                data->drag_moved = moved;
            } else {
                /* Tap release: forward right away so clicks stay quick. */
                data->state = DL_IDLE;
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
        } else if (!pressed && data->release_due) {
            /* Timeout: the synthetic (or an equivalent real) release. */
            data->release_due = false;
            data->state = DL_IDLE;
            forward = true;
        }
        /* Other motion-less packets (e.g. taps) keep the lock. */
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
        } else if (!pressed && data->release_due) {
            /* Inactivity timeout: the synthetic release. */
            data->release_due = false;
            data->state = DL_IDLE;
            forward = true;
        } else {
            /* Motion-less packet (e.g. a tap): treat as a lift, short grace. */
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

    if (!forward) {
        neutralise(event);
    }
    return ZMK_INPUT_PROC_CONTINUE;
}

static int dl_init(const struct device *dev) {
    struct dl_data *data = dev->data;

    k_work_init_delayable(&data->release_work, dl_release_timeout);
    data->state = DL_IDLE;
    return 0;
}

static const struct zmk_input_processor_driver_api dl_driver_api = {
    .handle_event = dl_handle_event,
};

#define DL_INST(n)                                                                                 \
    static const struct dl_config dl_config_##n = {                                                \
        .timeout_ms = DT_INST_PROP(n, timeout_ms),                                                 \
        .soft_drag_idle_ms = DT_INST_PROP(n, soft_drag_idle_ms),                                   \
    };                                                                                             \
    static struct dl_data dl_data_##n = {};                                                        \
    DEVICE_DT_INST_DEFINE(n, dl_init, NULL, &dl_data_##n, &dl_config_##n, POST_KERNEL,             \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &dl_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DL_INST)
