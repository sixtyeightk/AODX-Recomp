/*
 * switch.h — minimal libnx stand-in for the Mac Switch-stub build
 * (ARCUS_FAKE_SWITCH=ON). Declares only what the game and runner use, with
 * libnx's names, values and semantics, so the Switch code paths compile and
 * run unchanged on the desktop. Not used for the real .nro.
 *
 * Test controls (libnx_stub.c):
 *   libnx_stub_pad_set()   injects one Npad's raw state (buttons, sticks)
 *   GENESIS_FAKE_HOME=N:M  appletGetFocusState() reports the HOME menu
 *                          (Background) from its N-th call for M calls
 *   GENESIS_FAKE_DOCKED=1  appletGetOperationMode() reports Console
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;
typedef u32      Result;

#define BIT(n)  (1U << (n))
#define BITL(n) (1UL << (n))

/* ---- hid / pad -------------------------------------------------------------------- */
typedef enum {
    HidNpadIdType_No1 = 0, HidNpadIdType_No2 = 1, HidNpadIdType_No3 = 2, HidNpadIdType_No4 = 3,
    HidNpadIdType_No5 = 4, HidNpadIdType_No6 = 5, HidNpadIdType_No7 = 6, HidNpadIdType_No8 = 7,
    HidNpadIdType_Other = 0x10, HidNpadIdType_Handheld = 0x20,
} HidNpadIdType;

typedef enum {
    HidNpadStyleTag_NpadFullKey  = BIT(0),
    HidNpadStyleTag_NpadHandheld = BIT(1),
    HidNpadStyleTag_NpadJoyDual  = BIT(2),
    HidNpadStyleTag_NpadJoyLeft  = BIT(3),
    HidNpadStyleTag_NpadJoyRight = BIT(4),
} HidNpadStyleTag;
#define HidNpadStyleSet_NpadFullCtrl (HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual)
#define HidNpadStyleSet_NpadStandard (HidNpadStyleSet_NpadFullCtrl | HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)

typedef enum { HidNpadAttribute_IsConnected = BIT(0) } HidNpadAttribute;

typedef enum {
    HidNpadButton_A      = BITL(0),  HidNpadButton_B      = BITL(1),
    HidNpadButton_X      = BITL(2),  HidNpadButton_Y      = BITL(3),
    HidNpadButton_StickL = BITL(4),  HidNpadButton_StickR = BITL(5),
    HidNpadButton_L      = BITL(6),  HidNpadButton_R      = BITL(7),
    HidNpadButton_ZL     = BITL(8),  HidNpadButton_ZR     = BITL(9),
    HidNpadButton_Plus   = BITL(10), HidNpadButton_Minus  = BITL(11),
    HidNpadButton_Left   = BITL(12), HidNpadButton_Up     = BITL(13),
    HidNpadButton_Right  = BITL(14), HidNpadButton_Down   = BITL(15),
} HidNpadButton;

typedef struct HidAnalogStickState { s32 x; s32 y; } HidAnalogStickState;
#define JOYSTICK_MAX (0x7FFF)
#define JOYSTICK_MIN (-0x7FFF)

typedef struct {
    u8 id_mask;
    u8 active_id_mask;
    bool read_handheld;
    bool active_handheld;
    u32 style_set;
    u32 attributes;
    u64 buttons_cur;
    u64 buttons_old;
    HidAnalogStickState sticks[2];
    u32 gc_triggers[2];
} PadState;

void padConfigureInput(u32 max_players, u32 style_set);
void padInitializeWithMask(PadState *pad, u64 mask);
#define padInitialize(_pad, ...) do { \
    const HidNpadIdType _pad_ids[] = { __VA_ARGS__ }; \
    u64 _pad_mask = 0; \
    for (unsigned _pad_i = 0; _pad_i < sizeof(_pad_ids) / sizeof(_pad_ids[0]); ++_pad_i) \
        _pad_mask |= 1UL << (_pad_ids[_pad_i]); \
    padInitializeWithMask((_pad), _pad_mask); \
} while (0)
void padUpdate(PadState *pad);
static inline bool padIsConnected(const PadState *pad) { return pad->attributes & HidNpadAttribute_IsConnected; }
static inline u64 padGetButtons(const PadState *pad) { return pad->buttons_cur; }
static inline u64 padGetButtonsDown(const PadState *pad) { return ~pad->buttons_old & pad->buttons_cur; }
static inline HidAnalogStickState padGetStickPos(const PadState *pad, unsigned i) { return pad->sticks[i]; }

/* ---- applet ----------------------------------------------------------------------- */
typedef enum { AppletOperationMode_Handheld = 0, AppletOperationMode_Console = 1 } AppletOperationMode;
typedef enum {
    AppletHookType_OnFocusState = 0, AppletHookType_OnOperationMode, AppletHookType_OnPerformanceMode,
    AppletHookType_OnExitRequest, AppletHookType_OnResume,
} AppletHookType;
typedef enum {
    AppletFocusState_InFocus = 1, AppletFocusState_OutOfFocus = 2, AppletFocusState_Background = 3,
} AppletFocusState;
typedef void (*AppletHookFn)(AppletHookType hook, void *param);
typedef struct AppletHookCookie {
    struct AppletHookCookie *next;
    AppletHookFn callback;
    void *param;
} AppletHookCookie;

bool appletMainLoop(void);
void appletHook(AppletHookCookie *cookie, AppletHookFn callback, void *param);
AppletOperationMode appletGetOperationMode(void);
AppletFocusState appletGetFocusState(void);

/* ---- svc -------------------------------------------------------------------------- */
void svcSleepThread(s64 nano);
/* Applet trace (engine patch 0022): system tick, process memory info, applet type. */
u64 armGetSystemTick(void);
u64 armTicksToNs(u64 tick);
typedef u32 Handle;
#define CUR_PROCESS_HANDLE 0xFFFF8001u
enum { InfoType_TotalMemorySize = 6, InfoType_UsedMemorySize = 7 };
Result svcGetInfo(u64 *out, u32 id0, Handle handle, u64 id1);
int appletGetAppletType(void);

/* ---- stub-only test controls ------------------------------------------------------ */
/* Raw state of Npad slot `npad` (0 = No1/handheld, 1 = No2). Sticks use libnx units
 * (+-JOYSTICK_MAX, y up). Takes effect at the next padUpdate. */
void libnx_stub_pad_set(int npad, u64 buttons, s32 lx, s32 ly, s32 rx, s32 ry, int connected);
/* Number of appletGetFocusState() calls that reported out of focus so far. */
unsigned libnx_stub_focus_lost_polls(void);
