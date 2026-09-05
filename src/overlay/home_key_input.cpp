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
    bool initialized{};
    bool capturing{};
    bool pointer_grabbed{};
    xcb_window_t window{};
    xcb_cursor_t cursor{};
    int origin_x{}, origin_y{};
    std::vector<UiInput> events;
    const std::uint32_t control_keys[15] = {XK_F1,XK_F2,XK_F3,XK_F4,XK_F5,XK_F6,XK_F7,
        XK_Up,XK_Down,XK_Left,XK_Right,XK_Return,XK_space,XK_Page_Up,XK_Page_Down};

    // Probe devices may never present. Let only a presenting device acquire
    // the global key, otherwise a probe can consume Home without polling it.
    void initialize() {
        if (initialized) return;
        initialized = true;
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
    impl_->initialize();
    if (!available()) return false;
    bool edge = false;
    while (xcb_generic_event_t* event = xcb_poll_for_event(impl_->connection)) {
        const auto type = static_cast<std::uint8_t>(event->response_type & ~0x80U);
        if (type == XCB_KEY_PRESS) {
            const auto* key = reinterpret_cast<xcb_key_press_event_t*>(event);
            const auto elapsed = static_cast<xcb_timestamp_t>(key->time - impl_->last_toggle);
            const auto symbol = xcb_key_symbols_get_keysym(impl_->symbols, key->detail, 0);
            if (impl_->capturing && key->detail != impl_->home)
                impl_->events.push_back({-1,-1,false,symbol});
            if (key->detail == impl_->home && !impl_->pressed &&
                (impl_->last_toggle == 0 || elapsed >= 750)) {
                impl_->pressed = true;
                impl_->last_toggle = key->time;
                edge = true;
            }
        } else if (type == XCB_BUTTON_PRESS && impl_->pointer_grabbed) {
            const auto* button = reinterpret_cast<xcb_button_press_event_t*>(event);
            if (button->detail == 1)
                impl_->events.push_back({button->root_x-impl_->origin_x,button->root_y-impl_->origin_y,true,0});
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

void HomeKeyInput::capture(bool visible) noexcept {
#if defined(NEUROSHADE_HAVE_XCB_HOTKEY)
    impl_->initialize();
    if (!available() || impl_->capturing == visible) return;
    impl_->capturing = visible;
    for (auto symbol : impl_->control_keys) {
        auto* codes = xcb_key_symbols_get_keycode(impl_->symbols, symbol);
        if (!codes) continue;
        for (auto* code = codes; *code; ++code) {
            if (visible) xcb_grab_key(impl_->connection,0,impl_->root,0,*code,XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC);
            else xcb_ungrab_key(impl_->connection,*code,impl_->root,0);
        }
        std::free(codes);
    }
    if (visible) {
        auto* focus = xcb_get_input_focus_reply(impl_->connection,xcb_get_input_focus(impl_->connection),nullptr);
        if (focus && focus->focus > 1 && focus->focus != impl_->root) {
            impl_->window = focus->focus;
            auto* origin = xcb_translate_coordinates_reply(impl_->connection,
                xcb_translate_coordinates(impl_->connection,impl_->window,impl_->root,0,0),nullptr);
            if (origin) { impl_->origin_x=origin->dst_x; impl_->origin_y=origin->dst_y; std::free(origin); }
            if (!impl_->cursor) {
                auto font=xcb_generate_id(impl_->connection);
                xcb_open_font(impl_->connection,font,6,"cursor");
                impl_->cursor=xcb_generate_id(impl_->connection);
                xcb_create_glyph_cursor(impl_->connection,impl_->cursor,font,font,68,69,0,0,0,65535,65535,65535);
                xcb_close_font(impl_->connection,font);
            }
            auto* grab=xcb_grab_pointer_reply(impl_->connection,xcb_grab_pointer(impl_->connection,0,
                impl_->window,XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                XCB_GRAB_MODE_ASYNC,XCB_GRAB_MODE_ASYNC,XCB_NONE,impl_->cursor,XCB_CURRENT_TIME),nullptr);
            impl_->pointer_grabbed=grab && grab->status==XCB_GRAB_STATUS_SUCCESS;
            std::free(grab);
        }
        std::free(focus);
    } else {
        if (impl_->pointer_grabbed) xcb_ungrab_pointer(impl_->connection,XCB_CURRENT_TIME);
        impl_->pointer_grabbed=false;
        impl_->events.clear();
    }
    xcb_flush(impl_->connection);
#else
    (void)visible;
#endif
}
std::vector<UiInput> HomeKeyInput::take_events() {
#if defined(NEUROSHADE_HAVE_XCB_HOTKEY)
    auto result=std::move(impl_->events); impl_->events.clear(); return result;
#else
    return {};
#endif
}
bool HomeKeyInput::mouse_captured() const noexcept {
#if defined(NEUROSHADE_HAVE_XCB_HOTKEY)
    return impl_->pointer_grabbed;
#else
    return false;
#endif
}

}  // namespace neuroshade::overlay
