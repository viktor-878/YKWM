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
 * YkwmCrashMon
 *
 * YeKara crash monitor module for FVWM.
 *
 * What it does
 * ------------
 *  1. HUNG WINDOWS
 *     Pings every monitored window that supports _NET_WM_PING. If a window
 *     does not answer within HangTimeout seconds, a Motif dialog is shown
 *     offering "Wait" or "Force Quit".
 *
 *  2. CRASHED APPLICATIONS
 *     When a monitored window is destroyed, the owning process
 *     (_NET_WM_PID) is inspected through /proc. If it died from a fatal
 *     signal (SEGV, ABRT, BUS, ILL, FPE, ...), or, optionally, exited with
 *     a non-zero status, a Motif error dialog is shown. A window that was
 *     hung and then disappeared is reported as well.
 *
 *  3. CRITICAL WINDOWS
 *     Windows matching a Critical rule (toolbox, desktop/icon window, ...)
 *     run CrashCommand (default: ykwm-crash-screen) as soon as they are
 *     destroyed (and optionally when they hang). No dialog is shown for
 *     these; the command is responsible for the user interface.
 *
 * The command is run via /bin/sh -c with these variables in its environment:
 *     YKWM_CRASH_REASON   critical-closed | critical-hung
 *     YKWM_CRASH_NAME     window title
 *     YKWM_CRASH_CLASS    WM_CLASS class
 *     YKWM_CRASH_RESOURCE WM_CLASS instance
 *     YKWM_CRASH_PID      owning pid (0 if unknown)
 *     YKWM_CRASH_WINDOW   X window id (decimal)
 *     YKWM_CRASH_SIGNAL   fatal signal number, when it could be determined
 *     YKWM_CRASH_EXITCODE exit code, when it could be determined
 *
 * Configuration (in your fvwm config; every option is optional):
 *
 *     DestroyModuleConfig YkwmCrashMon: *
 *     *YkwmCrashMon: PingInterval      5
 *     *YkwmCrashMon: HangTimeout       10
 *     *YkwmCrashMon: ShowDialogs       yes
 *     *YkwmCrashMon: MonitorAll        yes
 *     *YkwmCrashMon: ReportNonZeroExit no
 *     *YkwmCrashMon: CrashCommand      ykwm-crash-screen
 *     *YkwmCrashMon: CriticalDelay     500
 *     *YkwmCrashMon: CriticalCooldown  5
 *     *YkwmCrashMon: CriticalOnHang    no
 *     *YkwmCrashMon: Critical          Class   YkwmToolbox
 *     *YkwmCrashMon: Critical          Class   YkwmDesktop
 *     *YkwmCrashMon: Critical          Name    "Desktop*"
 *     *YkwmCrashMon: Ignore            Class   XClock
 *
 *     Module YkwmCrashMon
 *
 * Rule syntax:  Critical|Ignore  Class|Resource|Name  <glob pattern>
 *   Class    matches WM_CLASS class      (case-insensitive glob)
 *   Resource matches WM_CLASS instance
 *   Name     matches the window title
 *
 * Build (link against Motif and Xt in addition to the usual FVWM libs):
 *     LIBS += -lXm -lXt -lXext -lX11
 *
 * Notes / limitations
 * -------------------
 *  - Crash classification is best effort. It reads the exit status of the
 *    dead process from /proc/<pid>/stat while the process is still a zombie.
 *    If the parent reaps the process before we look, the exit cause is
 *    unknown and no dialog is shown. Hung detection and Critical windows do
 *    not depend on this.
 *  - Only windows that FVWM manages are visible to this module. If your
 *    desktop/icon window is override-redirect, FVWM will not report it; give
 *    it a normal managed window (e.g. _NET_WM_WINDOW_TYPE_DESKTOP) or monitor
 *    it by other means.
 *  - When FVWM quits or restarts, the module pipe closes and we exit, so
 *    session shutdown does not trigger the crash command (the short
 *    CriticalDelay exists for exactly that reason).
 */

#define _GNU_SOURCE

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <fnmatch.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/StringDefs.h>
#include <X11/Intrinsic.h>
#include <X11/Shell.h>

#include <Xm/Xm.h>
#include <Xm/MessageB.h>

#include "libs/fvwmlib.h"
#include "libs/Module.h"
#include "libs/Parse.h"
#include "libs/Strings.h"


/* ------------------------------------------------------------------ */
/* Types                                                              */
/* ------------------------------------------------------------------ */

enum {
    RULE_CLASS,
    RULE_RESOURCE,
    RULE_NAME
};

typedef struct _Rule {
    int kind;
    char *glob;
    struct _Rule *next;
} Rule;

