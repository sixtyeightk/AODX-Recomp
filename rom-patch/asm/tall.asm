; Arcus Odyssey (USA) — camera stops and player limits for the native engine's
; full-screen "tall" view (tools/build.py --tall -> build/arcus-tall.bin; see TALL_PATCHES).
;
; The view size depends on the Switch's dock state (480 x 270 docked, 426 x 240 handheld), so
; the host writes it to RAM every frame (all words; 0 = the native 424 x 224 behaviour):
;   VIEW_EXTRA   px of map shown beside the 320-px screen on each side (52 when 0)
;   VIEW_TOP     px shown above raster line 0 (8)
;   VIEW_DOWN    px the HUD moved down: H - VIEW_TOP - 224 (38 docked, 8 handheld)
;   VIEW_CAMB    px of map shown below the original 192-line playfield: the map is visible
;                down to the bottom of the output beside the HUD, H - VIEW_TOP - 192
;
; Horizontal: the native.asm hooks with their 424-px constants derived from VIEW_EXTRA
; (TALL_PATCHES points native.asm's jump table here):
;   CAM_MIN = E-16, CAM_RIGHT = E+336, INIT_MIN/RIGHT = those rounded up to a metatile,
;   player limits $83-E / $1A5+E, maps narrower than 320+2E are left alone.
; Vertical (new call sites):
;   camera top stop     -16 -> -16+VIEW_TOP   ($F1C2: the "W-16" base of the up clamp)
;   camera bottom stop  W-$C1-16 -> minus VIEW_CAMB as well ($F274)
;   player limits       $83 -> $83-VIEW_TOP ($DD04 own, $F294 push), $11D -> $11D+VIEW_DOWN
;                       ($DE12 own, $F1E4 push)
;   area start          the start camera y is clamped like x (a multiple of 16, players moved
;                       with it) so the first frame never shows map beyond the bottom stop

NATIVE_MODE     equ     $FFFFFA08
VIEW_EXTRA      equ     $FFFFFA0A
VIEW_TOP        equ     $FFFFFA0C
VIEW_DOWN       equ     $FFFFFA0E
VIEW_CAMB       equ     $FFFFFA10
MAP_HDR         equ     $FFFF0C00               ; width (metatiles) at +0, height at +2
CAM_X           equ     $FFFF8100
CAM_Y           equ     $FFFF8102
AREA            equ     $FFFF828E
CLAMPED         equ     $FFFF8292
P1_X            equ     $FFFF8118
P2_X            equ     $FFFF811A
P1_Y            equ     $FFFF811C
P2_Y            equ     $FFFF811E
P1_X_PREV       equ     $FFFF8128
P2_X_PREV       equ     $FFFF812A
P1_Y_PREV       equ     $FFFF812C
P2_Y_PREV       equ     $FFFF812E
LIMIT_LEFT      equ     $83
LIMIT_RIGHT     equ     $1A5
LIMIT_TOP       equ     $83
LIMIT_BOTTOM    equ     $11D

        org     $100A00

        jmp     (CamInit).l                     ; $100A00  native table $100600
        jmp     (CamLeftStop).l                 ; $100A06  native table $100606
        jmp     (CamRightStop).l                ; $100A0C  native table $10060C
        jmp     (AutoScrollWrap).l              ; $100A12  native table $100612
        jmp     (OwnLeftLimit).l                ; $100A18  native table $100618
        jmp     (OwnRightLimit).l               ; $100A1E  native table $10061E
        jmp     (PushLeftLimit).l               ; $100A24  native table $100624
        jmp     (PushRightLimit).l              ; $100A2A  native table $10062A
        jmp     (CamInitY).l                    ; $100A30  jsr from $7A06
        jmp     (CamTopBase).l                  ; $100A36  jsr from $F1C2
        jmp     (CamBottomSub).l                ; $100A3C  jsr from $F274
        jmp     (OwnTopLimit).l                 ; $100A42  jsr from $DD04
        jmp     (OwnBottomLimit).l              ; $100A48  jsr from $DE12
        jmp     (PushUpLimit).l                 ; $100A4E  jsr from $F294
        jmp     (PushDownLimit).l               ; $100A54  jsr from $F1E4
        jmp     (RightColumn).l                 ; $100A5A  jsr from $91C6 / $92BE
        jmp     (RewindColumns).l               ; $100A60  jsr from $9FE2 / $A0B6

; ---------------------------------------------------------------------------------------------
; d0.w = E (px per side). Preserves everything else.
Extra:
        move.w  (VIEW_EXTRA).w,d0
        bne.s   .done
        moveq   #52,d0
.done:
        rts

; ---------------------------------------------------------------------------------------------
; As native.asm CamInit ($79F2: replaces "move.w (a0)+,d0 / move.w d0,$8100.w").
CamInit:
        move.w  (a0)+,d0
        movem.l d1-d4,-(sp)
        bsr     WideHere
        bne.s   .done
        move.w  d0,d3                           ; d3 = camera
        bsr     Extra
        move.w  d0,d4                           ; d4 = E
        move.w  d3,d0
        move.w  (MAP_HDR).l,d1
        asl.w   #4,d1                           ; d1 = W (px)
        move.w  d4,d2
        add.w   d2,d2
        addi.w  #320,d2
        cmp.w   d2,d1                           ; maps narrower than the view: untouched
        bcs.s   .done
        move.w  d1,d2
        subi.w  #16,d2
        cmp.w   d2,d0                           ; [W-16, W) is the original's -16..-1
        bcs.s   .signed
        sub.w   d1,d0
.signed:
        move.w  d4,d2
        subi.w  #16-15,d2                       ; INIT_MIN = (E-16) rounded up to 16
        andi.w  #$FFF0,d2
        cmp.w   d2,d0
        bge.s   .right
        move.w  d2,d3
        sub.w   d0,d2                           ; d2 = shift (> 0)
        move.w  d3,d0
        bra.s   .shift
.right:
        move.w  d4,d3
        addi.w  #336+15,d3                      ; INIT_RIGHT = (E+336) rounded up to 16
        andi.w  #$FFF0,d3
        sub.w   d3,d1                           ; d1 = rightmost start camera
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
        movem.l (sp)+,d1-d4
        move.w  d0,(CAM_X).w
        rts

; ---------------------------------------------------------------------------------------------
; As native.asm CamLeftStop ($F33C). In: d2 = camera x, d7.w = step, a0 -> map header.
; Out: carry set = stop. d1 is free here.
CamLeftStop:
        tst.b   (CLAMPED).w
        beq.s   .go
        move.w  (a0),d1
        subq.w  #1,d1
        asl.w   #4,d1                           ; d1 = W-16, the original stop
        tst.b   (NATIVE_MODE).w
        bne.s   .original
        cmp.w   d1,d2                           ; already in W-16..W-1 (left of the stop)
        bcc.s   .stop
        move.w  d2,d1
        sub.w   d7,d1                           ; camera after this step
        bcs.s   .stop
        move.l  d0,-(sp)
        bsr     Extra
        subi.w  #16,d0                          ; CAM_MIN
        cmp.w   d0,d1                           ; carry set if it would pass CAM_MIN
        movem.l (sp)+,d0                        ; (keeps the CCR)
        rts
.original:                                      ; stop if d2 in [W-16, W-16+step)
        cmp.w   d1,d2
        bcs.s   .go
        add.w   d7,d1
        cmp.w   d1,d2
        rts
.stop:
        ori     #1,ccr
        rts
.go:
        andi    #$FE,ccr
        rts

; ---------------------------------------------------------------------------------------------
; As native.asm CamRightStop ($F3D0). Out: carry set = stop.
CamRightStop:
        tst.b   (CLAMPED).w
        beq.s   .go
        move.w  (a0),d1
        asl.w   #4,d1
        subi.w  #16,d1
        cmp.w   d1,d2                           ; in W-16..W-1 (left of the map): scroll
        bcc.s   .go
        tst.b   (NATIVE_MODE).w
        beq.s   .wide
        subi.w  #336-16,d1                      ; original: W-336
        bra.s   .test
.wide:
        move.l  d0,-(sp)
        bsr     Extra
        addi.w  #336-16,d0
        sub.w   d0,d1                           ; W - CAM_RIGHT
        move.l  (sp)+,d0
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
; As native.asm AutoScrollWrap ($B734).
AutoScrollWrap:
        sub.w   d0,d2
        bcs.s   .wrap
        tst.b   (NATIVE_MODE).w
        bne.s   .done
        move.l  d0,-(sp)
        bsr     Extra
        subi.w  #16,d0
        cmp.w   d0,d2
        movem.l (sp)+,d0
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
; Horizontal player limits (as native.asm; carry set = free, clear = blocked).
OwnLeftLimit:
        move.l  d0,-(sp)
        bsr     LeftLimit
        cmp.w   (a2),d0
        movem.l (sp)+,d0
        bhi.s   Blocked
        bra.s   Free

OwnRightLimit:
        move.l  d0,-(sp)
        bsr     RightLimit
        cmp.w   (a2),d0
        movem.l (sp)+,d0
        bls.s   Blocked
        bra.s   Free

PushLeftLimit:
        move.l  d0,-(sp)
        bsr     LeftLimit
        cmp.w   (a4),d0
        movem.l (sp)+,d0
        bhi.s   Blocked
        sub.w   d7,(a4)
        bra.s   Free

PushRightLimit:
        move.l  d0,-(sp)
        bsr     RightLimit
        cmp.w   (a4),d0
        movem.l (sp)+,d0
        bls.s   Blocked
        add.w   d7,(a4)
Free:
        ori     #1,ccr
        rts
Blocked:
        andi    #$FE,ccr
        rts

LeftLimit:
        bsr     WideHere
        beq.s   .wide
        move.w  #LIMIT_LEFT,d0
        rts
.wide:
        move.l  d1,-(sp)
        bsr     Extra
        move.w  d0,d1
        move.w  #LIMIT_LEFT,d0
        sub.w   d1,d0
        move.l  (sp)+,d1
        rts

RightLimit:
        bsr     WideHere
        beq.s   .wide
        move.w  #LIMIT_RIGHT,d0
        rts
.wide:
        bsr     Extra
        addi.w  #LIMIT_RIGHT,d0
        rts

; ---------------------------------------------------------------------------------------------
; Vertical view extensions that apply here (0 in camera-locked arenas and in 4:3, which keep
; the original framing; the ROM decides, so an area change takes effect on its first frame).
; Out: d0.w. Preserves everything else.
VTop:
        moveq   #0,d0
        bsr     WideHere
        bne.s   .done
        move.w  (VIEW_TOP).w,d0
.done:
        rts
VDown:
        moveq   #0,d0
        bsr     WideHere
        bne.s   .done
        move.w  (VIEW_DOWN).w,d0
.done:
        rts
VCamb:
        moveq   #0,d0
        bsr     WideHere
        bne.s   .done
        move.w  (VIEW_CAMB).w,d0
.done:
        rts

; Area start, replaces "move.w (a0)+,d0 / move.w d0,$8102.w" at $7A06 (the caller derives
; $810A/$8106/$810E from d0).
; 1. Some entry points start at y >= H (area $28: 1680 in a 512-px map). The game's row wrap
;    then draws from map row 0 as if the camera were at -16, but the 20-row ring needs the
;    camera to say so: use H-16.
; 2. Tall view: clamp the start camera into [top stop, bottom stop] rounded inwards to a
;    metatile (the start draw needs y & 15 == 0) and move both players with it.
CamInitY:
        move.w  (a0)+,d0
        movem.l d1-d4,-(sp)
        move.w  (MAP_HDR+2).l,d1
        asl.w   #4,d1                           ; d1 = H (px)
        beq.s   .done
        cmp.w   d1,d0
        bcs.s   .inmap
        move.w  d1,d0
        subi.w  #16,d0
.inmap:
        move.w  d0,d4
        bsr     VCamb
        move.w  d0,d3
        move.w  d4,d0
        tst.w   d3
        beq.s   .done                           ; not tall here: unchanged
        move.w  d1,d2
        subi.w  #16,d2
        cmp.w   d2,d0                           ; [H-16, H) is -16..-1: already at the top
        bcc.s   .done
        subi.w  #$C1+16,d1
        sub.w   d3,d1                           ; d1 = bottom stop
        bmi.s   .done                           ; map shorter than the view
        andi.w  #$FFF0,d1
        cmp.w   d1,d0
        ble.s   .done
        move.w  d1,d2
        sub.w   d0,d2                           ; d2 = shift (< 0): camera moves up
        move.w  d1,d0
        sub.w   d2,(P1_Y).w                     ; players move down the screen
        sub.w   d2,(P1_Y_PREV).w
        sub.w   d2,(P2_Y).w
        sub.w   d2,(P2_Y_PREV).w
.done:
        movem.l (sp)+,d1-d4
        move.w  d0,(CAM_Y).w
        rts

; Replaces "move.w 2(a0),d1 / subq.w #1,d1 / asl.w #4,d1" at $F1C2 (+ nop): d1 = H-16+top.
CamTopBase:
        move.l  d0,-(sp)
        bsr     VTop
        move.w  2(a0),d1
        asl.w   #4,d1
        subi.w  #16,d1
        add.w   d0,d1
        move.l  (sp)+,d0
        rts

; Replaces "subi.w #$C1,d1 / sub.w d7,d1" at $F274: the bottom stop moves up by VIEW_CAMB.
CamBottomSub:
        move.l  d0,-(sp)
        bsr     VCamb
        subi.w  #$C1,d1
        sub.w   d0,d1
        sub.w   d7,d1
        move.l  (sp)+,d0
        rts

; Own vertical limits: "jsr / beq.s <the btst's beq.w to the blocked target>"; Z set = blocked.
; $DD04 cmpi.w #$83,(a3) / bcs.w $DE00     -> jsr / beq.s $DD00 (beq.w $DE00)
; $DE12 cmpi.w #$11D,(a3) / bcc.w $DF06    -> jsr / beq.s $DE0E (beq.w $DF06)
OwnTopLimit:
        movem.l d0-d1,-(sp)
        bsr     VTop
        move.w  #LIMIT_TOP,d1
        sub.w   d0,d1
        cmp.w   (a3),d1                         ; blocked if y < limit, i.e. limit > y
        movem.l (sp)+,d0-d1
        bhi.s   ZSet
        bra.s   ZClear

OwnBottomLimit:
        movem.l d0-d1,-(sp)
        bsr     VDown
        move.w  #LIMIT_BOTTOM,d1
        add.w   d0,d1
        cmp.w   (a3),d1                         ; blocked if y >= limit
        movem.l (sp)+,d0-d1
        bls.s   ZSet
ZClear:
        andi    #$FB,ccr
        rts
ZSet:
        ori     #4,ccr
        rts

; Push limits while the camera scrolls (other player at (a5)); carry set = free (and the moved
; add/sub done), clear = blocked:
; $F294 cmpi.w #$83,(a5) / bcs.s $F302 / sub.w d7,(a5)   -> jsr / bcc.s $F27C (bcc.w $F302)
; $F1E4 cmpi.w #$11D,(a5) / bcc.s $F250 / add.w d7,(a5)  -> jsr / bcc.s $F1B8 (bcc.w $F250)
PushUpLimit:
        movem.l d0-d1,-(sp)
        bsr     VTop
        move.w  #LIMIT_TOP,d1
        sub.w   d0,d1
        cmp.w   (a5),d1
        movem.l (sp)+,d0-d1
        bhi     Blocked                         ; other player's y < limit
        sub.w   d7,(a5)
        bra     Free

PushDownLimit:
        movem.l d0-d1,-(sp)
        bsr     VDown
        move.w  #LIMIT_BOTTOM,d1
        add.w   d0,d1
        cmp.w   (a5),d1
        movem.l (sp)+,d0-d1
        bls     Blocked                         ; other player's y >= limit
        add.w   d7,(a5)
        bra     Free

; ---------------------------------------------------------------------------------------------
; Map column wraps for the 35-metatile window. The game wraps a column index once ("if >= W,
; subtract W"), which is wrong when the window is wider than the map (many maps are 32
; metatiles wide). In: d0 = column, a2 -> map header (width at (a2)).
; Right column: replaces "addi.w #34,d0 / cmp.w (a2),d0 / bcs.s +2 / sub.w (a2),d0" (+ 2 nops).
RightColumn:
        addi.w  #34,d0
.wrap:
        cmp.w   (a2),d0
        bcs.s   .done
        sub.w   (a2),d0
        bra.s   .wrap
.done:
        rts

; Level start, next row: replaces "subi.w #34,d0 / bcc.s +2 / add.w (a2),d0" (+ nop).
RewindColumns:
        subi.w  #34,d0
.wrap:
        bpl.s   .done
        add.w   (a2),d0
        bra.s   .wrap
.done:
        rts

; ---------------------------------------------------------------------------------------------
; Z set if the wide rules apply here (as native.asm WideHere). Preserves all registers.
WideHere:
        tst.b   (NATIVE_MODE).w
        bne.s   .done
        movem.l d1/a1,-(sp)
        lea     (LockedAreas).l,a1
.next:
        move.b  (a1)+,d1
        bmi.s   .free
        cmp.b   (AREA).w,d1
        bne.s   .next
        movem.l (sp)+,d1/a1
        andi    #$FB,ccr
        rts
.free:
        movem.l (sp)+,d1/a1
        ori     #4,ccr
.done:
        rts

LockedAreas:
        dc.b    $12,$13,$17,$19,$1A,$20,$33,$34,$FF
        even
