
/* -*-c-*- */
/* This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see:
 * <http://www.gnu.org/licenses/>
 */

/*
 * YkwmSplash
 *
 * YeKara splash screen module.
 *
 */

#include "config.h"

#ifdef HAVE_SYS_BSDTYPES_H
#include <sys/bsdtypes.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <ctype.h>
#include <time.h>

#include "libs/ftime.h"

#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include <X11/Xlib.h>
#include <X11/StringDefs.h>
#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/extensions/shape.h>

#include "libs/fvwmlib.h"
#include "libs/Picture.h"
#include "libs/Module.h"
#include "libs/FScreen.h"
#include "libs/FShape.h"
#include "libs/FRenderInit.h"
#include "libs/Graphics.h"
#include "libs/Parse.h"
#include "libs/Strings.h"

#include "ykwm-splash.xpm"
#include "ykwm-splash.xbm"


typedef struct _PImageIcon {
    Pixmap pixmap;
    Pixmap mask;
    int width;
    int height;
    int depth;
} PImageIcon;


/* Alignment options */

enum {
    ALIGN_LEFT,
    ALIGN_CENTER,
    ALIGN_RIGHT
};

enum {
    ALIGN_TOP,
    ALIGN_MIDDLE,
    ALIGN_BOTTOM
};


/* Function prototypes */

void GetXBMData(void);
void GetXPMData(char **);
void GetImageFile(char *, char *);
void change_window_name(char *);

static void parseOptions(int fd[2]);
static void CreateSplashBackground(void);
static void DestroySplash(void);
static Bool ParseBoolean(const char *value);


/* Global state */

PImageIcon view;

Window win;
Display *dpy;

char *imagePath = NULL;
char *imageName = NULL;
char *splash_bg_color = NULL;

static char *MyName = NULL;
static int MyNameLen;

int timeout = 3000000;

Window Root;

int screen;
int x_fd;

XSizeHints mysizehints;

Pixel back_pix;
Pixel fore_pix;

GC NormalGC;
GC FGC;

static Atom wm_del_win;

Bool no_wm = False;
Bool splash_bg = False;

int banner_halign = ALIGN_CENTER;
int banner_valign = ALIGN_MIDDLE;

Pixmap splash_pixmap = None;

#define MW_EVENTS (ExposureMask | ButtonReleaseMask)


/*
 * Parse boolean configuration values.
 */

static Bool ParseBoolean(const char *value)
{
    if (!value)
        return False;

    return (
        strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0 ||
        strcmp(value, "1") == 0
    );
}


/*
 * Create the full-screen splash background.
 */

static void CreateSplashBackground(void)
{
    int screen_width;
    int screen_height;

    int x = 0;
    int y = 0;

    XColor color;
    XGCValues gcv;
    GC gc;

    unsigned long bg;

    screen_width = DisplayWidth(dpy, screen);
    screen_height = DisplayHeight(dpy, screen);

    /*
     * Calculate horizontal banner position.
     */

    switch (banner_halign) {
    case ALIGN_LEFT:
        x = 0;
        break;

    case ALIGN_RIGHT:
        x = screen_width - view.width;
        break;

    case ALIGN_CENTER:
    default:
        x = (screen_width - view.width) / 2;
        break;
    }

    /*
     * Calculate vertical banner position.
     */

    switch (banner_valign) {
    case ALIGN_TOP:
        y = 0;
        break;

    case ALIGN_BOTTOM:
        y = screen_height - view.height;
        break;

    case ALIGN_MIDDLE:
    default:
        y = (screen_height - view.height) / 2;
        break;
    }

    /*
     * Resolve background color.
     */

    bg = BlackPixel(dpy, screen);

    if (splash_bg_color &&
        XParseColor(dpy, Pcmap, splash_bg_color, &color) &&
        XAllocColor(dpy, Pcmap, &color)) {
        bg = color.pixel;
    }

    /*
     * Create full-screen pixmap.
     */

    splash_pixmap = XCreatePixmap(
        dpy,
        win,
        screen_width,
        screen_height,
        Pdepth
    );

    if (splash_pixmap == None) {
        fprintf(stderr,
                "YkwmSplash: ERROR creating background pixmap\n");
        return;
    }

    /*
     * Fill the background.
     */

    gcv.foreground = bg;

    gc = XCreateGC(
        dpy,
        splash_pixmap,
        GCForeground,
        &gcv
    );

    XFillRectangle(
        dpy,
        splash_pixmap,
        gc,
        0,
        0,
        screen_width,
        screen_height
    );

    /*
     * Draw the banner.
     */

    XCopyArea(
        dpy,
        view.pixmap,
        splash_pixmap,
        gc,
        0,
        0,
        view.width,
        view.height,
        x,
        y
    );

    XFreeGC(dpy, gc);

    XSetWindowBackgroundPixmap(
        dpy,
        win,
        splash_pixmap
    );
}