typedef struct _WinInfo {
    Window w;               /* client window                     */
    Window frame;           /* FVWM frame window                 */

    char *name;
    char *res_name;
    char *res_class;

    pid_t pid;
    int pid_tries;

    Bool supports_ping;
    int proto_tries;

    Bool ping_pending;
    time_t ping_sent;
    time_t last_ping;

    Bool hung;
    Bool forced;            /* the user chose Force Quit         */

    Widget hang_dialog;

    struct _WinInfo *next;
} WinInfo;

enum {
    TOMB_WATCH,             /* poll /proc for the exit status    */
    TOMB_CRITICAL           /* delayed critical-window trigger   */
};

typedef struct _Tomb {
    int kind;
    char *name;
    char *res_class;
    char *res_name;
    pid_t pid;
    Window id;
    int tries;
    Bool was_hung;
    Bool forced;
    const char *reason;
} Tomb;


/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

static char *MyName = NULL;
static int MyNameLen;

static int fvwm_fd[2];

static XtAppContext app;
static Widget g_top;
static Display *dpy;
static Window Root;

static Atom a_WM_PROTOCOLS;
static Atom a_NET_WM_PING;
static Atom a_NET_WM_PID;

static XtEventDispatchProc prev_cm_dispatch = NULL;
static int (*prev_xerror)(Display *, XErrorEvent *) = NULL;

static WinInfo *windows = NULL;
static Rule *critical_rules = NULL;
static Rule *ignore_rules = NULL;

/* Options */
static int ping_interval = 5;           /* seconds                   */
static int hang_timeout = 10;           /* seconds                   */
static int critical_delay = 500;        /* milliseconds              */
static int critical_cooldown = 5;       /* seconds                   */
static Bool show_dialogs = True;
static Bool monitor_all = True;
static Bool report_nonzero_exit = False;
static Bool critical_on_hang = False;
static char *crash_command = NULL;

static time_t last_command_run = 0;

#define RECENT_MAX 16
static struct {
    pid_t pid;
    time_t when;
} recent[RECENT_MAX];
static int recent_next = 0;

RETSIGTYPE DeadPipe(int nonsense);

#define OWN_CLASS "YkwmCrashMon"
#define WATCH_INTERVAL_MS 250
#define WATCH_MAX_TRIES 12


/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static Bool ParseBoolean(const char *value)
{
    if (!value)
        return False;

    while (isspace((unsigned char)*value))
        value++;

    return (
        strncasecmp(value, "true", 4) == 0 ||
        strncasecmp(value, "yes", 3) == 0 ||
        strncasecmp(value, "on", 2) == 0 ||
        *value == '1'
    );
}

static char *Dup(const char *s)
{
    return s ? strdup(s) : NULL;
}

static void SetStr(char **dst, const char *src)
{
    free(*dst);
    *dst = Dup(src);
}

/* Remainder of a config line, trimmed and with matching quotes removed. */
static char *RestOfLine(const char *s)
{
    size_t n;
    char *r;

    while (isspace((unsigned char)*s))
        s++;

    n = strlen(s);

    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;

    if (n >= 2 &&
        ((s[0] == '"' && s[n - 1] == '"') ||
         (s[0] == '\'' && s[n - 1] == '\''))) {
        s++;
        n -= 2;
    }

    r = malloc(n + 1);

    if (!r)
        return NULL;

    memcpy(r, s, n);
    r[n] = '\0';

    return r;
}

static const char *DisplayName(const char *name,
                               const char *cls,
                               const char *res)
{
    if (name && *name)
        return name;
    if (cls && *cls)
        return cls;
    if (res && *res)
        return res;

    return "Unknown application";
}

static Bool IsCrashSignal(int sig)
{
    switch (sig) {
    case SIGSEGV:
    case SIGABRT:
    case SIGBUS:
    case SIGILL:
    case SIGFPE:
    case SIGSYS:
    case SIGTRAP:
    case SIGQUIT:
    case SIGXCPU:
    case SIGXFSZ:
        return True;
    default:
        return False;
    }
}

static Bool RecentlyReported(pid_t pid)
{
    int i;
    time_t now = time(NULL);

    if (pid <= 0)
        return False;

    for (i = 0; i < RECENT_MAX; i++) {
        if (recent[i].pid == pid && now - recent[i].when < 5)
            return True;
    }

    recent[recent_next].pid = pid;
    recent[recent_next].when = now;
    recent_next = (recent_next + 1) % RECENT_MAX;

    return False;
}


/* ------------------------------------------------------------------ */
/* Rules                                                              */
/* ------------------------------------------------------------------ */

