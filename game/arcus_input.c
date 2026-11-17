/*
 * arcus_input.c — Arcus Odyssey controls (SPEC.md §6.1); see arcus_input.h.
 *
 * Backends fill one ArcusHostPad per player each frame:
 *   Switch (and the Mac Switch-stub build): libnx pad, P1 = Npad No1 or the
 *     handheld controls, P2 = Npad No2. Labels are unambiguous there.
 *   Mac: SDL GameController in plug order, read by position (Switch layout:
 *     SDL A = bottom = host B, SDL B = right = host A, SDL X = left = host Y),
 *     plus the keyboard for P1.
 *   Tests: ARCUS_INPUT_INJECT (below). In the Switch-stub build injected states
 *     go through the libnx stub, so the libnx read path is exercised as well.
 *
 * Injection file: one line per change, "<frame> <player 1|2> <tokens>" (frame =
 * runner iteration: the wall frame, except that it keeps counting while the
 * pause overlay holds the wall frame still),
 * the state holding until that player's next line. Tokens: button names
 * A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT; LS=x,y and RS=x,y (stick,
 * -1..1, y up); LSA=deg[,r] and RSA=deg[,r] (stick by angle, 0 = right,
 * 90 = up; r defaults to 1); "-" for a neutral pad. "#" starts a comment.
 * The state given for frame N is what the game samples during frame N.
 */
#include "arcus_input.h"
#include "arcus_host.h"
#include "arcus_magic.h"

#include "input_map.h"          /* g_input_map: pad type of the two ports */
#include "platform_switch.h"    /* GENESIS_PLATFORM_SWITCH */
#include "genesis_host_mem.h"   /* glue_peek8 (trace only) */

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if GENESIS_PLATFORM_SWITCH
#include <switch.h>
#endif

#define ARCUS_PLAYERS 2

static ArcusHostPad  s_pad[ARCUS_PLAYERS];
static ArcusAutofire s_af[ARCUS_PLAYERS];
static uint16_t      s_mask[ARCUS_PLAYERS];
static uint32_t      s_suppress[ARCUS_PLAYERS];   /* held when the overlay / quick magic closed */
static int           s_quick_was[ARCUS_PLAYERS];  /* quick magic owned the controls last frame */
static int           s_window_was;                /* the item window was open last frame */
static uint32_t      s_nav_edges, s_nav_prev;
static int           s_nav_repeat_frames;
static int           s_overlay_was_open;
static uint64_t      s_frame;
/* Test clock for injection and trace: one tick per runner iteration. It equals
 * the wall frame until the pause overlay holds the frame number still (the
 * overlay pauses the game but keeps sampling the pads). */
static uint64_t      s_clock;
static int           s_clock_set;
static int           s_inited;

/* ---- mapping ------------------------------------------------------------------------- */

static int autofire_period(int period)
{
    if (period < 3 || period > 60) period = 10;   /* never hold C (holding C charges) */
    return period;
}

uint16_t arcus_input_map(const ArcusHostPad *p, ArcusAutofire *af, int period)
{
    const float t = ARCUS_STICK_THRESHOLD;
    uint32_t b = p->buttons;
    uint16_t m = 0;

    if ((b & ARCUS_HB_UP)    || p->ly >  t) m |= ARCUS_GP_UP;
    if ((b & ARCUS_HB_DOWN)  || p->ly < -t) m |= ARCUS_GP_DOWN;
    if ((b & ARCUS_HB_LEFT)  || p->lx < -t) m |= ARCUS_GP_LEFT;
    if ((b & ARCUS_HB_RIGHT) || p->lx >  t) m |= ARCUS_GP_RIGHT;

    if (p->ry >  t) m |= ARCUS_GP_Z;      /* aim up    */
    if (p->ry < -t) m |= ARCUS_GP_Y;      /* aim down  */
    if (p->rx < -t) m |= ARCUS_GP_X;      /* aim left  */
    if (p->rx >  t) m |= ARCUS_GP_MODE;   /* aim right */

    if (b & ARCUS_HB_A)                  m |= ARCUS_GP_C;
    if (b & (ARCUS_HB_B | ARCUS_HB_ZL))  m |= ARCUS_GP_B;
    if (b & ARCUS_HB_Y)                  m |= ARCUS_GP_A;
    if (b & ARCUS_HB_PLUS)               m |= ARCUS_GP_START;

    /* ZR autofire: C for 2 frames, released for period-2, from the ZR-down frame. */
    if (b & ARCUS_HB_ZR) {
        if (!af->held) { af->held = 1; af->phase = 0; }
        if (af->phase % (uint32_t)autofire_period(period) < 2) m |= ARCUS_GP_C;
        af->phase++;
    } else {
        af->held = 0;
        af->phase = 0;
    }
    return m;
}