/*
 * Destroy splash resources.
 */

static void DestroySplash(void)
{
    if (win != None) {
        XDestroyWindow(dpy, win);
        win = None;
    }

    if (splash_pixmap != None) {
        XFreePixmap(dpy, splash_pixmap);
        splash_pixmap = None;
    }

    XSync(dpy, False);
}


/*
 * Main entry point.
 */

int main(int argc, char **argv)
{
    char *display_name = NULL;
    char *string = NULL;

    int retval = 0;

    XEvent Event;

    fd_set in_fdset;
    struct timeval value;

    int fd[2];

    XSetWindowAttributes attr;

    int wx = 0;
    int wy = 0;

    int screen_width;
    int screen_height;

    time_t start_time;
    time_t current_time;

    long remaining_time;


    /*
     * Save program name.
     */

    string = strrchr(argv[0], '/');

    if (string != NULL)
        string++;
    else
        string = argv[0];

    MyNameLen = strlen(string) + 1;

    MyName = fxmalloc(MyNameLen + 1);

    *MyName = '*';
    strcpy(MyName + 1, string);


    /*
     * Get FVWM module file descriptors.
     */

    if (argc >= 3) {
        fd[0] = atoi(argv[1]);
        fd[1] = atoi(argv[2]);
    } else {
        fprintf(stderr,
                "%s: Version " VERSION
                " should only be executed by fvwm!\n",
                MyName + 1);

        exit(1);
    }


    /*
     * Optional command-line image.
     */

    if (argc > 6) {
        imageName = fxmalloc(strlen(argv[6]) + 1);
        strcpy(imageName, argv[6]);
    }


    /*
     * Open X display.
     */

    if (!(dpy = XOpenDisplay(display_name))) {
        fprintf(stderr,
                "YkwmSplash: can't open display %s\n",
                XDisplayName(display_name));

        exit(1);
    }

    screen = DefaultScreen(dpy);
    Root = RootWindow(dpy, screen);
    x_fd = XConnectionNumber(dpy);

    flib_init_graphics(dpy);


    /*
     * Receive module configuration.
     */

    SetMessageMask(fd, M_CONFIG_INFO | M_END_CONFIG_INFO);

    parseOptions(fd);


    /*
     * SplashBG requires an unmanaged full-screen window.
     */

    if (splash_bg)
        no_wm = True;


    /*
     * Create temporary window.
     *
     * Required before loading images into the selected visual.
     */

    attr.background_pixmap = None;
    attr.border_pixel = 0;
    attr.colormap = Pcmap;
    attr.override_redirect = no_wm;

    win = XCreateWindow(
        dpy,
        Root,
        -20,
        -20,
        10,
        10,
        0,
        Pdepth,
        InputOutput,
        Pvisual,
        CWColormap |
        CWBackPixmap |
        CWBorderPixel |
        CWOverrideRedirect,
        &attr
    );


    /*
     * Load banner image.
     */

    if (imageName)
        GetImageFile(imageName, imagePath);
    else
        GetXPMData(ykwm_splash_xpm);


    /*
     * Convert image depth if necessary.
     */

    if (view.depth != Pdepth) {
        XGCValues gcv;
        GC gc;
        Pixmap temp;

        gcv.background = PictureWhitePixel();
        gcv.foreground = PictureBlackPixel();

        gc = fvwmlib_XCreateGC(
            dpy,
            win,
            GCForeground | GCBackground,
            &gcv
        );

        temp = XCreatePixmap(
            dpy,
            win,
            view.width,
            view.height,
            Pdepth
        );

        XCopyPlane(
            dpy,
            view.pixmap,
            temp,
            gc,
            0,
            0,
            view.width,
            view.height,
            0,
            0,
            1
        );

        XFreePixmap(dpy, view.pixmap);
        XFreeGC(dpy, gc);

        view.pixmap = temp;
        view.depth = Pdepth;
    }


    /*
     * Determine window dimensions.
     */

    screen_width = DisplayWidth(dpy, screen);
    screen_height = DisplayHeight(dpy, screen);

    if (splash_bg) {
        mysizehints.width = screen_width;
        mysizehints.height = screen_height;

        wx = 0;
        wy = 0;

        CreateSplashBackground();
    } else {
        mysizehints.width = view.width;
        mysizehints.height = view.height;

        XSetWindowBackgroundPixmap(
            dpy,
            win,
            view.pixmap
        );

        FScreenCenterOnScreen(
            NULL,
            FSCREEN_PRIMARY,
            &wx,
            &wy,
            view.width,
            view.height
        );
    }


    /*
     * Window size hints.
     */

    mysizehints.flags =
        USSize |
        USPosition |
        PWinGravity |
        PResizeInc |
        PBaseSize |
        PMinSize |
        PMaxSize;

    mysizehints.width_inc = 1;
    mysizehints.height_inc = 1;

    mysizehints.base_width = mysizehints.width;
    mysizehints.base_height = mysizehints.height;

    mysizehints.min_width = mysizehints.width;
    mysizehints.min_height = mysizehints.height;

    mysizehints.max_width = mysizehints.width;
    mysizehints.max_height = mysizehints.height;

    mysizehints.win_gravity = NorthWestGravity;


    /*
     * Apply FVWM screen positioning hints when using
     * the ordinary banner-sized window.
     */

    if (!splash_bg) {
        FScreenMangleScreenIntoUSPosHints(
            FSCREEN_PRIMARY,
            &mysizehints
        );
    }


    /*
     * Window manager protocols.
     */

    wm_del_win = XInternAtom(
        dpy,
        "WM_DELETE_WINDOW",
        False
    );

    XSetWMProtocols(dpy, win, &wm_del_win, 1);

    XSetWMNormalHints(dpy, win, &mysizehints);

    change_window_name("YeKara Splash");


    /*
     * Position and resize.
     */

    XMoveResizeWindow(
        dpy,
        win,
        wx,
        wy,
        mysizehints.width,
        mysizehints.height
    );


    /*
     * Apply banner shape only when not using
     * a full-screen splash background.
     */

    if (!splash_bg &&
        FHaveShapeExtension &&
        view.mask != None) {

        FShapeCombineMask(
            dpy,
            win,
            FShapeBounding,
            0,
            0,
            view.mask,
            FShapeSet
        );
    }


    /*
     * Display window.
     */

    XMapWindow(dpy, win);

    XSync(dpy, False);

    XSelectInput(
        dpy,
        win,
        ButtonReleaseMask
    );


    /*
     * Tell FVWM that module startup is complete.
     */

    SendFinishedStartupNotification(fd);


    /*
     * Timeout event loop.
     *
     * Use elapsed time instead of reusing a modified
     * select() timeval.
     */

    start_time = time(NULL);

    while (1) {

        current_time = time(NULL);

        remaining_time =
            timeout / 1000000 -
            (long)(current_time - start_time);

        if (remaining_time <= 0) {
            DestroySplash();
            exit(0);
        }

        FD_ZERO(&in_fdset);
        FD_SET(x_fd, &in_fdset);

        value.tv_sec = remaining_time;
        value.tv_usec = 0;

        if (XPending(dpy)) {
            retval = 1;
        } else {
            retval = select(
                x_fd + 1,
                SELECT_FD_SET_CAST &in_fdset,
                NULL,
                NULL,
                &value
            );
        }

        if (retval == 0) {
            DestroySplash();
            exit(0);
        }

        if (retval < 0) {
            continue;
        }

        if (XPending(dpy)) {

            XNextEvent(dpy, &Event);

            switch (Event.type) {

            case ButtonRelease:
                DestroySplash();
                exit(0);

            case ClientMessage:

                if (Event.xclient.format == 32 &&
                    (Atom)Event.xclient.data.l[0] == wm_del_win) {

                    DestroySplash();
                    exit(0);
                }

                break;

            default:
                break;
            }
        }
    }

    return 0;
}