static void AddRule(Rule **list, const char *kind, char *glob)
{
    Rule *r;
    int k;

    if (!glob || !*glob) {
        free(glob);
        return;
    }

    if (strcasecmp(kind, "Class") == 0)
        k = RULE_CLASS;
    else if (strcasecmp(kind, "Resource") == 0)
        k = RULE_RESOURCE;
    else if (strcasecmp(kind, "Name") == 0)
        k = RULE_NAME;
    else {
        fprintf(stderr, "%s: unknown rule kind '%s'\n", MyName + 1, kind);
        free(glob);
        return;
    }

    r = calloc(1, sizeof(*r));

    if (!r) {
        free(glob);
        return;
    }

    r->kind = k;
    r->glob = glob;
    r->next = *list;
    *list = r;
}

static Bool GlobMatch(const char *pattern, const char *text)
{
    int flags = 0;

    if (!text)
        return False;

#ifdef FNM_CASEFOLD
    flags |= FNM_CASEFOLD;
#endif

    return fnmatch(pattern, text, flags) == 0;
}

static Bool MatchRules(const Rule *list,
                       const char *name,
                       const char *cls,
                       const char *res)
{
    const Rule *r;

    for (r = list; r; r = r->next) {
        switch (r->kind) {
        case RULE_CLASS:
            if (GlobMatch(r->glob, cls))
                return True;
            break;
        case RULE_RESOURCE:
            if (GlobMatch(r->glob, res))
                return True;
            break;
        case RULE_NAME:
            if (GlobMatch(r->glob, name))
                return True;
            break;
        }
    }

    return False;
}

static Bool WinIsCritical(const WinInfo *wi)
{
    return MatchRules(critical_rules, wi->name, wi->res_class, wi->res_name);
}

static Bool WinIsMonitored(const WinInfo *wi)
{
    /* Never monitor our own dialogs. */
    if (wi->res_class && strcmp(wi->res_class, OWN_CLASS) == 0)
        return False;

    if (MatchRules(ignore_rules, wi->name, wi->res_class, wi->res_name))
        return False;

    if (monitor_all)
        return True;

    return WinIsCritical(wi);
}


/* ------------------------------------------------------------------ */
/* Window table                                                       */
/* ------------------------------------------------------------------ */

static WinInfo *FindWin(Window w)
{
    WinInfo *wi;

    for (wi = windows; wi; wi = wi->next) {
        if (wi->w == w)
            return wi;
    }

    return NULL;
}

static WinInfo *GetWin(Window w)
{
    WinInfo *wi = FindWin(w);

    if (wi)
        return wi;

    wi = calloc(1, sizeof(*wi));

    if (!wi)
        return NULL;

    wi->w = w;
    wi->last_ping = time(NULL);
    wi->next = windows;
    windows = wi;

    return wi;
}

static void FreeWin(WinInfo *wi)
{
    free(wi->name);
    free(wi->res_name);
    free(wi->res_class);
    free(wi);
}


/* ------------------------------------------------------------------ */
/* X helpers                                                          */
/* ------------------------------------------------------------------ */

static int XErrHandler(Display *d, XErrorEvent *e)
{
    /* Windows vanish all the time; errors against them are expected. */
    if (e->error_code == BadWindow || e->error_code == BadMatch)
        return 0;

    if (prev_xerror)
        return prev_xerror(d, e);

    return 0;
}

static pid_t QueryPid(Window w)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    pid_t pid = 0;

    if (XGetWindowProperty(dpy, w, a_NET_WM_PID, 0, 1, False,
                           XA_CARDINAL, &type, &fmt, &n, &after,
                           &data) == Success && data) {
        if (type == XA_CARDINAL && fmt == 32 && n == 1)
            pid = (pid_t)(*(unsigned long *)data);

        XFree(data);
    }

    return pid;
}

static Bool QuerySupportsPing(Window w)
{
    Atom *protos = NULL;
    int count = 0;
    int i;
    Bool found = False;

    if (XGetWMProtocols(dpy, w, &protos, &count) && protos) {
        for (i = 0; i < count; i++) {
            if (protos[i] == a_NET_WM_PING) {
                found = True;
                break;
            }
        }

        XFree(protos);
    }

    return found;
}

static void SendPing(WinInfo *wi)
{
    XEvent ev;
    time_t now = time(NULL);

    memset(&ev, 0, sizeof(ev));

    ev.xclient.type = ClientMessage;
    ev.xclient.window = wi->w;
    ev.xclient.message_type = a_WM_PROTOCOLS;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)a_NET_WM_PING;
    ev.xclient.data.l[1] = CurrentTime;
    ev.xclient.data.l[2] = (long)wi->w;

    XSendEvent(dpy, wi->w, False, NoEventMask, &ev);

    wi->ping_pending = True;
    wi->ping_sent = now;
    wi->last_ping = now;
}


/* ------------------------------------------------------------------ */
/* /proc exit status                                                  */
/* ------------------------------------------------------------------ */