/* ---- test injection ------------------------------------------------------------------ */

typedef struct InjectEvent { uint64_t frame; int player; ArcusHostPad pad; } InjectEvent;
static InjectEvent *s_inj;
static size_t s_inj_n, s_inj_next;
static ArcusHostPad s_inj_pad[ARCUS_PLAYERS];
static int s_inj_active;

static int parse_pair(const char *v, float *a, float *b)
{
    return sscanf(v, "%f,%f", a, b) == 2;
}

static void inject_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[arcus_input] cannot open %s\n", path); return; }
    char line[512];
    size_t cap = 0;
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *save = NULL;
        char *tok = strtok_r(line, " \t\r\n", &save);
        if (!tok) continue;
        InjectEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.frame = strtoull(tok, NULL, 10);
        tok = strtok_r(NULL, " \t\r\n", &save);
        ev.player = tok ? atoi(tok) - 1 : -1;
        if (ev.player < 0 || ev.player >= ARCUS_PLAYERS) {
            fprintf(stderr, "[arcus_input] %s:%d: bad player\n", path, lineno);
            continue;
        }
        ev.pad.connected = 1;
        static const struct { const char *name; uint32_t bit; } names[] = {
            {"A", ARCUS_HB_A}, {"B", ARCUS_HB_B}, {"X", ARCUS_HB_X}, {"Y", ARCUS_HB_Y},
            {"L", ARCUS_HB_L}, {"R", ARCUS_HB_R}, {"ZL", ARCUS_HB_ZL}, {"ZR", ARCUS_HB_ZR},
            {"PLUS", ARCUS_HB_PLUS}, {"MINUS", ARCUS_HB_MINUS}, {"LSTICK", ARCUS_HB_LSTICK}, {"RSTICK", ARCUS_HB_RSTICK}, {"UP", ARCUS_HB_UP},
            {"DOWN", ARCUS_HB_DOWN}, {"LEFT", ARCUS_HB_LEFT}, {"RIGHT", ARCUS_HB_RIGHT},
        };
        while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
            int ok = 0;
            if (strcmp(tok, "-") == 0) ok = 1;
            for (size_t i = 0; !ok && i < sizeof names / sizeof names[0]; i++)
                if (strcmp(tok, names[i].name) == 0) { ev.pad.buttons |= names[i].bit; ok = 1; }
            if (!ok && (strncmp(tok, "LS=", 3) == 0 || strncmp(tok, "RS=", 3) == 0)) {
                float *x = tok[0] == 'L' ? &ev.pad.lx : &ev.pad.rx;
                float *y = tok[0] == 'L' ? &ev.pad.ly : &ev.pad.ry;
                ok = parse_pair(tok + 3, x, y);
            }
            if (!ok && (strncmp(tok, "LSA=", 4) == 0 || strncmp(tok, "RSA=", 4) == 0)) {
                float deg = 0, r = 1;
                int n = sscanf(tok + 4, "%f,%f", &deg, &r);
                if (n >= 1) {
                    double a = deg * 3.14159265358979323846 / 180.0;
                    float x = (float)(r * cos(a)), y = (float)(r * sin(a));
                    if (tok[0] == 'L') { ev.pad.lx = x; ev.pad.ly = y; }
                    else               { ev.pad.rx = x; ev.pad.ry = y; }
                    ok = 1;
                }
            }
            if (!ok) fprintf(stderr, "[arcus_input] %s:%d: unknown token %s\n", path, lineno, tok);
        }
        if (s_inj_n == cap) {
            cap = cap ? cap * 2 : 256;
            InjectEvent *n = (InjectEvent *)realloc(s_inj, cap * sizeof *n);
            if (!n) break;
            s_inj = n;
        }
        s_inj[s_inj_n++] = ev;
    }
    fclose(f);
    s_inj_active = 1;
    fprintf(stderr, "[arcus_input] injection: %zu events from %s\n", s_inj_n, path);
}

static void inject_advance(uint64_t frame)
{
    while (s_inj_next < s_inj_n && s_inj[s_inj_next].frame <= frame) {
        const InjectEvent *e = &s_inj[s_inj_next++];
        s_inj_pad[e->player] = e->pad;
    }
}

