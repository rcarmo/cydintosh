#include "Icons.r"
#include "Types.r"
#include "Dialogs.r"
#include "Menus.r"
#include "Processes.r"
#include "Finder.r"

type 'OLIT'
{
    byte;
};

resource 'ICN#' (128) {
    {
        $"0000000000000000"
        $"0000000000181800"
        $"003C3C00007E7E00"
        $"00FFFF0000FFFF00"
        $"01C3C38001818180"
        $"038181C0038181C0"
        $"038181C0038181C0"
        $"038181C0038181C0"
        $"038181C0038181C0"
        $"038181C0038181C0"
        $"038181C001818180"
        $"01C3C38000FFFF00"
        $"00FFFF00007E7E00"
        $"003C3C0000181800"
        $"0000000000000000"
        $"0000000000000000",
        $"0000000000000000"
        $"00181800003C3C00"
        $"007E7E0000FFFF00"
        $"01FFFF8003FFFFC0"
        $"07FFFFE007FFFFE0"
        $"07FFFFE007FFFFE0"
        $"07FFFFE007FFFFE0"
        $"07FFFFE007FFFFE0"
        $"07FFFFE007FFFFE0"
        $"07FFFFE007FFFFE0"
        $"03FFFFC001FFFF80"
        $"00FFFF00007E7E00"
        $"003C3C0000181800"
        $"0000000000000000"
        $"0000000000000000"
    }
};

resource 'ics#' (128) {
    {
        $"0000018003C007E00FF01FF83C3C381C381C381C381C381C1FF80FF007E003C0018000000000",
        $"0000018003C007E00FF01FF83FFC7FFE7FFE7FFE7FFE7FFE3FFC1FF80FF007E003C0018000000000"
    }
};

resource 'FREF' (128) {
    'APPL',
    0,
    ""
};

resource 'BNDL' (128) {
    'OLIT',
    0,
    {
        'ICN#', {
            0, 128
        },
        'FREF', {
            0, 128
        }
    }
};

resource 'OLIT' (0, sysHeap) {
    0
};

resource 'STR ' (128) {
    "OfficeLights"
};

resource 'DITL' (128) {
    {
        { 80, 70, 100, 150 },
        Button { enabled, "OK" };
        { 10, 20, 26, 200 },
        StaticText { disabled, "Cydintosh Office Lights" };
        { 30, 72, 42, 148 },
        StaticText { disabled, "v1.0.0" };
        { 48, 42, 60, 180 },
        StaticText { disabled, "Office lighting controller" };
    }
};

resource 'ALRT' (128) {
    { 100, 20, 210, 220 },
    128,
    {
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1,
    },
    noAutoCenter
};

resource 'MENU' (1) {
    1, textMenuProc;
    allEnabled, enabled;
    apple;
    {
    }
};

resource 'MENU' (2) {
    2, textMenuProc;
    allEnabled, enabled;
    "File";
    {
        "Refresh", noIcon, "R", noMark, plain;
        "-", noIcon, noKey, noMark, plain;
        "Quit", noIcon, "Q", noMark, plain;
    }
};

resource 'MBAR' (128) {
    { 1, 2 };
};

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    notHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    256 * 1024,
    128 * 1024
};
