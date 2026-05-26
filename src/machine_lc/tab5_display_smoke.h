#ifndef MACHINE_LC_TAB5_DISPLAY_SMOKE_H
#define MACHINE_LC_TAB5_DISPLAY_SMOKE_H

#include <stdint.h>

#ifndef TAB5_DSI_PANEL_H_RES
#define TAB5_DSI_PANEL_H_RES 720u
#endif

#ifndef TAB5_DSI_PANEL_V_RES
#define TAB5_DSI_PANEL_V_RES 1280u
#endif

#ifndef TAB5_DSI_SMOKE_STRIP_LINES
#define TAB5_DSI_SMOKE_STRIP_LINES 16u
#endif

typedef enum {
    TAB5_DISPLAY_SMOKE_SOLID_COLORS,
    TAB5_DISPLAY_SMOKE_ORIENTATION_MARKERS,
    TAB5_DISPLAY_SMOKE_ONE_BIT_CHECKER,
    TAB5_DISPLAY_SMOKE_INDEXED_RAMP,
} tab5_display_smoke_pattern_t;

void tab5_display_smoke_log_config(void);
uint32_t tab5_display_smoke_render_pattern(tab5_display_smoke_pattern_t pattern);
void tab5_display_smoke_probe_patterns(void);
const char *tab5_display_smoke_pattern_name(tab5_display_smoke_pattern_t pattern);

#endif