/*
 * Returns:
 *   1  process is a zombie, *status holds its wait()-style status
 *   0  process is still alive
 *  -1  process is gone or /proc cannot be read
 */
static int ReadExitStatus(pid_t pid, int *status)
{
    char path[64];
    char buf[2048];
    FILE *fp;
    size_t n;
    char *p;
    int field;

    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);

    fp = fopen(path, "r");

    if (!fp)
        return -1;

    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);

    if (n == 0)
        return -1;

    buf[n] = '\0';

    /* The command name is in parentheses and may contain spaces. */
    p = strrchr(buf, ')');

    if (!p || p[1] != ' ')
        return -1;

    p += 2;                         /* p now points at field 3 (state) */

    if (*p != 'Z')
        return 0;

    /* Field 52 of /proc/<pid>/stat is exit_code. */
    for (field = 3; field < 52; field++) {
        p = strchr(p, ' ');

        if (!p)
            return -1;

        p++;
    }

    *status = atoi(p);

    return 1;
}


/* ------------------------------------------------------------------ */
/* Dialogs (Motif)                                                    */
/* ------------------------------------------------------------------ */

static Widget MakeDialog(Bool warning,
                         const char *title,
                         const char *text,
                         const char *ok_label,
                         const char *cancel_label)
{
    Arg args[8];
    Cardinal n = 0;
    Widget dlg;
    XmString xs_title = XmStringCreateLocalized((char *)title);
    XmString xs_text = XmStringCreateLocalized((char *)text);
    XmString xs_ok = XmStringCreateLocalized((char *)ok_label);
    XmString xs_cancel = NULL;

    XtSetArg(args[n], XmNdialogTitle, xs_title); n++;
    XtSetArg(args[n], XmNmessageString, xs_text); n++;
    XtSetArg(args[n], XmNokLabelString, xs_ok); n++;

    if (cancel_label) {
        xs_cancel = XmStringCreateLocalized((char *)cancel_label);
        XtSetArg(args[n], XmNcancelLabelString, xs_cancel); n++;
    }

    XtSetArg(args[n], XmNdialogStyle, XmDIALOG_MODELESS); n++;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;

    if (warning)
        dlg = XmCreateWarningDialog(g_top, "ykwmHangDialog", args, n);
    else
        dlg = XmCreateErrorDialog(g_top, "ykwmCrashDialog", args, n);

    XmStringFree(xs_title);
    XmStringFree(xs_text);
    XmStringFree(xs_ok);

    if (xs_cancel)
        XmStringFree(xs_cancel);

    XtVaSetValues(XtParent(dlg), XmNdeleteResponse, XmDESTROY, NULL);

    XtUnmanageChild(XmMessageBoxGetChild(dlg, XmDIALOG_HELP_BUTTON));

    if (!cancel_label) {
        XtUnmanageChild(XmMessageBoxGetChild(dlg, XmDIALOG_CANCEL_BUTTON));
    } else {
        XtVaSetValues(dlg,
                      XmNdefaultButtonType, XmDIALOG_CANCEL_BUTTON,
                      NULL);
    }

    return dlg;
}

static void DestroyDialogCB(Widget w, XtPointer client, XtPointer call)
{
    (void)client;
    (void)call;

    XtDestroyWidget(XtParent(w));
}

static void ShowErrorDialog(const char *title, const char *text)
{
    Widget dlg;

    if (!show_dialogs)
        return;

    dlg = MakeDialog(False, title, text, "Close", NULL);

    XtAddCallback(dlg, XmNokCallback, DestroyDialogCB, NULL);
    XtManageChild(dlg);

    XBell(dpy, 0);
}

static void ForceQuit(Window w)
{
    WinInfo *wi = FindWin(w);
    WinInfo *o;
    pid_t pid;

    if (!wi)
        return;

    pid = wi->pid;

    /* Mark every window of that process so none of them is reported as a
     * crash once the process dies. */
    for (o = windows; o; o = o->next) {
        if (o == wi || (pid > 0 && o->pid == pid))
            o->forced = True;
    }

    if (pid > 1 && pid != getpid())
        kill(pid, SIGKILL);
    else
        XKillClient(dpy, w);

    XFlush(dpy);
}

static void HangForceQuitCB(Widget w, XtPointer client, XtPointer call)
{
    (void)call;

    ForceQuit((Window)(uintptr_t)client);
    XtDestroyWidget(XtParent(w));
}

static void HangWaitCB(Widget w, XtPointer client, XtPointer call)
{
    WinInfo *wi = FindWin((Window)(uintptr_t)client);

    (void)call;

    if (wi) {
        /* Snooze: give it another full timeout before nagging again. */
        wi->hung = False;
        wi->ping_pending = True;
        wi->ping_sent = time(NULL);
    }

    XtDestroyWidget(XtParent(w));
}

