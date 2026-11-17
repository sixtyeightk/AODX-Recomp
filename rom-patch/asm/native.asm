; Arcus Odyssey (USA) — 424-px native widescreen: camera map-edge stops and co-op screen bounds
;
; Built on top of widescreen.asm by `tools/build.py --native` -> build/arcus-native.bin.
;
; Camera: $8100 = map x of the screen's left edge - 16 (the drawn window starts one metatile
; left of the screen), wrapped into [0, W) with W = map width in px ($FF0C00 metatiles * 16).
; The original stops it at -16 (left; stored as W-16) and W-336 (right), so a 320-px screen
; ends exactly at the map edges. A 424-px view shows 52 px more on each side, so the native
; build stops it at CAM_MIN = 36 and W-388: the view [cam+16-52, cam+16+372) == [0, W).
; Players are in screen coordinates (x = screen x + 128) and their own move limits are
; $83/$1A5 (screen 3..293); the native build allows $4F/$1D9 (screen -49..345), so co-op
; players can use the full 424 px and the limits meet the map edges at the camera stops.
;
; NATIVE_MODE ($FFFA08, RAM the game never uses; cleared at power-on): 0 = 424-px behaviour
; (default), non-zero = every hook behaves exactly like the original game (for a 4:3 option:
; the host writes it, e.g. every frame, since the game clears RAM at boot and after game over).
;
;   CamInit         area load ($79F2, after the map is decompressed): clamp the start camera
;                   of the entry point into [48, W-400] and move both players by the same
;                   amount, so they keep their map position. The shift is a multiple of 16:
;                   the start draw puts map column cam>>4 at VRAM column 0 with hscroll 0, i.e.
;                   it needs cam & 15 == 0 (all entry points are metatile-aligned; scrolling
;                   then keeps (cam + $8110) & 15 == 0). Walking can still push the camera the
;                   last 12 px to CAM_MIN / W-388.
;   CamLeftStop     scrolling left  ($F33C): stop at CAM_MIN instead of -16.
;   CamRightStop    scrolling right ($F3D0): stop at W-388 instead of W-336.
;   OwnLeft/RightLimit   own move limits ($DF24 / $E030).
;   PushLeft/RightLimit  how far the camera may push the other player ($F3FA / $F360).
;   AutoScrollWrap  the scripted auto-scroll ($829E, $B718; area 4, the Act 2 bridge) moves the
;                   camera left 6 px per frame through a looping map (period (W-32)*16; the last
;                   32 metatiles repeat the first 32). It wrapped at 0; it now wraps below
;                   CAM_MIN so the 424 view stays inside the repeated columns.
;
; Camera-locked arenas (LockedAreas) keep the original start camera and player limits: $19 $1A
; $33 $34 are never scrolled horizontally ($F304), and $12 $13 $17 $20 $33 load with the camera
; lock $808A set ($7B5A; boss rooms). Their view is static and centred on the arena; the side
; strips only show what lies beyond the arena walls (void, or wrapped scenery in $19 $1A $20).
;
; Vertical ring (no code here: constant edits in tools/build.py HUD_ROW_PATCHES). The planes are
; 64x32 tiles = 16 metatile rows (256 px) and vscroll = $8112 + 16, so with R0 = $8102>>4 and
; k = $8112>>4 the 224-px view shows map rows R0+1..R0+15 (VRAM rows k+1..k+15 mod 16). The
; game kept only 15 rows drawn ([R0, R0+14] after scrolling down; R0+15 was the stale R0-1),
; which was enough for the 192-px playfield (rows up to R0+13) but not for the 424 view, which
; shows the map beside the HUD (screen y 192..223): its bottom row popped in late. Now all 16
; slots are kept = [R0, R0+15]: scrolling down draws R0+15 (was R0+14) into VRAM row k+15
; (= the slot of the invisible row R0-1), the column drawers draw 16 metatiles (was 15) and the
; level start fills 16 rows (was 15). Scrolling up already drew R0 (invisible margin row) into
; slot k, which keeps [R0, R0+15]. Nothing visible in the 320x192 playfield changes (slot k+15
; is at screen y >= 208), and the same holds for the half-speed plane ($8106/$8116 >> 1).