/* ---- backends --------------------------------------------------------------------------- */

#if GENESIS_PLATFORM_SWITCH

static PadState s_npad[ARCUS_PLAYERS];

static void backend_init(void)
{
    padConfigureInput(ARCUS_PLAYERS, HidNpadStyleSet_NpadStandard);
    padInitialize(&s_npad[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
    padInitialize(&s_npad[1], HidNpadIdType_No2);
}

static float stick_unit(s32 v)
{
    float f = (float)v / (float)JOYSTICK_MAX;
    return f > 1.f ? 1.f : f < -1.f ? -1.f : f;
}

#if defined(GENESIS_SWITCH_STUBS)
static s32 stick_raw(float v) { return (s32)lrintf(v * (float)JOYSTICK_MAX); }
static u64 hb_to_npad(uint32_t b)
{
    u64 r = 0;
    if (b & ARCUS_HB_A) r |= HidNpadButton_A;         if (b & ARCUS_HB_B) r |= HidNpadButton_B;
    if (b & ARCUS_HB_X) r |= HidNpadButton_X;         if (b & ARCUS_HB_Y) r |= HidNpadButton_Y;
    if (b & ARCUS_HB_L) r |= HidNpadButton_L;         if (b & ARCUS_HB_R) r |= HidNpadButton_R;
    if (b & ARCUS_HB_ZL) r |= HidNpadButton_ZL;       if (b & ARCUS_HB_ZR) r |= HidNpadButton_ZR;
    if (b & ARCUS_HB_PLUS) r |= HidNpadButton_Plus;   if (b & ARCUS_HB_MINUS) r |= HidNpadButton_Minus;
    if (b & ARCUS_HB_UP) r |= HidNpadButton_Up;       if (b & ARCUS_HB_DOWN) r |= HidNpadButton_Down;
    if (b & ARCUS_HB_LEFT) r |= HidNpadButton_Left;   if (b & ARCUS_HB_RIGHT) r |= HidNpadButton_Right;
    if (b & ARCUS_HB_LSTICK) r |= HidNpadButton_StickL;
    if (b & ARCUS_HB_RSTICK) r |= HidNpadButton_StickR;
    return r;
}
#endif

static void backend_poll(void)
{
#if defined(GENESIS_SWITCH_STUBS)
    /* Stub build: injected states become raw Npad state, read back via libnx. */
    for (int p = 0; p < ARCUS_PLAYERS; p++) {
        const ArcusHostPad *q = &s_inj_pad[p];
        libnx_stub_pad_set(p, hb_to_npad(q->buttons), stick_raw(q->lx), stick_raw(q->ly),
                           stick_raw(q->rx), stick_raw(q->ry), q->connected);
    }
#endif
    static const struct { u64 npad; uint32_t hb; } map[] = {
        {HidNpadButton_A, ARCUS_HB_A}, {HidNpadButton_B, ARCUS_HB_B},
        {HidNpadButton_X, ARCUS_HB_X}, {HidNpadButton_Y, ARCUS_HB_Y},
        {HidNpadButton_L, ARCUS_HB_L}, {HidNpadButton_R, ARCUS_HB_R},
        {HidNpadButton_ZL, ARCUS_HB_ZL}, {HidNpadButton_ZR, ARCUS_HB_ZR},
        {HidNpadButton_Plus, ARCUS_HB_PLUS}, {HidNpadButton_Minus, ARCUS_HB_MINUS},
        {HidNpadButton_Up, ARCUS_HB_UP}, {HidNpadButton_Down, ARCUS_HB_DOWN},
        {HidNpadButton_Left, ARCUS_HB_LEFT}, {HidNpadButton_Right, ARCUS_HB_RIGHT},
        {HidNpadButton_StickL, ARCUS_HB_LSTICK}, {HidNpadButton_StickR, ARCUS_HB_RSTICK},
    };
    for (int p = 0; p < ARCUS_PLAYERS; p++) {
        PadState *n = &s_npad[p];
        padUpdate(n);
        ArcusHostPad *h = &s_pad[p];
        memset(h, 0, sizeof *h);
        h->connected = padIsConnected(n);
        u64 b = padGetButtons(n);
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (b & map[i].npad) h->buttons |= map[i].hb;
        HidAnalogStickState l = padGetStickPos(n, 0), r = padGetStickPos(n, 1);
        h->lx = stick_unit(l.x); h->ly = stick_unit(l.y);
        h->rx = stick_unit(r.x); h->ry = stick_unit(r.y);
    }
}

#else /* SDL GameController + keyboard */

static SDL_GameController *s_ctl[ARCUS_PLAYERS];

static void backend_init(void)
{
    /* Positional face buttons on Nintendo pads too (SDL A = bottom). */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
}

static void sdl_refresh_controllers(void)
{
    for (int p = 0; p < ARCUS_PLAYERS; p++)
        if (s_ctl[p] && !SDL_GameControllerGetAttached(s_ctl[p])) {
            SDL_GameControllerClose(s_ctl[p]);
            s_ctl[p] = NULL;
        }
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        int used = 0, slot = -1;
        for (int p = 0; p < ARCUS_PLAYERS; p++) {
            if (s_ctl[p] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_ctl[p])) == id)
                used = 1;
            if (!s_ctl[p] && slot < 0) slot = p;
        }
        if (!used && slot >= 0) s_ctl[slot] = SDL_GameControllerOpen(i);
    }
}

