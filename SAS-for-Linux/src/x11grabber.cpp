#include "x11grabber.h"

#if defined(SAS_HAS_XCB)

#include <QGuiApplication>
#include <QSocketNotifier>
#include <QWidget>

#include <xcb/xcb.h>

#include <cstdlib>

#define SAS_DEBUG_FILE "/tmp/sas-debug.log"

namespace {

// XK_Delete = 0xffff, XK_KP_Delete = 0xff9f（避免引入 Xlib 头文件）
const xcb_keysym_t kSymDelete   = 0xffff;
const xcb_keysym_t kSymKpDelete = 0xff9f;

} // namespace

class X11Grabber::Private {
public:
    xcb_connection_t *conn = nullptr;
    QSocketNotifier *notifier = nullptr;
    xcb_keycode_t kcDel = 0;   // Delete 键的 keycode
    xcb_keycode_t kcKp = 0;    // 小键盘 Delete

    void refreshKeycodes()
    {
        if (!conn)
            return;
        const xcb_setup_t *setup = xcb_get_setup(conn);
        const int count = setup->max_keycode - setup->min_keycode + 1;
        xcb_get_keyboard_mapping_cookie_t ck =
            xcb_get_keyboard_mapping(conn, setup->min_keycode, count);
        xcb_get_keyboard_mapping_reply_t *rep =
            xcb_get_keyboard_mapping_reply(conn, ck, nullptr);
        if (!rep)
            return;
        const xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(rep);
        const int per = rep->keysyms_per_keycode;
        kcDel = kcKp = 0;
        for (int i = 0; i < count; ++i) {
            for (int j = 0; j < per; ++j) {
                const xcb_keysym_t s = syms[i * per + j];
                if (s == kSymDelete)
                    kcDel = setup->min_keycode + i;
                else if (s == kSymKpDelete)
                    kcKp = setup->min_keycode + i;
            }
        }
        free(rep);
    }
};

X11Grabber::X11Grabber(QObject *parent) : QObject(parent), d(new Private) {}

X11Grabber::~X11Grabber()
{
    if (d->notifier)
        delete d->notifier;
    if (d->conn) {
        xcb_disconnect(d->conn);
    }
    delete d;
}

bool X11Grabber::x11Available() const
{
    return QGuiApplication::platformName() == QLatin1String("xcb");
}

bool X11Grabber::start()
{
    if (d->conn)
        return true;

    d->conn = xcb_connect(nullptr, nullptr);
    if (!d->conn || xcb_connection_has_error(d->conn)) {
        if (d->conn)
            xcb_disconnect(d->conn);
        d->conn = nullptr;
        return false;
    }

    d->refreshKeycodes();
    const xcb_setup_t *setup = xcb_get_setup(d->conn);
    const xcb_window_t root = xcb_setup_roots_iterator(setup).data->root;

    const uint16_t masks[] = {
        XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1,
        XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_LOCK,       // + CapsLock
        XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_2,          // + NumLock
        XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_LOCK
            | XCB_MOD_MASK_2,
        XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_5,          // + ISO_Level3
    };

    int grabbed = 0;
    const xcb_keycode_t keycodes[] = { d->kcDel, d->kcKp };
    for (xcb_keycode_t kc : keycodes) {
        if (!kc)
            continue;
        for (uint16_t m : masks) {
            xcb_void_cookie_t ck = xcb_grab_key(d->conn, false, root, m, kc,
                                                XCB_GRAB_MODE_ASYNC,
                                                XCB_GRAB_MODE_ASYNC);
            xcb_generic_error_t *err = xcb_request_check(d->conn, ck);
            if (err)
                free(err);
            else
                ++grabbed;
        }
    }
    xcb_flush(d->conn);

    if (!grabbed) {
        xcb_disconnect(d->conn);
        d->conn = nullptr;
        return false;
    }

    d->notifier = new QSocketNotifier(xcb_get_file_descriptor(d->conn),
                                      QSocketNotifier::Read, this);
    connect(d->notifier, &QSocketNotifier::activated,
            this, &X11Grabber::poll);
    return true;
}

void X11Grabber::poll()
{
    if (!d->conn)
        return;
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(d->conn))) {
        const uint8_t type = ev->response_type & ~0x80;
        if (type == XCB_KEY_PRESS) {
            const auto *ke = reinterpret_cast<const xcb_key_press_event_t *>(ev);
            const uint16_t mods = ke->state & (XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1);
            if (mods == (XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1)
                && (ke->detail == d->kcDel || (d->kcKp && ke->detail == d->kcKp)))
                emit activated();
        } else if (type == XCB_MAPPING_NOTIFY) {
            d->refreshKeycodes();
        } else if (type == XCB_FOCUS_IN) {
        } else if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE) {
            const auto *ke = reinterpret_cast<const xcb_key_press_event_t *>(ev);
        }
        free(ev);
    }
}

#else // 未编译 XCB 支持时的空实现

class X11Grabber::Private {
public:
    bool dummy = false;
};

X11Grabber::X11Grabber(QObject *parent) : QObject(parent), d(new Private) {}
X11Grabber::~X11Grabber() { delete d; }

bool X11Grabber::x11Available() const { return false; }
bool X11Grabber::start() { return false; }
void X11Grabber::poll() {}

#endif