#pragma once
#include <QLibrary>
#include <QPoint>

// XTEST sends real server input to the foreign client. QTest's QWidget events
// only reach BAT and cannot test mouse selection or paste inside another app.
class X11Input {
public:
    X11Input() {
        if (open && close && flush && motion && button && key && keysym && keycode && setFocus && getFocus)
            display = open(nullptr);
    }
    ~X11Input() { if (display) close(display); }
    bool available() const { return display; }
    void move(const QPoint &point) { motion(display, -1, point.x(), point.y(), 0); flush(display); }
    void press(unsigned int value) { button(display, value, true, 0); flush(display); }
    void release(unsigned int value) { button(display, value, false, 0); flush(display); }
    void keyEvent(const char *name, bool pressed) {
        key(display, keycode(display, keysym(name)), pressed, 0); flush(display);
    }
    void focus(unsigned long window) { setFocus(display, window, 2, 0); flush(display); }
    unsigned long focusedWindow() const {
        unsigned long window = 0; int revert = 0;
        if (getFocus) getFocus(display, &window, &revert);
        return window;
    }
    bool createUnmappedWindows(int count) {
        if (!display || !root || !create || !sync) return false;
        for (int i = 0; i < count; ++i)
            create(display, root(display), 0, 0, 1, 1, 0, 0, 0);
        sync(display, false);
        return true; // Closing this connection releases all fixture windows.
    }
    bool setProcessId(unsigned long window, unsigned long pid) {
        return scalarProperty(window, "_NET_WM_PID", 6 /* XA_CARDINAL */, pid);
    }
    bool setWindowType(unsigned long window, const char *type) {
        if (!display || !atom) return false;
        return scalarProperty(window, "_NET_WM_WINDOW_TYPE", 4 /* XA_ATOM */, atom(display, type, false));
    }
    bool setTransientFor(unsigned long window, unsigned long parent) {
        return scalarProperty(window, "WM_TRANSIENT_FOR", 33 /* XA_WINDOW */, parent);
    }
private:
    bool scalarProperty(unsigned long window, const char *name, unsigned long type, unsigned long value) {
        if (!display || !atom || !changeProperty || !deleteProperty || !sync) return false;
        const auto property = atom(display, name, false);
        if (value) changeProperty(display, window, property, type, 32, 0,
                                  reinterpret_cast<const unsigned char *>(&value), 1);
        else deleteProperty(display, window, property);
        sync(display, false);
        return true;
    }
    QLibrary x11{"X11", 6}, xtest{"Xtst", 6};
    void *(*open)(const char *) = reinterpret_cast<decltype(open)>(x11.resolve("XOpenDisplay"));
    int (*close)(void *) = reinterpret_cast<decltype(close)>(x11.resolve("XCloseDisplay"));
    int (*flush)(void *) = reinterpret_cast<decltype(flush)>(x11.resolve("XFlush"));
    int (*motion)(void *, int, int, int, unsigned long) = reinterpret_cast<decltype(motion)>(xtest.resolve("XTestFakeMotionEvent"));
    int (*button)(void *, unsigned int, int, unsigned long) = reinterpret_cast<decltype(button)>(xtest.resolve("XTestFakeButtonEvent"));
    int (*key)(void *, unsigned int, int, unsigned long) = reinterpret_cast<decltype(key)>(xtest.resolve("XTestFakeKeyEvent"));
    unsigned long (*keysym)(const char *) = reinterpret_cast<decltype(keysym)>(x11.resolve("XStringToKeysym"));
    unsigned char (*keycode)(void *, unsigned long) = reinterpret_cast<decltype(keycode)>(x11.resolve("XKeysymToKeycode"));
    int (*setFocus)(void *, unsigned long, int, unsigned long) = reinterpret_cast<decltype(setFocus)>(x11.resolve("XSetInputFocus"));
    int (*getFocus)(void *, unsigned long *, int *) = reinterpret_cast<decltype(getFocus)>(x11.resolve("XGetInputFocus"));
    unsigned long (*root)(void *) = reinterpret_cast<decltype(root)>(x11.resolve("XDefaultRootWindow"));
    unsigned long (*create)(void *, unsigned long, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long) =
        reinterpret_cast<decltype(create)>(x11.resolve("XCreateSimpleWindow"));
    int (*sync)(void *, int) = reinterpret_cast<decltype(sync)>(x11.resolve("XSync"));
    unsigned long (*atom)(void *, const char *, int) = reinterpret_cast<decltype(atom)>(x11.resolve("XInternAtom"));
    int (*changeProperty)(void *, unsigned long, unsigned long, unsigned long, int, int, const unsigned char *, int) =
        reinterpret_cast<decltype(changeProperty)>(x11.resolve("XChangeProperty"));
    int (*deleteProperty)(void *, unsigned long, unsigned long) = reinterpret_cast<decltype(deleteProperty)>(x11.resolve("XDeleteProperty"));
    void *display = nullptr;
};