static float axis_unit(SDL_GameController *c, SDL_GameControllerAxis a)
{
    int v = SDL_GameControllerGetAxis(c, a);
    return v < 0 ? v / 32768.f : v / 32767.f;
}

static void backend_poll(void)
{
    static int s_last_count = -1;
    int count = SDL_NumJoysticks();
    if (count != s_last_count) { s_last_count = count; sdl_refresh_controllers(); }

    for (int p = 0; p < ARCUS_PLAYERS; p++) {
        ArcusHostPad *h = &s_pad[p];
        memset(h, 0, sizeof *h);
        SDL_GameController *c = s_ctl[p];
        if (!c) continue;
        h->connected = 1;
        static const struct { SDL_GameControllerButton b; uint32_t hb; } map[] = {
            {SDL_CONTROLLER_BUTTON_A, ARCUS_HB_B},   /* bottom */
            {SDL_CONTROLLER_BUTTON_B, ARCUS_HB_A},   /* right  */
            {SDL_CONTROLLER_BUTTON_X, ARCUS_HB_Y},   /* left   */
            {SDL_CONTROLLER_BUTTON_Y, ARCUS_HB_X},   /* top    */
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ARCUS_HB_L},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ARCUS_HB_R},
            {SDL_CONTROLLER_BUTTON_START, ARCUS_HB_PLUS},
            {SDL_CONTROLLER_BUTTON_BACK, ARCUS_HB_MINUS},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, ARCUS_HB_UP},
            {SDL_CONTROLLER_BUTTON_DPAD_DOWN, ARCUS_HB_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, ARCUS_HB_LEFT},
            {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ARCUS_HB_RIGHT},
            {SDL_CONTROLLER_BUTTON_LEFTSTICK, ARCUS_HB_LSTICK},
            {SDL_CONTROLLER_BUTTON_RIGHTSTICK, ARCUS_HB_RSTICK},
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (SDL_GameControllerGetButton(c, map[i].b)) h->buttons |= map[i].hb;
        if (axis_unit(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 0.5f) h->buttons |= ARCUS_HB_ZL;
        if (axis_unit(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f) h->buttons |= ARCUS_HB_ZR;
        h->lx =  axis_unit(c, SDL_CONTROLLER_AXIS_LEFTX);
        h->ly = -axis_unit(c, SDL_CONTROLLER_AXIS_LEFTY);
        h->rx =  axis_unit(c, SDL_CONTROLLER_AXIS_RIGHTX);
        h->ry = -axis_unit(c, SDL_CONTROLLER_AXIS_RIGHTY);
    }

    /* Keyboard for P1: arrows move (no aim keys: facing follows movement), Z/X/C = Genesis A/B/C (host Y/B/A),
     * S = quick magic (host X), V = autofire, Enter = Start, F1 = overlay (Escape too, in
     * nav_held), Q/W = quick save/load (host L/R), Backspace = rewind (host L3),
     * M = mini map (host R3). */
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (ks) {
        ArcusHostPad *h = &s_pad[0];
        static const struct { SDL_Scancode sc; uint32_t hb; } keys[] = {
            {SDL_SCANCODE_UP, ARCUS_HB_UP}, {SDL_SCANCODE_DOWN, ARCUS_HB_DOWN},
            {SDL_SCANCODE_LEFT, ARCUS_HB_LEFT}, {SDL_SCANCODE_RIGHT, ARCUS_HB_RIGHT},
            {SDL_SCANCODE_Z, ARCUS_HB_Y}, {SDL_SCANCODE_X, ARCUS_HB_B},
            {SDL_SCANCODE_C, ARCUS_HB_A}, {SDL_SCANCODE_S, ARCUS_HB_X},
            {SDL_SCANCODE_V, ARCUS_HB_ZR},
            {SDL_SCANCODE_RETURN, ARCUS_HB_PLUS}, {SDL_SCANCODE_F1, ARCUS_HB_MINUS},
            {SDL_SCANCODE_Q, ARCUS_HB_L}, {SDL_SCANCODE_W, ARCUS_HB_R},
            {SDL_SCANCODE_BACKSPACE, ARCUS_HB_LSTICK}, {SDL_SCANCODE_M, ARCUS_HB_RSTICK},
        };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            if (ks[keys[i].sc]) h->buttons |= keys[i].hb;
    }

    /* Injected test states add to the live controls. */
    if (s_inj_active)
        for (int p = 0; p < ARCUS_PLAYERS; p++) {
            const ArcusHostPad *q = &s_inj_pad[p];
            ArcusHostPad *h = &s_pad[p];
            h->buttons |= q->buttons;
            if (q->lx != 0.f || q->ly != 0.f) { h->lx = q->lx; h->ly = q->ly; }
            if (q->rx != 0.f || q->ry != 0.f) { h->rx = q->rx; h->ry = q->ry; }
            h->connected |= q->connected;
        }
}