/*
 * Load embedded XBM fallback.
 */

void GetXBMData(void)
{
    if ((view.pixmap =
         XCreatePixmapFromBitmapData(
             dpy,
             win,
             (char *)ykwm_splash_bits,
             ykwm_splash_width,
             ykwm_splash_height,
             BlackPixel(dpy, screen),
             WhitePixel(dpy, screen),
             Pdepth
         )) == None) {

        fprintf(stderr,
                "YkwmSplash: ERROR converting embedded XBM\n");

        exit(1);
    }

    view.width = ykwm_splash_width;
    view.height = ykwm_splash_height;
    view.depth = Pdepth;
    view.mask = None;
}


/*
 * Load embedded XPM.
 */

void GetXPMData(char **data)
{
    if (!PImageLoadPixmapFromXpmData(
            dpy,
            win,
            0,
            data,
            &view.pixmap,
            &view.mask,
            &view.width,
            &view.height,
            &view.depth)) {

        GetXBMData();
    }
}


/*
 * Load external image file.
 */

void GetImageFile(char *file, char *path)
{
    char *full_file = NULL;

    FvwmPictureAttributes fpa;

    fpa.mask =
        FPAM_NO_ALLOC_PIXELS |
        FPAM_DITHER |
        FPAM_NO_ALPHA;

    if (file)
        full_file = PictureFindImageFile(file, path, R_OK);

    if (full_file) {

        if (PImageLoadPixmapFromFile(
                dpy,
                Root,
                full_file,
                &view.pixmap,
                &view.mask,
                None,
                &view.width,
                &view.height,
                &view.depth,
                0,
                NULL,
                0,
                fpa)) {

            return;
        }

        fprintf(stderr,
                "YkwmSplash: ERROR loading image file, "
                "using embedded fallback\n");

    } else {

        fprintf(stderr,
                "YkwmSplash: ERROR finding image file in ImagePath, "
                "using embedded fallback\n");
    }

    GetXPMData(ykwm_splash_xpm);
}