static void HangDialogDestroyedCB(Widget w, XtPointer client, XtPointer call)
{
    WinInfo *wi = FindWin((Window)(uintptr_t)client);

    (void)w;
    (void)call;

    if (wi)
        wi->hang_dialog = NULL;
}

static void ShowHangDialog(WinInfo *wi)
{
    char text[1024];
    char title[256];
    Widget dlg;
    const char *label = DisplayName(wi->name, wi->res_class, wi->res_name);

    if (!show_dialogs || wi->hang_dialog)
        return;

    snprintf(title, sizeof(title), "%s - Not Responding", label);

    snprintf(text, sizeof(text),
             "\"%s\" is not responding.\n\n"
             "You can wait for it to recover, or force it to quit.\n"
             "Any unsaved work in that application will be lost.",
             label);

    dlg = MakeDialog(True, title, text, "Force Quit", "Wait");

    XtAddCallback(dlg, XmNokCallback,
                  HangForceQuitCB, (XtPointer)(uintptr_t)wi->w);
    XtAddCallback(dlg, XmNcancelCallback,
                  HangWaitCB, (XtPointer)(uintptr_t)wi->w);
    XtAddCallback(dlg, XmNdestroyCallback,
                  HangDialogDestroyedCB, (XtPointer)(uintptr_t)wi->w);

    wi->hang_dialog = dlg;

    XtManageChild(dlg);
    XBell(dpy, 0);
}


/* ------------------------------------------------------------------ */
/* Running the crash command                                          */
/* ------------------------------------------------------------------ */

static void SetEnvLong(const char *key, long v)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%ld", v);
    setenv(key, buf, 1);
}

static void RunCrashCommand(const char *reason,
                            const char *name,
                            const char *cls,
                            const char *res,
                            pid_t pid,
                            Window id)
{
    time_t now = time(NULL);
    pid_t child;
    int status = 0;
    int have_status = 0;

    if (!crash_command || !*crash_command)
        return;

    if (last_command_run != 0 &&
        now - last_command_run < critical_cooldown)
        return;

    last_command_run = now;

    if (pid > 0)
        have_status = (ReadExitStatus(pid, &status) == 1);

    child = fork();

    if (child < 0) {
        fprintf(stderr, "%s: fork failed: %s\n",
                MyName + 1, strerror(errno));
        return;
    }

    if (child == 0) {
        /* Double fork so the command is never our zombie. */
        pid_t grandchild = fork();

        if (grandchild != 0)
            _exit(0);

        {
            long maxfd = sysconf(_SC_OPEN_MAX);
            long fd;

            if (maxfd < 0 || maxfd > 4096)
                maxfd = 4096;

            for (fd = 3; fd < maxfd; fd++)
                close((int)fd);
        }

        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_DFL);

        setsid();

        setenv("YKWM_CRASH_REASON", reason, 1);
        setenv("YKWM_CRASH_NAME", name ? name : "", 1);
        setenv("YKWM_CRASH_CLASS", cls ? cls : "", 1);
        setenv("YKWM_CRASH_RESOURCE", res ? res : "", 1);
        SetEnvLong("YKWM_CRASH_PID", (long)pid);
        SetEnvLong("YKWM_CRASH_WINDOW", (long)id);

        if (have_status) {
            if (WIFSIGNALED(status))
                SetEnvLong("YKWM_CRASH_SIGNAL", WTERMSIG(status));
            else if (WIFEXITED(status))
                SetEnvLong("YKWM_CRASH_EXITCODE", WEXITSTATUS(status));
        }

        execl("/bin/sh", "sh", "-c", crash_command, (char *)NULL);
        _exit(127);
    }

    waitpid(child, NULL, 0);
}


/* ------------------------------------------------------------------ */
/* Tombstones: deferred decisions after a window is destroyed         */
/* ------------------------------------------------------------------ */

static void FreeTomb(Tomb *t)
{
    free(t->name);
    free(t->res_class);
    free(t->res_name);
    free(t);
}

static void ReportCrash(const Tomb *t, int status)
{
    char title[256];
    char text[1024];
    const char *label = DisplayName(t->name, t->res_class, t->res_name);

    if (RecentlyReported(t->pid))
        return;

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        const char *desc = strsignal(sig);

        snprintf(title, sizeof(title), "%s - Crashed", label);
        snprintf(text, sizeof(text),
                 "\"%s\" has crashed and had to close.\n\n"
                 "Cause: signal %d (%s)\n"
                 "Process ID: %ld",
                 label, sig, desc ? desc : "unknown", (long)t->pid);
    } else {
        snprintf(title, sizeof(title), "%s - Error", label);
        snprintf(text, sizeof(text),
                 "\"%s\" stopped because of an error.\n\n"
                 "Exit code: %d\n"
                 "Process ID: %ld",
                 label, WEXITSTATUS(status), (long)t->pid);
    }

    ShowErrorDialog(title, text);
}