#endif /* backends */

/* ---- navigation edges ------------------------------------------------------------------ */

static uint32_t nav_held(void)
{
    const float t = ARCUS_STICK_THRESHOLD;
    uint32_t n = 0;
    for (int p = 0; p < ARCUS_PLAYERS; p++) {
        const ArcusHostPad *h = &s_pad[p];
        if ((h->buttons & ARCUS_HB_UP)    || h->ly >  t) n |= ARCUS_NAV_UP;
        if ((h->buttons & ARCUS_HB_DOWN)  || h->ly < -t) n |= ARCUS_NAV_DOWN;
        if ((h->buttons & ARCUS_HB_LEFT)  || h->lx < -t) n |= ARCUS_NAV_LEFT;
        if ((h->buttons & ARCUS_HB_RIGHT) || h->lx >  t) n |= ARCUS_NAV_RIGHT;
        if (h->buttons & ARCUS_HB_A)     n |= ARCUS_NAV_OK;
        if (h->buttons & ARCUS_HB_B)     n |= ARCUS_NAV_BACK;
        if (h->buttons & ARCUS_HB_MINUS) n |= ARCUS_NAV_MENU;
    }
#if !GENESIS_PLATFORM_SWITCH
    /* Keyboard: the arrows/F1 already arrive through the P1 host pad; Enter is OK only
     * here (it is also Start for the game). Escape opens the menu, and inside it is Back
     * (which closes it from the first page); what a press means is fixed when it goes
     * down, so holding it doesn't open and then close the menu. */
    static uint32_t s_esc;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (ks) {
        if (ks[SDL_SCANCODE_RETURN]) n |= ARCUS_NAV_OK;
        if (!ks[SDL_SCANCODE_ESCAPE]) s_esc = 0;
        else if (!s_esc) s_esc = arcus_overlay_open() ? ARCUS_NAV_BACK : ARCUS_NAV_MENU;
        n |= s_esc;
    }
#endif
    return n;
}

static void nav_update(void)
{
    const uint32_t dirs = ARCUS_NAV_UP | ARCUS_NAV_DOWN | ARCUS_NAV_LEFT | ARCUS_NAV_RIGHT;
    uint32_t held = nav_held();
    uint32_t edges = held & ~s_nav_prev;
    /* Held directions repeat: after 20 frames, every 6 frames. */
    if ((held & dirs) && (held & dirs) == (s_nav_prev & dirs)) {
        if (++s_nav_repeat_frames >= 20 && (s_nav_repeat_frames - 20) % 6 == 0)
            edges |= held & dirs;
    } else {
        s_nav_repeat_frames = 0;
    }
    s_nav_prev = held;
    s_nav_edges = edges;
}

uint32_t arcus_input_nav_edges(void) { return s_nav_edges; }
uint64_t arcus_input_frame_number(void) { return s_frame; }