NATIVE_MODE     equ     $FFFFFA08               ; 0 = 424-px view, else original behaviour
CAM_MIN         equ     36                      ; 52 - 16: view left edge (cam + 16 - 52) at map x 0
CAM_RIGHT       equ     388                     ; W - CAM_RIGHT = rightmost camera (16 + 372)
INIT_MIN        equ     48                      ; CAM_MIN rounded up to a metatile
INIT_RIGHT      equ     400                     ; CAM_RIGHT rounded up to a metatile
WIDE_MIN_W      equ     424                     ; maps narrower than the view are left alone
LIMIT_LEFT      equ     $83                     ; original player limits (x = screen x + 128)
LIMIT_RIGHT     equ     $1A5
WIDE_LEFT       equ     LIMIT_LEFT-52           ; $4F
WIDE_RIGHT      equ     LIMIT_RIGHT+52          ; $1D9
MAP_HDR         equ     $FFFF0C00               ; map width (metatiles) at +0, height at +2
CAM_X           equ     $FFFF8100
AREA            equ     $FFFF828E
NO_RESPAWN      equ     $FFFFFA13               ; host option (written every frame): killed enemies stay dead
ONCE_LIST       equ     $FFFF8400               ; spawn ids the spawner skips ($11D28), count ONCE_N
ONCE_N          equ     $FFFF813C
ONCE_MAX        equ     $F0                     ; the list ends at $84FF (the enemy table starts at $8500)
VDP_CTRL        equ     $C00004
CLAMPED         equ     $FFFF8292               ; set at every area load: map edges stop the camera
P1_X            equ     $FFFF8118               ; player x, and the copies the loader also sets
P2_X            equ     $FFFF811A
P1_X_PREV       equ     $FFFF8128
P2_X_PREV       equ     $FFFF812A

        org     $100600

