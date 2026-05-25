#ifndef CYD_MACHINE_H
#define CYD_MACHINE_H

// Explicit machine-model selection. PlatformIO environments should define
// exactly one of these so Mac Plus ROM patching and Macintosh LC bring-up stay
// isolated from one another.
#if defined(CYD_MACHINE_MAC_PLUS) && defined(CYD_MACHINE_MAC_LC)
#error "Select exactly one machine model: CYD_MACHINE_MAC_PLUS or CYD_MACHINE_MAC_LC"
#endif

#if !defined(CYD_MACHINE_MAC_PLUS) && !defined(CYD_MACHINE_MAC_LC)
#error "No Cydintosh machine model selected"
#endif

#if defined(CYD_MACHINE_MAC_PLUS)
#define CYD_MACHINE_NAME "Macintosh Plus"
#define CYD_ROM_EXPECTED_SIZE 0x20000u
#define CYD_ROM_KIND_MAC_PLUS 1
#endif

#if defined(CYD_MACHINE_MAC_LC)
#define CYD_MACHINE_NAME "Macintosh LC"
#define CYD_ROM_EXPECTED_SIZE 0x80000u
#define CYD_ROM_KIND_MAC_LC 1

#if !defined(CYD_BOARD_M5STACK_TAB5_ESP32P4_LC)
#error "Macintosh LC machine model is only supported by the M5Stack Tab5 ESP32-P4 LC target"
#endif
#endif

#if defined(CYD_BOARD_M5STACK_TAB5_ESP32P4_LC) && !defined(CYD_MACHINE_MAC_LC)
#error "M5Stack Tab5 LC board profile requires CYD_MACHINE_MAC_LC"
#endif

#endif