/* ---- per-frame driver ------------------------------------------------------------------ */

static FILE *s_trace;
static int s_test_period;      /* ARCUS_INPUT_AUTOFIRE_PERIOD (tests), 0 = settings */
static int s_test_pad3;        /* ARCUS_INPUT_PAD_TYPE=3 (tests): 3-button ports */
static uint32_t s_trace_addr[32];
static int s_trace_naddr;
static uint64_t s_trace_from;

static void init_once(void)
{
    if (s_inited) return;
    s_inited = 1;
    const char *inj = getenv("ARCUS_INPUT_INJECT");
    if (inj && inj[0]) inject_load(inj);
    const char *tr = getenv("ARCUS_INPUT_TRACE");
    if (tr && tr[0]) {
        s_trace = fopen(tr, "w");
        const char *a = getenv("ARCUS_INPUT_TRACE_ADDRS");
        while (a && *a && s_trace_naddr < 32) {
            char *end;
            unsigned long v = strtoul(a, &end, 16);
            if (end == a) break;
            s_trace_addr[s_trace_naddr++] = (uint32_t)v;
            a = (*end == ',') ? end + 1 : end;
        }
        const char *from = getenv("ARCUS_INPUT_TRACE_FROM");
        if (from) s_trace_from = strtoull(from, NULL, 10);
    }
    const char *per = getenv("ARCUS_INPUT_AUTOFIRE_PERIOD");
    if (per) s_test_period = atoi(per);
    const char *pt = getenv("ARCUS_INPUT_PAD_TYPE");
    s_test_pad3 = pt && atoi(pt) == 3;
    backend_init();
}

static void sample(uint64_t wall_frame)
{
    init_once();
    s_frame = wall_frame;
    if (s_inj_active) inject_advance(s_clock);
    backend_poll();
    nav_update();
}

void arcus_input_poll(void)
{
    sample(s_frame);
}

void arcus_input_frame(uint64_t wall_frame)
{
    s_clock = s_clock_set ? s_clock + 1 : wall_frame;
    s_clock_set = 1;
    sample(wall_frame);

    if (s_test_pad3)
        g_input_map.p[0].pad_type = g_input_map.p[1].pad_type = PAD_3BUTTON;

    int open = arcus_overlay_open();
    /* Item window toggle (always on; the user, 2026-11-12: no setting): while the game's item window
     * is open, the item button closes it and attack chooses, so one button opens and closes
     * it. The window reads Genesis A as choose and B / C as close ($1C440, $1C590), so A and
     * C swap, and ZR autofire adds nothing (its C pulses would close it). A face button held
     * when the window opens or closes stays dead until released: the item button that opened
     * it would otherwise close it at once, and the one that closed it reopen it. */
    int window = !open && arcus_magic_window_open();
    int window_edge = !open && window != s_window_was;
    s_window_was = window;
    for (int p = 0; p < ARCUS_PLAYERS; p++) {
        if (open) {
            /* The overlay owns the controls: no game input, autofire restarts. */
            s_mask[p] = 0;
            s_af[p].held = 0;
            s_af[p].phase = 0;
            arcus_magic_input(p, NULL);    /* a quick magic hold ends without a pick */
            s_quick_was[p] = 0;
            continue;
        }
        if (s_overlay_was_open)            /* buttons that closed the overlay */
            s_suppress[p] = s_pad[p].buttons;
        s_suppress[p] &= s_pad[p].buttons; /* ...stay dead until released */
        ArcusHostPad pad = s_pad[p];
        pad.buttons &= ~s_suppress[p];
        /* Quick magic (hold X) owns the player's controls while its list is up; the buttons
         * still down when it closes (B that cancelled, a direction) stay dead until released. */
        if (arcus_magic_input(p, &pad)) {
            s_mask[p] = 0;
            s_af[p].held = 0;
            s_af[p].phase = 0;
            s_quick_was[p] = 1;
            continue;
        }
        if (s_quick_was[p]) {
            s_quick_was[p] = 0;
            s_suppress[p] |= s_pad[p].buttons;
            pad.buttons &= ~s_suppress[p];
        }
        if (window_edge) {
            s_suppress[p] |= s_pad[p].buttons & (ARCUS_HB_A | ARCUS_HB_Y | ARCUS_HB_ZR);
            pad.buttons &= ~s_suppress[p];
        }
        s_mask[p] = arcus_input_map(&pad, &s_af[p],
                                    s_test_period ? s_test_period : g_arcus_settings.autofire_period);
        if (window) {
            s_mask[p] &= (uint16_t)~(ARCUS_GP_A | ARCUS_GP_C);
            if (pad.buttons & ARCUS_HB_A) s_mask[p] |= ARCUS_GP_A;   /* attack chooses */
            if (pad.buttons & ARCUS_HB_Y) s_mask[p] |= ARCUS_GP_C;   /* item button closes */
        }
    }
    s_overlay_was_open = open;

    if (s_trace && s_clock >= s_trace_from) {
        fprintf(s_trace, "%llu %04X %04X %02X %d", (unsigned long long)s_clock,
                s_mask[0], s_mask[1], s_nav_edges, open);
        for (int i = 0; i < s_trace_naddr; i++)
            fprintf(s_trace, " %02X", glue_peek8(s_trace_addr[i]));
        fputc('\n', s_trace);
        fflush(s_trace);
    }
}

