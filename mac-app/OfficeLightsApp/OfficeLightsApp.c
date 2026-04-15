#include <Devices.h>
#include <Dialogs.h>
#include <Events.h>
#include <Fonts.h>
#include <Menus.h>
#include <Quickdraw.h>
#include <Strings.h>
#include <TextEdit.h>
#include <Windows.h>
#include <stdio.h>
#include <string.h>

#include "../common/esp_ipc.h"

#define appleMenuID 1
#define fileMenuID  2

#define mAppleAbout  1
#define mFileRefresh 1
#define mFileQuit    3

#define CMD_GET_LIGHT_STATES     0x40
#define CMD_SET_LIGHT_STATE      0x41
#define CMD_SET_LIGHT_BRIGHTNESS 0x42

#define LIGHT_STATUS_UPDATED     0x00
#define LIGHT_STATUS_NOT_CHANGED 0x01

#define MAX_LIGHTS 3

typedef struct {
    unsigned char id;
    unsigned char state;
    unsigned char brightness;
    unsigned short color_temp;
    unsigned char linkquality;
    unsigned char available;
    char name[24];
    char last_seen[20];
} LightState;

typedef struct {
    unsigned char seq;
    unsigned char count;
    LightState lights[MAX_LIGHTS];
} LightSnapshot;

static WindowPtr mainWindow;
static MenuHandle appleMenu, fileMenu;
static Rect windowRect = {40, 10, 315, 235};
static LightSnapshot lightSnapshot;
static unsigned char lastSeq = 0;
static ControlHandle onButtons[MAX_LIGHTS];
static ControlHandle offButtons[MAX_LIGHTS];
static ControlHandle brightnessCtrls[MAX_LIGHTS];
static ControlHandle refreshBtn;

static void ctopstr(char *s) {
    int len = strlen(s);
    if (len > 255) len = 255;
    memmove(s + 1, s, len);
    s[0] = len;
}

static void init_toolbox(void) {
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
}

static int fetch_light_states(void) {
    unsigned char req[1];
    unsigned char len;
    unsigned char status;
    req[0] = lastSeq;
    ipc_send_command(CMD_GET_LIGHT_STATES, 1, req);
    status = ipc_get_response(&len, (unsigned char *)&lightSnapshot, sizeof(LightSnapshot));
    if (status == LIGHT_STATUS_UPDATED && len >= sizeof(unsigned char) * 2) {
        lastSeq = lightSnapshot.seq;
        return 1;
    }
    return 0;
}

static void send_light_state(unsigned char lightId, unsigned char on) {
    unsigned char req[2];
    unsigned char len;
    req[0] = lightId;
    req[1] = on;
    ipc_send_command(CMD_SET_LIGHT_STATE, 2, req);
    ipc_get_response(&len, NULL, 0);
}

static void send_light_brightness(unsigned char lightId, unsigned char brightness) {
    unsigned char req[2];
    unsigned char len;
    req[0] = lightId;
    req[1] = brightness;
    ipc_send_command(CMD_SET_LIGHT_BRIGHTNESS, 2, req);
    ipc_get_response(&len, NULL, 0);
}

static void draw_header(void) {
    MoveTo(10, 18);
    TextFace(bold);
    DrawString("\pOffice Lights");
    TextFace(0);
    MoveTo(10, 22);
    LineTo(215, 22);
}

static void draw_light_row(int index, int y) {
    char buf[64];
    LightState *light = &lightSnapshot.lights[index];

    sprintf(buf, "%s", light->name[0] ? light->name : "(unconfigured)");
    ctopstr(buf);
    MoveTo(10, y);
    TextFace(bold);
    DrawString((StringPtr)buf);
    TextFace(0);

    sprintf(buf, "State: %s  Brightness: %d  LQ: %d", light->state ? "On" : "Off",
            light->brightness, light->linkquality);
    ctopstr(buf);
    MoveTo(10, y + 14);
    DrawString((StringPtr)buf);

    if (light->last_seen[0]) {
        sprintf(buf, "Last seen: %s", light->last_seen);
        ctopstr(buf);
        MoveTo(10, y + 28);
        DrawString((StringPtr)buf);
    }
}

static void draw_content(void) {
    int i;
    SetPort(mainWindow);
    EraseRect(&mainWindow->portRect);
    draw_header();
    for (i = 0; i < MAX_LIGHTS; i++) {
        draw_light_row(i, 44 + (i * 74));
    }
    DrawControls(mainWindow);
}

static void update_controls_from_state(void) {
    int i;
    for (i = 0; i < MAX_LIGHTS; i++) {
        SetControlValue(brightnessCtrls[i], lightSnapshot.lights[i].brightness);
        SetControlValue(onButtons[i], lightSnapshot.lights[i].state ? 1 : 0);
        SetControlValue(offButtons[i], lightSnapshot.lights[i].state ? 0 : 1);
    }
}

static void create_controls(void) {
    Rect r;
    int i;
    for (i = 0; i < MAX_LIGHTS; i++) {
        int top = 88 + (i * 74);
        SetRect(&r, 10, top, 55, top + 18);
        onButtons[i] = NewControl(mainWindow, &r, "\pOn", true, 0, 0, 1, pushButProc, 100 + i);
        SetRect(&r, 60, top, 110, top + 18);
        offButtons[i] = NewControl(mainWindow, &r, "\pOff", true, 0, 0, 1, pushButProc, 110 + i);
        SetRect(&r, 118, top, 210, top + 16);
        brightnessCtrls[i] = NewControl(mainWindow, &r, "\p", true, 0, 0, 254, scrollBarProc, 120 + i);
    }
    SetRect(&r, 150, 278, 212, 298);
    refreshBtn = NewControl(mainWindow, &r, "\pRefresh", true, 0, 0, 1, pushButProc, 150);
}

