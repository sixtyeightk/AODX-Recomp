/*
 * libnx_stub.c — desktop implementation of the libnx subset in stub/switch.h,
 * for the Mac Switch-stub build only (ARCUS_FAKE_SWITCH=ON).
 */
#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL2/SDL.h>

#define STUB_NPADS 8

static struct { u64 buttons; HidAnalogStickState l, r; int connected; } s_npad[STUB_NPADS];
static u32 s_max_players = 8;

void libnx_stub_pad_set(int npad, u64 buttons, s32 lx, s32 ly, s32 rx, s32 ry, int connected)
{
    if (npad < 0 || npad >= STUB_NPADS) return;
    s_npad[npad].buttons = buttons;
    s_npad[npad].l.x = lx; s_npad[npad].l.y = ly;
    s_npad[npad].r.x = rx; s_npad[npad].r.y = ry;
    s_npad[npad].connected = connected;
}

void padConfigureInput(u32 max_players, u32 style_set)
{
    (void)style_set;
    s_max_players = max_players ? max_players : 1;
}

void padInitializeWithMask(PadState *pad, u64 mask)
{
    memset(pad, 0, sizeof *pad);
    pad->id_mask = (u8)(mask & 0xFF);
    pad->read_handheld = (mask >> HidNpadIdType_Handheld) & 1;
}

/* As libnx: the first connected Npad in the mask wins (handheld counts as No1). */
void padUpdate(PadState *pad)
{
    pad->buttons_old = pad->buttons_cur;
    pad->buttons_cur = 0;
    pad->attributes = 0;
    pad->active_id_mask = 0;
    memset(pad->sticks, 0, sizeof pad->sticks);
    for (unsigned i = 0; i < STUB_NPADS && i < s_max_players; i++) {
        int in_mask = (pad->id_mask >> i) & 1;
        if (i == 0 && pad->read_handheld) in_mask = 1;
        if (!in_mask || !s_npad[i].connected) continue;
        pad->active_id_mask |= (u8)(1u << i);
        pad->attributes = HidNpadAttribute_IsConnected;
        pad->style_set = HidNpadStyleTag_NpadFullKey;
        pad->buttons_cur = s_npad[i].buttons;
        pad->sticks[0] = s_npad[i].l;
        pad->sticks[1] = s_npad[i].r;
        break;
    }
}

/* ---- applet ---- */
static AppletHookCookie *s_hooks;
static unsigned long s_focus_polls, s_lost_polls;
static long s_home_from = -1, s_home_len;
static int s_home_parsed, s_last_focus = AppletFocusState_InFocus;

static void fire_hooks(AppletHookType t)
{
    for (AppletHookCookie *c = s_hooks; c; c = c->next) c->callback(t, c->param);
}

void appletHook(AppletHookCookie *cookie, AppletHookFn callback, void *param)
{
    cookie->callback = callback;
    cookie->param = param;
    cookie->next = s_hooks;
    s_hooks = cookie;
}

bool appletMainLoop(void) { return true; }

AppletOperationMode appletGetOperationMode(void)
{
    const char *d = getenv("GENESIS_FAKE_DOCKED");
    return (d && d[0] == '1') ? AppletOperationMode_Console : AppletOperationMode_Handheld;
}

AppletFocusState appletGetFocusState(void)
{
    if (!s_home_parsed) {
        s_home_parsed = 1;
        const char *h = getenv("GENESIS_FAKE_HOME");
        if (h && sscanf(h, "%ld:%ld", &s_home_from, &s_home_len) != 2) s_home_from = -1;
    }
    long n = (long)s_focus_polls++;
    int focus = (s_home_from >= 0 && n >= s_home_from && n < s_home_from + s_home_len)
                    ? AppletFocusState_Background : AppletFocusState_InFocus;
    if (focus != AppletFocusState_InFocus) s_lost_polls++;
    if (focus != s_last_focus) { s_last_focus = focus; fire_hooks(AppletHookType_OnFocusState); }
    return (AppletFocusState)focus;
}

unsigned libnx_stub_focus_lost_polls(void) { return (unsigned)s_lost_polls; }

void svcSleepThread(s64 nano)
{
    /* Keep tests fast: sleep at most 1 ms. */
    struct timespec ts = { 0, nano > 1000000 ? 1000000 : (long)nano };
    nanosleep(&ts, NULL);
}

/* Applet trace (engine patch 0022). */
u64 armGetSystemTick(void) { return (u64)SDL_GetPerformanceCounter(); }
u64 armTicksToNs(u64 tick) { return (u64)((double)tick * 1e9 / (double)SDL_GetPerformanceFrequency()); }
Result svcGetInfo(u64 *out, u32 id0, Handle handle, u64 id1)
{
    (void)handle; (void)id1;
    *out = id0 == InfoType_TotalMemorySize ? 3200ull << 20 : 0;
    return 0;
}
int appletGetAppletType(void) { return 0; }