; Fixed entry points: the ROM call sites (tools/build.py NATIVE_PATCHES) target these.
        jmp     (CamInit).l                     ; $100600  jsr from $79F2
        jmp     (CamLeftStop).l                 ; $100606  jsr from $F33C
        jmp     (CamRightStop).l                ; $10060C  jsr from $F3D0
        jmp     (AutoScrollWrap).l              ; $100612  jsr from $B734
        jmp     (OwnLeftLimit).l                ; $100618  jsr from $DF24
        jmp     (OwnRightLimit).l               ; $10061E  jsr from $E030
        jmp     (PushLeftLimit).l               ; $100624  jsr from $F3FA
        jmp     (PushRightLimit).l              ; $10062A  jsr from $F360
        jmp     (AimRelease).l                  ; $100630  jsr from $10008E (twin-stick AimHook)
        jmp     (Unused).l                      ; $100636  (was the title menu's config line)
        jmp     (Unused).l                      ; $10063C  (kept so later entries stay put)
        jmp     (Unused).l                      ; $100642
        jmp     (OnceTest).l                    ; $100648  jsr from $13D5A (enemy removal)

; ---------------------------------------------------------------------------------------------
; Replaces "move.w (a0)+,d0 / move.w d0,$8100.w" at $79F2 (a0 -> entry point: cam x, cam y,
; P1 x/y, P2 x/y). The caller derives $8108/$8104/$810C from d0, so d0 must hold the result.
CamInit:
        move.w  (a0)+,d0
        movem.l d1-d2,-(sp)
        bsr     WideHere
        bne.s   .done
        move.w  (MAP_HDR).l,d1
        asl.w   #4,d1                           ; d1 = W (px)
        cmpi.w  #WIDE_MIN_W,d1
        bcs.s   .done
        move.w  d1,d2
        subi.w  #16,d2
        cmp.w   d2,d0                           ; [W-16, W) is the original's -16..-1
        bcs.s   .signed
        sub.w   d1,d0
.signed:
        cmpi.w  #INIT_MIN,d0
        bge.s   .right
        move.w  #INIT_MIN,d2
        sub.w   d0,d2                           ; d2 = shift (> 0)
        move.w  #INIT_MIN,d0
        bra.s   .shift
.right:
        subi.w  #INIT_RIGHT,d1                  ; d1 = rightmost start camera
        cmp.w   d1,d0
        ble.s   .done
        move.w  d1,d2
        sub.w   d0,d2                           ; d2 = shift (< 0)
        move.w  d1,d0
.shift:
        sub.w   d2,(P1_X).w                     ; players are in screen coordinates
        sub.w   d2,(P1_X_PREV).w
        sub.w   d2,(P2_X).w
        sub.w   d2,(P2_X_PREV).w
.done:
        movem.l (sp)+,d1-d2
        move.w  d0,(CAM_X).w
        rts

; ---------------------------------------------------------------------------------------------
; Scrolling left. Replaces the clamp test at $F33C ("tst.b $8292 / ... / bcs.w $F3BC").
; In: d2 = camera x, d7.w = step, a0 -> map header. Out: carry set = stop. d1 is free here.
CamLeftStop:
        tst.b   (CLAMPED).w
        beq.s   .go
        move.w  (a0),d1
        subq.w  #1,d1
        asl.w   #4,d1                           ; d1 = W-16, the original stop
        tst.b   (NATIVE_MODE).w
        bne.s   .original
        cmp.w   d1,d2                           ; already in W-16..W-1 (left of the 424 stop)
        bcc.s   .stop
        move.w  d2,d1
        sub.w   d7,d1                           ; camera after this step
        bcs.s   .stop
        cmpi.w  #CAM_MIN,d1                     ; carry set if it would pass CAM_MIN
        rts
.original:                                      ; stop if d2 in [W-16, W-16+step)
        cmp.w   d1,d2
        bcs.s   .go
        add.w   d7,d1
        cmp.w   d1,d2
        rts                                     ; carry = d2 < W-16+step
.stop:
        ori     #1,ccr
        rts
.go:
        andi    #$FE,ccr
        rts

; ---------------------------------------------------------------------------------------------
; Scrolling right. Replaces $F3D0-$F3E7 ("tst.b $8292 / ... / bcc.w $F456"); the call site
; then reloads d1 = W at $F3E8 as the original did. Out: carry set = stop.
CamRightStop:
        tst.b   (CLAMPED).w
        beq.s   .go
        move.w  (a0),d1
        asl.w   #4,d1
        subi.w  #16,d1
        cmp.w   d1,d2                           ; in W-16..W-1 (left of the map): scroll
        bcc.s   .go
        subi.w  #CAM_RIGHT-16,d1
        tst.b   (NATIVE_MODE).w
        beq.s   .test
        addi.w  #CAM_RIGHT-336,d1               ; original: W-336
.test:
        sub.w   d7,d1
        cmp.w   d1,d2
        bcc.s   .stop                           ; d2 + step > rightmost camera
.go:
        andi    #$FE,ccr
        rts
.stop:
        ori     #1,ccr
        rts

; ---------------------------------------------------------------------------------------------
; Auto-scroll: replaces "sub.w d0,d2 / bcc / (wrap by (W-32)*16)" at $B734. d1/a1 as original.
AutoScrollWrap:
        sub.w   d0,d2
        bcs.s   .wrap
        tst.b   (NATIVE_MODE).w
        bne.s   .done
        cmpi.w  #CAM_MIN,d2
        bcc.s   .done
.wrap:
        lea     (MAP_HDR).l,a1
        move.w  (a1),d1
        subi.w  #$20,d1
        asl.w   #4,d1
        add.w   d1,d2
.done:
        rts

; ---------------------------------------------------------------------------------------------
; Player limits: plain call/return hooks (no return-address tricks, so a static recompiler
; sees ordinary calls). Each site becomes "jsr hook / bcc.s X" where X is an existing
; branch-on-carry-clear to the original "blocked" target, so carry clear = blocked:
;   $DF24  cmpi.w #$83,(a2)  / bcs.w $E01E          -> jsr / bcc.s $DF3A (bcc.w $E01E)
;   $E030  cmpi.w #$1A5,(a2) / bcc.w $E11E          -> jsr / bcc.s $E046 (bcc.w $E11E)
;   $F3FA  cmpi.w #$83,(a4)  / bcs.s $F456 / sub.w d7,(a4)  -> jsr / bcc.s $F456
;   $F360  cmpi.w #$1A5,(a4) / bcc.s $F3BC / add.w d7,(a4)  -> jsr / bcc.s $F3BC
; The push hooks also perform the moved "add/sub.w d7,(a4)" (the other player's x) when not
; blocked. (a2) = this player's x, (a4) = the other player's x; registers are preserved.
OwnLeftLimit:
        move.l  d0,-(sp)
        bsr.s   LeftLimit
        cmp.w   (a2),d0
        movem.l (sp)+,d0                        ; (movem to a register keeps the CCR)
        bhi.s   Blocked                         ; x < limit
        bra.s   Free

OwnRightLimit:
        move.l  d0,-(sp)
        bsr.s   RightLimit
        cmp.w   (a2),d0
        movem.l (sp)+,d0
        bls.s   Blocked                         ; x >= limit
        bra.s   Free

PushLeftLimit:
        move.l  d0,-(sp)
        bsr.s   LeftLimit
        cmp.w   (a4),d0
        movem.l (sp)+,d0
        bhi.s   Blocked                         ; other player's x < limit
        sub.w   d7,(a4)                         ; moved from the call site
        bra.s   Free

PushRightLimit:
        move.l  d0,-(sp)
        bsr.s   RightLimit
        cmp.w   (a4),d0
        movem.l (sp)+,d0
        bls.s   Blocked                         ; other player's x >= limit
        add.w   d7,(a4)                         ; moved from the call site
Free:
        ori     #1,ccr                          ; carry set: carry on
        rts
Blocked:
        andi    #$FE,ccr                        ; carry clear: the site's bcc takes the block path
        rts

; d0.w = the player x limit that applies here (424-px or original).
LeftLimit:
        move.w  #LIMIT_LEFT,d0
        bsr.s   WideHere
        bne.s   .keep
        move.w  #WIDE_LEFT,d0
.keep:
        rts

RightLimit:
        move.w  #LIMIT_RIGHT,d0
        bsr.s   WideHere
        bne.s   .keep
        move.w  #WIDE_RIGHT,d0
.keep:
        rts

; ---------------------------------------------------------------------------------------------
; Z set if the 424-px rules apply here: NATIVE_MODE == 0 and the area is not a camera-locked
; arena. Preserves all registers.
WideHere:
        tst.b   (NATIVE_MODE).w
        bne.s   .done                           ; Z clear: original behaviour
        movem.l d1/a1,-(sp)
        lea     (LockedAreas).l,a1
.next:
        move.b  (a1)+,d1
        bmi.s   .free                           ; end of list: wide (set Z below)
        cmp.b   (AREA).w,d1
        bne.s   .next
        movem.l (sp)+,d1/a1                     ; movem to registers keeps the CCR
        andi    #$FB,ccr                        ; locked: clear Z
        rts
.free:
        movem.l (sp)+,d1/a1
        ori     #4,ccr                          ; set Z
.done:
        rts

LockedAreas:
        dc.b    $12,$13,$17,$19,$1A,$20,$33,$34,$FF
        even

; ---------------------------------------------------------------------------------------------
; Twin-stick facing (native only): when the aim stick is centred, drop the committed aim so the
; character faces the way it moves again; the right stick overrides only while it is held.
; (v1/--wide keep the last aim until the stick points elsewhere.) Replaces, inside AimHook:
;     move.b (a0,d1.w),d2 / bmi.s .settle
; In:  a0 = DirIndex, d1.w = raw aim mask, a1 -> AIM_RAW[p] (+2 = committed mask).
; Out: d2 = facing index; for a centred/impossible mask d1 = 0 and the commit is cleared, so
;      AimHook's next test (committed == raw) takes its .settle path and applies no aim.
AimRelease:
        move.b  (a0,d1.w),d2                    ; replaced: move.b (a0,d1.w),d2
        bpl.s   .aim
        moveq   #0,d1                           ; no aim this frame
        clr.b   2(a1)                           ; forget the commit: movement drives facing
.aim:
        rts

; ---------------------------------------------------------------------------------------------
; Unused entry points (the title menu's "PRESS MINUS FOR CONFIG" line, removed in v0.5.1: the
; host draws a hint instead).
Unused:
        rts

; ---------------------------------------------------------------------------------------------
; Enemy respawns. Enemies ($FF8500, $1C bytes each, count $8138) come from the area's spawn
; table (ROM $26B0A[area]: type, column, row; the entry's index is the enemy's id at +$18).
; The spawner ($11C58) creates the entries on the band of map cells the camera scrolls into,
; unless the id is alive or on ONCE_LIST; so an enemy killed and scrolled past comes back.
; When an enemy is removed ($13CBE: +6 bit 7), one killed (+4 = 0) with "once" set (+7 bit 2;
; e.g. chests) has its id added to ONCE_LIST (cleared at every area load).
;
; OnceTest replaces that "btst #2,$7(a1)" at $13D5A; the caller then "beq.s"s past the add
; (Z set = not once) and the add itself still requires +4 = 0 (killed, not scrolled away).
; With NO_RESPAWN set, every enemy is once, except an id already listed (several enemies
; share their spawner's id) or a full list.
OnceTest:
        btst    #2,$7(a1)                       ; replaced: once?
        bne.s   .done                           ; yes: the game's own rule (Z clear)
        tst.b   (NO_RESPAWN).w
        beq.s   .done                           ; option off: Z set, not once
        movem.l d0-d1/a0,-(sp)
        move.b  $18(a1),d1
        lea     (ONCE_LIST).w,a0
        move.w  (ONCE_N).w,d0
        cmpi.w  #ONCE_MAX,d0
        bcc.s   .no                             ; full
        subq.w  #1,d0
        bcs.s   .yes                            ; empty
.scan:  cmp.b   (a0)+,d1
        beq.s   .no                             ; already listed
        dbra    d0,.scan
.yes:   tst.b   (NO_RESPAWN).w                  ; Z clear (the flag is set): once
        movem.l (sp)+,d0-d1/a0                  ; (movem to registers leaves the flags)
        rts
.no:    cmp.b   d1,d1                           ; Z set: not added
        movem.l (sp)+,d0-d1/a0
.done:  rts