static void TombTimer(XtPointer client, XtIntervalId *id)
{
    Tomb *t = (Tomb *)client;
    int status = 0;
    int r;

    (void)id;

    if (t->kind == TOMB_CRITICAL) {
        RunCrashCommand(t->reason, t->name, t->res_class, t->res_name,
                        t->pid, t->id);
        FreeTomb(t);
        return;
    }

    /* TOMB_WATCH */
    r = ReadExitStatus(t->pid, &status);

    if (r == 1) {
        if (WIFSIGNALED(status)) {
            if (IsCrashSignal(WTERMSIG(status)))
                ReportCrash(t, status);
        } else if (WIFEXITED(status) &&
                   WEXITSTATUS(status) != 0 &&
                   report_nonzero_exit) {
            ReportCrash(t, status);
        }

        FreeTomb(t);
        return;
    }

    if (r == -1) {
        /* Already reaped (or not ours to see): cause unknown. */
        FreeTomb(t);
        return;
    }

    /* Still alive: probably just one window of a multi-window app. */
    if (++t->tries >= WATCH_MAX_TRIES) {
        FreeTomb(t);
        return;
    }

    XtAppAddTimeOut(app, WATCH_INTERVAL_MS, TombTimer, t);
}

static void StartTomb(int kind,
                      const WinInfo *wi,
                      const char *reason,
                      unsigned long delay_ms)
{
    Tomb *t = calloc(1, sizeof(*t));

    if (!t)
        return;

    t->kind = kind;
    t->name = Dup(wi->name);
    t->res_class = Dup(wi->res_class);
    t->res_name = Dup(wi->res_name);
    t->pid = wi->pid;
    t->id = wi->w;
    t->was_hung = wi->hung;
    t->forced = wi->forced;
    t->reason = reason;

    XtAppAddTimeOut(app, delay_ms, TombTimer, t);
}


/* ------------------------------------------------------------------ */
/* Monitoring logic                                                   */
/* ------------------------------------------------------------------ */

static void OnHang(WinInfo *wi)
{
    wi->hung = True;

    if (WinIsCritical(wi) && critical_on_hang) {
        RunCrashCommand("critical-hung",
                        wi->name, wi->res_class, wi->res_name,
                        wi->pid, wi->w);
        return;
    }

    ShowHangDialog(wi);
}

static void HandlePong(Window w)
{
    WinInfo *wi = FindWin(w);

    if (!wi)
        return;

    wi->ping_pending = False;
    wi->last_ping = time(NULL);

    if (wi->hung) {
        wi->hung = False;

        if (wi->hang_dialog) {
            Widget d = wi->hang_dialog;

            wi->hang_dialog = NULL;
            XtDestroyWidget(XtParent(d));
        }
    }
}

static void OnWindowDestroyed(Window w)
{
    WinInfo **pp;
    WinInfo *wi = NULL;
    Bool critical;
    Bool monitored;

    for (pp = &windows; *pp; pp = &(*pp)->next) {
        if ((*pp)->w == w) {
            wi = *pp;
            *pp = wi->next;
            break;
        }
    }

    if (!wi)
        return;

    if (wi->hang_dialog) {
        Widget d = wi->hang_dialog;

        wi->hang_dialog = NULL;
        XtDestroyWidget(XtParent(d));
    }

    critical = WinIsCritical(wi);
    monitored = WinIsMonitored(wi) || critical;

    if (!monitored) {
        FreeWin(wi);
        return;
    }

    if (critical) {
        /* Small delay: if FVWM is quitting, our pipe closes first and we
         * exit before this fires. */
        StartTomb(TOMB_CRITICAL, wi, "critical-closed",
                  (unsigned long)critical_delay);
    } else if (wi->hung && !wi->forced) {
        /* It stopped responding and then went away. */
        Tomb t;
        char title[256];
        char text[1024];
        const char *label = DisplayName(wi->name, wi->res_class,
                                        wi->res_name);

        memset(&t, 0, sizeof(t));

        if (!RecentlyReported(wi->pid)) {
            snprintf(title, sizeof(title), "%s - Closed", label);
            snprintf(text, sizeof(text),
                     "\"%s\" stopped responding and has closed.\n\n"
                     "Process ID: %ld",
                     label, (long)wi->pid);

            ShowErrorDialog(title, text);
        }
    } else if (!wi->forced && wi->pid > 1) {
        StartTomb(TOMB_WATCH, wi, "closed", 0);
    }

    FreeWin(wi);
}