/* Arcus reads both ports as 6-button pads (X/Y/Z/Mode = aim), while the engine
 * defaults to 3-button. Make 6-button the default for both ports unless
 * settings.ini sets pad_type in [input.p1] / [input.p2]. */
void arcus_input_load_settings(const char *settings_path)
{
    int set[ARCUS_PLAYERS] = {0};
    FILE *f = settings_path ? fopen(settings_path, "r") : NULL;
    if (f) {
        char line[256];
        int section = -1;
        while (fgets(line, sizeof line, f)) {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            int p;
            if (*s == '[') {
                section = (sscanf(s, "[input.p%d]", &p) == 1 && p >= 1 && p <= ARCUS_PLAYERS) ? p - 1 : -1;
                continue;
            }
            if (section >= 0 && strncmp(s, "pad_type", 8) == 0 && strchr(s, '='))
                set[section] = 1;
        }
        fclose(f);
    }
    for (int p = 0; p < ARCUS_PLAYERS; p++)
        if (!set[p]) g_input_map.p[p].pad_type = PAD_6BUTTON;
}

/* Raw host buttons (ARCUS_HB_*) of the latest sample; the overlay reads the
 * ZL+ZR rewind chord here (track C). */
uint32_t arcus_input_host_buttons(int player)
{
    return (player >= 0 && player < ARCUS_PLAYERS) ? s_pad[player].buttons : 0;
}

/* The strongest of four stick axes (both sticks of both players). The magnitude is kept in
 * its own variable: written as "fabsf(v) > fabsf(best)", Apple clang 17 -O3 dropped the
 * fabs on best, so a negative left-stick value (down / left) lost to the right stick's 0. */
static float strongest(const float v[4])
{
    float best = 0.f, mag = 0.f;
    for (int i = 0; i < 4; i++) {
        float a = fabsf(v[i]);
        if (a > mag) { mag = a; best = v[i]; }
    }
    return best;
}

/* The strongest vertical deflection of any stick of either player (-1..1, up > 0): the
 * extras viewer's scrolling. */
float arcus_input_stick_y(void)
{
    const float v[4] = { s_pad[0].ly, s_pad[0].ry, s_pad[1].ly, s_pad[1].ry };
    return strongest(v);
}

float arcus_input_stick_x(void)
{
    const float v[4] = { s_pad[0].lx, s_pad[0].rx, s_pad[1].lx, s_pad[1].rx };
    return strongest(v);
}

int arcus_input_sticks(int player, float v[4])
{
    if (player < 0 || player >= ARCUS_PLAYERS) return 0;
    const ArcusHostPad *h = &s_pad[player];
    v[0] = h->lx; v[1] = h->ly; v[2] = h->rx; v[3] = h->ry;
    if (fabsf(h->lx) < 0.2f && fabsf(h->ly) < 0.2f) {
        float x = (float)(!!(h->buttons & ARCUS_HB_RIGHT) - !!(h->buttons & ARCUS_HB_LEFT));
        float y = (float)(!!(h->buttons & ARCUS_HB_UP) - !!(h->buttons & ARCUS_HB_DOWN));
        if (x && y) { x *= 0.7071f; y *= 0.7071f; }
        if (x || y) { v[0] = x; v[1] = y; }
    }
    return h->connected;
}

uint16_t arcus_input_mask(int player)
{
    return (player >= 0 && player < ARCUS_PLAYERS) ? s_mask[player] : 0;
}
