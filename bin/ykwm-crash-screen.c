
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    Display *display = XOpenDisplay(NULL);

    if (!display) {
        fprintf(stderr, "Cannot open X display\n");
        return 1;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    int screen_width = DisplayWidth(display, screen);
    int screen_height = DisplayHeight(display, screen);

    const int bar_y = 29;
    const int bar_height = 32;

    Colormap cmap = DefaultColormap(display, screen);

    // Window attributes
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = BlackPixel(display, screen);

    Window crash = XCreateWindow(
        display,
        root,
        0, 0,
        screen_width,
        screen_height,
        0,
        DefaultDepth(display, screen),
        InputOutput,
        DefaultVisual(display, screen),
        CWOverrideRedirect | CWBackPixel,
        &attrs
    );

    // Allocate colors
    XColor deathBlue;
    XColor gray;
    XColor black;
    XColor white;

    XParseColor(display, cmap, "#0000AA", &deathBlue);
    XParseColor(display, cmap, "#C0C0C0", &gray);
    XParseColor(display, cmap, "#000000", &black);
    XParseColor(display, cmap, "#FFFFFF", &white);

    XAllocColor(display, cmap, &deathBlue);
    XAllocColor(display, cmap, &gray);
    XAllocColor(display, cmap, &black);
    XAllocColor(display, cmap, &white);

    XSetWindowBackground(display, crash, deathBlue.pixel);

    // Graphics context
    GC gc = XCreateGC(display, crash, 0, NULL);

    // Load Fixed font
    XFontStruct *font = XLoadQueryFont(
        display,
        "-*-fixed-*-*-*-*-14-*-*-*-*-*-*-1"
    );

    if (!font) {
        fprintf(stderr, "Could not load Fixed font\n");

        XFreeGC(display, gc);
        XDestroyWindow(display, crash);
        XCloseDisplay(display);

        return 1;
    }

    XSetFont(display, gc, font->fid);

    // Event selection
    XSelectInput(
        display,
        crash,
        ExposureMask | KeyPressMask
    );

    // Display crash screen
    XMapRaised(display, crash);
    XSync(display, False);

    // Grab pointer
    int pointer_status = XGrabPointer(
        display,
        crash,
        True,
        ButtonPressMask,
        GrabModeAsync,
        GrabModeAsync,
        None,
        None,
        CurrentTime
    );

    // Grab keyboard
    int keyboard_status = XGrabKeyboard(
        display,
        crash,
        True,
        GrabModeAsync,
        GrabModeAsync,
        CurrentTime
    );

    fprintf(
        stderr,
        "Pointer grab: %d | Keyboard grab: %d\n",
        pointer_status,
        keyboard_status
    );

    XFlush(display);

    // Main event loop
    int running = 1;

    while (running) {
        XEvent event;
        XNextEvent(display, &event);

        if (event.type == Expose && event.xexpose.count == 0) {

            // Gray title strip
            XSetForeground(display, gc, gray.pixel);

            XFillRectangle(
                display,
                crash,
                gc,
                0,
                bar_y,
                screen_width,
                bar_height
            );

            // Title
            const char *title = "YeKara Error";

            int title_y =
                bar_y +
                (bar_height - (font->ascent + font->descent)) / 2
                + font->ascent;

            XSetForeground(display, gc, black.pixel);

            XDrawString(
                display,
                crash,
                gc,
                16,
                title_y,
                title,
                strlen(title)
            );

            // Error message
            const char *message[] = {
                "Workspace Manager Failure.",
                "",
                "The workspace manager has encountered a fatal error.",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "Press F1 to restart the workspace manager"
            };

            int message_x = 29;
            int message_y = 120;
            int line_height = font->ascent + font->descent;

            XSetForeground(display, gc, white.pixel);

            for (int i = 0;
                 i < sizeof(message) / sizeof(message[0]);
                 i++) {

                if (message[i][0] == '\0')
                    continue;

                XDrawString(
                    display,
                    crash,
                    gc,
                    message_x,
                    message_y + i * line_height,
                    message[i],
                    strlen(message[i])
                );
            }
        }

        // F1 recovery
        if (event.type == KeyPress) {

            KeySym key = XLookupKeysym(&event.xkey, 0);

            if (key == XK_F1) {

                fprintf(stderr, "Restarting workspace manager...\n");

                // Release input grabs
                XUngrabPointer(display, CurrentTime);
                XUngrabKeyboard(display, CurrentTime);

                // Remove crash screen
                XDestroyWindow(display, crash);
                XFlush(display);

                // Restart FVWM
                int result = system("FvwmCommand 'Restart'");

                fprintf(stderr, "Restart command returned: %d\n", result);

                running = 0;
            }
        }
    }

    // Cleanup
    XFreeFont(display, font);
    XFreeGC(display, gc);

    XCloseDisplay(display);

    return 0;
}