static void Tick(XtPointer client, XtIntervalId *id)
{
    WinInfo *wi;
    time_t now = time(NULL);

    (void)client;
    (void)id;

    for (wi = windows; wi; wi = wi->next) {
        Bool critical = WinIsCritical(wi);

        if (!WinIsMonitored(wi) && !critical)
            continue;

        if (wi->pid == 0 && wi->pid_tries < 3) {
            wi->pid_tries++;
            wi->pid = QueryPid(wi->w);
        }

        if (!wi->supports_ping) {
            if (wi->proto_tries < 3) {
                wi->proto_tries++;
                wi->supports_ping = QuerySupportsPing(wi->w);
            }

            if (!wi->supports_ping)
                continue;
        }

        if (wi->ping_pending) {
            if (!wi->hung && now - wi->ping_sent >= hang_timeout)
                OnHang(wi);
        } else if (now - wi->last_ping >= ping_interval) {
            SendPing(wi);
        }
    }

    XFlush(dpy);

    XtAppAddTimeOut(app, 1000, Tick, NULL);
}


/* ------------------------------------------------------------------ */
/* X event hook: catch _NET_WM_PING replies on the root window        */
/* ------------------------------------------------------------------ */

static Boolean ClientMessageDispatcher(XEvent *ev)
{
    if (ev->type == ClientMessage &&
        ev->xclient.window == Root &&
        ev->xclient.message_type == a_WM_PROTOCOLS &&
        ev->xclient.format == 32 &&
        (Atom)ev->xclient.data.l[0] == a_NET_WM_PING) {

        HandlePong((Window)ev->xclient.data.l[2]);
        return True;
    }

    if (prev_cm_dispatch)
        return prev_cm_dispatch(ev);

    return False;
}


/* ------------------------------------------------------------------ */
/* FVWM packets                                                       */
/* ------------------------------------------------------------------ */

static void OnFvwmInput(XtPointer client, int *source, XtInputId *id)
{
    FvwmPacket *pk;
    unsigned long *body;
    WinInfo *wi;

    (void)client;
    (void)source;
    (void)id;

    pk = ReadFvwmPacket(fvwm_fd[1]);

    if (!pk)
        exit(0);                    /* FVWM went away */

    body = pk->body;

    switch (pk->type) {

    case M_ADD_WINDOW:
    case M_CONFIGURE_WINDOW:
        wi = GetWin((Window)body[0]);

        if (wi)
            wi->frame = (Window)body[1];

        break;

    case M_WINDOW_NAME:
        wi = GetWin((Window)body[0]);

        if (wi)
            SetStr(&wi->name, (char *)&body[3]);

        break;

    case M_RES_CLASS:
        wi = GetWin((Window)body[0]);

        if (wi)
            SetStr(&wi->res_class, (char *)&body[3]);

        break;

    case M_RES_NAME:
        wi = GetWin((Window)body[0]);

        if (wi)
            SetStr(&wi->res_name, (char *)&body[3]);

        break;

    case M_DESTROY_WINDOW:
        OnWindowDestroyed((Window)body[0]);
        break;

    default:
        break;
    }
}


/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

static void ParseOptions(void)
{
    char *tline = NULL;
    char *rest;
    char *tok;
    char opt[64];
    char kind[32];

    InitGetConfigLine(fvwm_fd, MyName);

    while (GetConfigLine(fvwm_fd, &tline), tline != NULL) {

        if (strlen(tline) <= 1)
            continue;

        rest = tline;
        tok = PeekToken(rest, &rest);

        if (!tok)
            continue;

        /* Ignore lines not addressed to this module. */
        if (strncasecmp(tok, MyName, MyNameLen))
            continue;

        snprintf(opt, sizeof(opt), "%s", tok + MyNameLen);

        if (strcasecmp(opt, "PingInterval") == 0) {
            ping_interval = atoi(rest);

            if (ping_interval < 1)
                ping_interval = 1;
        }
        else if (strcasecmp(opt, "HangTimeout") == 0) {
            hang_timeout = atoi(rest);

            if (hang_timeout < 1)
                hang_timeout = 1;
        }
        else if (strcasecmp(opt, "CriticalDelay") == 0) {
            critical_delay = atoi(rest);

            if (critical_delay < 0)
                critical_delay = 0;
        }
        else if (strcasecmp(opt, "CriticalCooldown") == 0) {
            critical_cooldown = atoi(rest);

            if (critical_cooldown < 0)
                critical_cooldown = 0;
        }
        else if (strcasecmp(opt, "ShowDialogs") == 0) {
            show_dialogs = ParseBoolean(rest);
        }
        else if (strcasecmp(opt, "MonitorAll") == 0) {
            monitor_all = ParseBoolean(rest);
        }
        else if (strcasecmp(opt, "ReportNonZeroExit") == 0) {
            report_nonzero_exit = ParseBoolean(rest);
        }
        else if (strcasecmp(opt, "CriticalOnHang") == 0) {
            critical_on_hang = ParseBoolean(rest);
        }
        else if (strcasecmp(opt, "CrashCommand") == 0) {
            free(crash_command);
            crash_command = RestOfLine(rest);
        }
        else if (strcasecmp(opt, "Critical") == 0 ||
                 strcasecmp(opt, "Ignore") == 0) {
            char *k = PeekToken(rest, &rest);

            if (k) {
                snprintf(kind, sizeof(kind), "%s", k);

                AddRule(strcasecmp(opt, "Critical") == 0 ?
                            &critical_rules : &ignore_rules,
                        kind, RestOfLine(rest));
            }
        }
    }
}


