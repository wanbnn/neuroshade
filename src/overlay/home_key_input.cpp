#include "overlay/home_key_input.hpp"

#if defined(NEUROSHADE_HAVE_XCB_HOTKEY)
#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#endif

namespace neuroshade::overlay {

struct HomeKeyInput::Impl {
#if defined(NEUROSHADE_HAVE_XCB_HOTKEY)
    xcb_connection_t* connection{};
    xcb_key_symbols_t* symbols{};
    xcb_window_t root{};
    xcb_keycode_t home{};
    bool pressed{};
    bool grabbed{};
    xcb_timestamp_t last_toggle{};
    xcb_atom_t test_atom{XCB_ATOM_NONE};

    Impl() {
        int screen_index = 0;
        connection = xcb_connect(nullptr, &screen_index);
        if (connection == nullptr || xcb_connection_has_error(connection) != 0) return;

        auto iterator = xcb_setup_roots_iterator(xcb_get_setup(connection));
        for (int index = 0; index < screen_index && iterator.rem != 0; ++index) {
            xcb_screen_next(&iterator);
        }
        if (iterator.rem == 0) return;
        root = iterator.data->root;

        symbols = xcb_key_symbols_alloc(connection);
        if (symbols == nullptr) return;
        xcb_keycode_t* codes = xcb_key_symbols_get_keycode(symbols, XK_Home);
        if (codes == nullptr || codes[0] == XCB_NO_SYMBOL) {
            std::free(codes);
            return;
        }
        home = codes[0];
        std::free(codes);

        const auto cookie = xcb_grab_key_checked(
            connection, 0, root, XCB_MOD_MASK_ANY, home,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_generic_error_t* error = xcb_request_check(connection, cookie);
        grabbed = error == nullptr;
        std::free(error);
        if (const char* atom_name = std::getenv("NEUROSHADE_TEST_HOME_ATOM");
            atom_name != nullptr && atom_name[0] != '\0') {
            const auto atom_cookie = xcb_intern_atom(
                connection, 0, static_cast<std::uint16_t>(std::strlen(atom_name)), atom_name);
            xcb_intern_atom_reply_t* reply =
                xcb_intern_atom_reply(connection, atom_cookie, nullptr);
            if (reply != nullptr) {
                test_atom = reply->atom;
                std::free(reply);
                const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
                xcb_change_window_attributes(connection, root, XCB_CW_EVENT_MASK, &mask);
            }
        }
        xcb_flush(connection);
    }
    ~Impl() {
        if (connection != nullptr) {
            if (grabbed) {
                xcb_ungrab_key(connection, home, root, XCB_MOD_MASK_ANY);
                xcb_flush(connection);
            }
            if (symbols != nullptr) xcb_key_symbols_free(symbols);
            xcb_disconnect(connection);
        }
    }
#endif
};

HomeKeyInput::HomeKeyInput() : impl_(std::make_unique<Impl>()) {}
HomeKeyInput::~HomeKeyInput() = default;

bool HomeKeyInput::available() const noexcept {
#if defined(NEUROSHADE_HAVE_XCB_HOTKEY)
    return impl_->connection != nullptr &&
           ((impl_->grabbed && impl_->home != 0) || impl_->test_atom != XCB_ATOM_NONE);
#else
    return false;
#endif
}

bool HomeKeyInput::pressed_edge() noexcept {
#if defined(NEUROSHADE_HAVE_XCB_HOTKEY)
    if (!available()) return false;
    bool edge = false;
    while (xcb_generic_event_t* event = xcb_poll_for_event(impl_->connection)) {
        const auto type = static_cast<std::uint8_t>(event->response_type & ~0x80U);
        if (type == XCB_KEY_PRESS) {
            const auto* key = reinterpret_cast<xcb_key_press_event_t*>(event);
            const auto elapsed = static_cast<xcb_timestamp_t>(key->time - impl_->last_toggle);
            if (key->detail == impl_->home && !impl_->pressed &&
                (impl_->last_toggle == 0 || elapsed >= 750)) {
                impl_->pressed = true;
                impl_->last_toggle = key->time;
                edge = true;
            }
        } else if (type == XCB_KEY_RELEASE) {
            const auto* key = reinterpret_cast<xcb_key_release_event_t*>(event);
            if (key->detail == impl_->home) impl_->pressed = false;
        } else if (type == XCB_PROPERTY_NOTIFY) {
            const auto* property = reinterpret_cast<xcb_property_notify_event_t*>(event);
            if (property->atom == impl_->test_atom) edge = true;
        }
        std::free(event);
    }
    return edge;
#else
    return false;
#endif
}

}  // namespace neuroshade::overlay
