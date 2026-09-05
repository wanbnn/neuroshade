#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

int main() {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "cannot open X display\n";
        return 1;
    }

    if (const char* atom_name = std::getenv("NEUROSHADE_TEST_HOME_ATOM");
        atom_name != nullptr && atom_name[0] != '\0') {
        const Atom atom = XInternAtom(display, atom_name, False);
        const unsigned long value = 1;
        XChangeProperty(display, DefaultRootWindow(display), atom, XA_CARDINAL, 32,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&value), 1);
        XSync(display, False);
        XCloseDisplay(display);
        return 0;
    }

    const KeyCode home = XKeysymToKeycode(display, XK_Home);
    if (home == 0) {
        std::cerr << "X server has no Home keycode\n";
        XCloseDisplay(display);
        return 1;
    }

    const bool pressed = XTestFakeKeyEvent(display, home, True, CurrentTime) != 0;
    XSync(display, False);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const bool released = XTestFakeKeyEvent(display, home, False, CurrentTime) != 0;
    XSync(display, False);
    XCloseDisplay(display);

    if (!pressed || !released) {
        std::cerr << "XTest failed to synthesize Home"
                  << " pressed=" << pressed << " released=" << released << '\n';
        return 1;
    }
    return 0;
}