/*
 * Parse FVWM module configuration.
 */

static void parseOptions(int fd[2])
{
    char *tline = NULL;
    char *tok;

    InitGetConfigLine(fd, MyName);

    while (GetConfigLine(fd, &tline), tline != NULL) {

        if (strlen(tline) <= 1)
            continue;

        tok = PeekToken(tline, &tline);


        /*
         * General FVWM configuration.
         */

        if (StrEquals(tok, "ImagePath")) {

            CopyString(&imagePath, tline);

            if (imagePath[0] == 0) {
                free(imagePath);
                imagePath = NULL;
            }

            continue;
        }


        /*
         * Ignore configuration lines not addressed
         * to this module.
         */

        if (strncasecmp(tok, MyName, MyNameLen))
            continue;


        /*
         * Module-specific configuration.
         */

        tok += MyNameLen;


        if (StrEquals(tok, "Pixmap")) {

            if (imageName == NULL) {
                CopyString(&imageName, tline);

                if (imageName[0] == 0) {
                    free(imageName);
                    imageName = NULL;
                }
            }

            continue;
        }


        if (StrEquals(tok, "NoDecor")) {
            no_wm = True;
            continue;
        }


        if (StrEquals(tok, "Timeout")) {
            timeout = atoi(tline) * 1000000;

            if (timeout < 0)
                timeout = 0;

            continue;
        }


        /*
         * Splash background.
         */

        if (StrEquals(tok, "SplashBG")) {
            splash_bg = ParseBoolean(tline);
            continue;
        }


        if (StrEquals(tok, "SplashBGColor")) {

            CopyString(&splash_bg_color, tline);

            if (splash_bg_color[0] == 0) {
                free(splash_bg_color);
                splash_bg_color = NULL;
            }

            continue;
        }


        /*
         * Horizontal banner alignment.
         */

        if (StrEquals(tok, "BannerHAlign")) {

            if (strcasecmp(tline, "Left") == 0)
                banner_halign = ALIGN_LEFT;

            else if (strcasecmp(tline, "Right") == 0)
                banner_halign = ALIGN_RIGHT;

            else
                banner_halign = ALIGN_CENTER;

            continue;
        }


        /*
         * Vertical banner alignment.
         */

        if (StrEquals(tok, "BannerVAlign")) {

            if (strcasecmp(tline, "Top") == 0)
                banner_valign = ALIGN_TOP;

            else if (strcasecmp(tline, "Bottom") == 0)
                banner_valign = ALIGN_BOTTOM;

            else
                banner_valign = ALIGN_MIDDLE;

            continue;
        }
    }
}


/*
 * Set window title.
 */

void change_window_name(char *str)
{
    XTextProperty name;

    if (XStringListToTextProperty(&str, 1, &name) == 0) {

        fprintf(stderr,
                "YkwmSplash: cannot allocate window name\n");

        return;
    }

    XSetWMName(dpy, win, &name);
    XSetWMIconName(dpy, win, &name);

    XFree(name.value);
}


/*
 * FVWM died.
 */

RETSIGTYPE DeadPipe(int nonsense)
{
    exit(0);
    SIGNAL_RETURN;
}