static void handle_menu(long menuChoice) {
    int menuID = HiWord(menuChoice);
    int itemID = LoWord(menuChoice);
    Str255 daName;

    switch (menuID) {
    case appleMenuID:
        if (itemID == mAppleAbout) {
            Alert(128, NULL);
        } else if (itemID > 1) {
            GetMenuItemText(appleMenu, itemID, daName);
            OpenDeskAcc(daName);
        }
        HiliteMenu(0);
        break;
    case fileMenuID:
        if (itemID == mFileRefresh) {
            if (fetch_light_states()) {
                update_controls_from_state();
                InvalRect(&mainWindow->portRect);
            }
        } else if (itemID == mFileQuit) {
            ExitToShell();
        }
        HiliteMenu(0);
        break;
    }
}

static void handle_control_click(Point localPt) {
    ControlHandle ctrl;
    short part = FindControl(localPt, mainWindow, &ctrl);
    int i;

    if (part == inButton) {
        if (ctrl == refreshBtn) {
            if (TrackControl(ctrl, localPt, NULL) && fetch_light_states()) {
                update_controls_from_state();
                InvalRect(&mainWindow->portRect);
            }
            return;
        }

        for (i = 0; i < MAX_LIGHTS; i++) {
            if (ctrl == onButtons[i] && TrackControl(ctrl, localPt, NULL)) {
                send_light_state((unsigned char)i, 1);
                lightSnapshot.lights[i].state = 1;
                update_controls_from_state();
                InvalRect(&mainWindow->portRect);
                return;
            }
            if (ctrl == offButtons[i] && TrackControl(ctrl, localPt, NULL)) {
                send_light_state((unsigned char)i, 0);
                lightSnapshot.lights[i].state = 0;
                update_controls_from_state();
                InvalRect(&mainWindow->portRect);
                return;
            }
        }
    }

    if (part != 0) {
        for (i = 0; i < MAX_LIGHTS; i++) {
            if (ctrl == brightnessCtrls[i]) {
                TrackControl(ctrl, localPt, NULL);
                lightSnapshot.lights[i].brightness = GetControlValue(ctrl);
                send_light_brightness((unsigned char)i, lightSnapshot.lights[i].brightness);
                InvalRect(&mainWindow->portRect);
                return;
            }
        }
    }
}

static void setup_menus(void) {
    Handle mbar = GetNewMBar(128);
    if (!mbar) {
        appleMenu = NewMenu(appleMenuID, "\p\024");
        AppendMenu(appleMenu, "\pAbout OfficeLights");
        AppendMenu(appleMenu, "\p-");
        AppendResMenu(appleMenu, 'DRVR');
        InsertMenu(appleMenu, 0);

        fileMenu = NewMenu(fileMenuID, "\pFile");
        AppendMenu(fileMenu, "\pRefresh");
        AppendMenu(fileMenu, "\p-");
        AppendMenu(fileMenu, "\pQuit");
        InsertMenu(fileMenu, 0);
    } else {
        SetMenuBar(mbar);
        AppendMenu(GetMenu(appleMenuID), "\pAbout OfficeLights");
        AppendMenu(GetMenu(appleMenuID), "\p-");
        AppendResMenu(GetMenu(appleMenuID), 'DRVR');
        appleMenu = GetMenuHandle(appleMenuID);
        fileMenu = GetMenuHandle(fileMenuID);
    }
    DrawMenuBar();
}

void main(void) {
    EventRecord event;
    WindowPtr window;
    Point localPt;

    memset(&lightSnapshot, 0, sizeof(lightSnapshot));
    init_toolbox();
    setup_menus();

    mainWindow = NewWindow(NULL, &windowRect, "\pOffice Lights", true, documentProc,
                           (WindowPtr)-1, true, 0);
    SetPort(mainWindow);

    create_controls();
    fetch_light_states();
    update_controls_from_state();
    draw_content();

    while (1) {
        SystemTask();
        if (GetNextEvent(everyEvent, &event)) {
            switch (event.what) {
            case mouseDown:
                switch (FindWindow(event.where, &window)) {
                case inMenuBar:
                    handle_menu(MenuSelect(event.where));
                    break;
                case inSysWindow:
                    SystemClick(&event, window);
                    break;
                case inDrag:
                    DragWindow(window, event.where, &qd.screenBits.bounds);
                    break;
                case inGoAway:
                    if (TrackGoAway(window, event.where) && window == mainWindow) {
                        DisposeWindow(window);
                        ExitToShell();
                    }
                    break;
                case inContent:
                    if (window == mainWindow) {
                        localPt = event.where;
                        GlobalToLocal(&localPt);
                        handle_control_click(localPt);
                    } else {
                        SelectWindow(window);
                    }
                    break;
                }
                break;
            case keyDown:
            case autoKey:
                if (event.modifiers & cmdKey) {
                    if ((event.message & charCodeMask) == 'q') {
                        ExitToShell();
                    } else {
                        handle_menu(MenuKey(event.message & charCodeMask));
                    }
                }
                break;
            case updateEvt:
                window = (WindowPtr)event.message;
                BeginUpdate(window);
                if (window == mainWindow) {
                    draw_content();
                }
                EndUpdate(window);
                break;
            }
        }
    }
}