/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    char *base;
    int xargc = 1;
    char *xargv[2];
    Dimension sw, sh;

    /* Module name as seen by FVWM's config lines. */
    base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];

    MyNameLen = (int)strlen(base) + 1;
    MyName = malloc(MyNameLen + 1);

    if (!MyName)
        exit(1);

    MyName[0] = '*';
    strcpy(MyName + 1, base);

    /* FVWM passes: <name> <write fd> <read fd> <config> <window> <ctx> */
    if (argc < 3) {
        fprintf(stderr,
                "%s: Version " VERSION
                " should only be executed by fvwm!\n",
                MyName + 1);
        exit(1);
    }

    fvwm_fd[0] = atoi(argv[1]);
    fvwm_fd[1] = atoi(argv[2]);

    crash_command = strdup("ykwm-crash-screen");

    /* Signals */
    {
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = DeadPipe;
        sigemptyset(&sa.sa_mask);

        sigaction(SIGPIPE, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
    }

    /* Toolkit: do not let Xt try to interpret FVWM's arguments. */
    xargv[0] = argv[0];
    xargv[1] = NULL;

    g_top = XtVaOpenApplication(&app, OWN_CLASS,
                                NULL, 0,
                                &xargc, xargv, NULL,
                                applicationShellWidgetClass,
                                XmNmappedWhenManaged, False,
                                XmNx, 0,
                                XmNy, 0,
                                NULL);

    dpy = XtDisplay(g_top);
    Root = RootWindow(dpy, DefaultScreen(dpy));

    /* Cover the screen (invisibly) so dialogs centre on the display. */
    sw = (Dimension)DisplayWidth(dpy, DefaultScreen(dpy));
    sh = (Dimension)DisplayHeight(dpy, DefaultScreen(dpy));

    XtVaSetValues(g_top, XmNwidth, sw, XmNheight, sh, NULL);

    a_WM_PROTOCOLS = XInternAtom(dpy, "WM_PROTOCOLS", False);
    a_NET_WM_PING = XInternAtom(dpy, "_NET_WM_PING", False);
    a_NET_WM_PID = XInternAtom(dpy, "_NET_WM_PID", False);

    prev_xerror = XSetErrorHandler(XErrHandler);

    /* _NET_WM_PING replies are sent to the root window with
     * SubstructureNotify|SubstructureRedirect, so selecting
     * SubstructureNotify is enough to receive them. */
    XSelectInput(dpy, Root, SubstructureNotifyMask);

    prev_cm_dispatch = XtSetEventDispatcher(dpy, ClientMessage,
                                            ClientMessageDispatcher);

    /* Receive and parse module configuration. */
    SetMessageMask(fvwm_fd, M_CONFIG_INFO | M_END_CONFIG_INFO);

    ParseOptions();

    /* Now subscribe to window events and ask for the current list. */
    SetMessageMask(fvwm_fd,
                   M_ADD_WINDOW |
                   M_CONFIGURE_WINDOW |
                   M_DESTROY_WINDOW |
                   M_WINDOW_NAME |
                   M_RES_CLASS |
                   M_RES_NAME |
                   M_END_WINDOWLIST);

    SendText(fvwm_fd, "Send_WindowList", 0);

    XtAppAddInput(app, fvwm_fd[1],
                  (XtPointer)XtInputReadMask,
                  OnFvwmInput, NULL);

    XtAppAddTimeOut(app, 1000, Tick, NULL);

    XtRealizeWidget(g_top);

    SendFinishedStartupNotification(fvwm_fd);

    XtAppMainLoop(app);

    return 0;
}


/*
 * FVWM died (or we were asked to terminate).
 */

RETSIGTYPE DeadPipe(int nonsense)
{
    (void)nonsense;

    exit(0);
    SIGNAL_RETURN;
}
