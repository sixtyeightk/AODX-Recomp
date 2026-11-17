; Arcus Odyssey (USA) — twin-stick aim patch
;
; Aim input: the right stick is remapped (RetroArch) onto the 6-button pad's extra buttons,
; which the game itself never reads. Two pieces of code:
;
;   PadRead  replaces the game's pad poll at $1AF2. It runs the original 3-button read for
;            each port, then continues the TH handshake to fetch X/Y/Z/Mode into AIM_RAW.
;   AimHook  runs just before the per-player facing block (called from $E738) and swaps the
;            d-pad bits of d0 for the player's committed aim. Movement reads its own copy of
;            the input and is unaffected.
;
; Remap contract (rmp): right stick up/down/left/right -> Z/Y/X/Mode, so the extra-button
; nibble read at "fourth high" (b0 Z, b1 Y, b2 X, b3 Mode) is already a d-pad mask in the
; game's own order (b0 U, b1 D, b2 L, b3 R).
;
; Aim rules (see notes/re-findings.md):
;   - aim takes over a player's facing only once they have used it (3-button pads and
;     players who never touch the right stick keep the original controls)
;   - right stick centred        -> keep last committed aim
;   - change of >= 90 degrees    -> commit instantly (deliberate flick)
;   - change to an adjacent 45   -> commit after DEBOUNCE consecutive samples; this filters
;     sector-boundary jitter and the one-sample cardinal blip when a diagonal is released
;
; The game polls the pads and runs the facing loop every *other* frame (30 Hz), so one
; sample spans 2 video frames and DEBOUNCE = 2 is ~67 ms.

DEMO_FLAG       equ     $FFFF8089       ; non-zero during attract-demo playback
PAD_HELD        equ     $FFFF8062       ; per port: held, pressed
FACING          equ     $FFFF8200       ; per-player facing 0-7
AIM_RAW         equ     $FFFFFA00       ; [2] extra-button nibble from PadRead (0 on 3-button pads)
AIM_COMMIT      equ     $FFFFFA02       ; [2] committed aim mask (0 = never aimed: original controls)
AIM_CAND        equ     $FFFFFA04       ; [2] candidate adjacent mask being debounced
AIM_CNT         equ     $FFFFFA06       ; [2] samples the candidate has been held
DEBOUNCE        equ     2

PORT1_DATA      equ     $A10003
ReadPad3        equ     $1B00           ; original: TH low/high read of (a1) into (a0)+ held/pressed

        org     $100000

; Fixed entry points: the ROM call sites (tools/build.py) target these addresses.
        jmp     (PadRead).l             ; $100000  <- jmp from $1AF2
        jmp     (AimHook).l             ; $100006  <- jsr from $E738

; ---------------------------------------------------------------------------------------------
; Pad poll. Reached by jmp from $1AF2, so callers' bsr returns from here. Clobbers d0-d2/a0-a1
; like the original.
PadRead:
        lea     (PAD_HELD).w,a0
        lea     PORT1_DATA,a1
        jsr     ReadPad3                ; port 1 held/pressed -> $8062/$8063
        bsr.s   ReadExtra
        move.b  d0,(AIM_RAW).w
        addq.w  #2,a1                   ; port 2 data
        jsr     ReadPad3                ; port 2 held/pressed -> $8064/$8065
        bsr.s   ReadExtra
        move.b  d0,(AIM_RAW+1).w
        rts

; Continue the 6-button handshake on (a1). ReadPad3 left TH high after one low/high pulse;
; two more rising edges reach "third low" (D3-D0 forced 0 on a 6-button pad) and
; "fourth high" (D3-D0 = Mode X Y Z, active low). A fourth pulse returns the pad to normal.
; Out: d0.b = extra buttons, active high (b0 Z, b1 Y, b2 X, b3 Mode), or 0 if not a 6-button pad.
ReadExtra:
        move.b  #$00,(a1)               ; TH low  (2nd)
        nop
        nop
        move.b  #$40,(a1)               ; TH high (2nd)
        nop
        nop
        move.b  #$00,(a1)               ; TH low  (3rd)
        nop
        nop
        move.b  (a1),d1                 ; ?0SA0000 on a 6-button pad
        move.b  #$40,(a1)               ; TH high (3rd)
        nop
        nop
        move.b  (a1),d0                 ; ?1CBMXYZ
        move.b  #$00,(a1)               ; TH low  (4th)
        nop
        nop
        move.b  #$40,(a1)               ; TH high (4th): back to 3-button values, TH idle high
        andi.b  #$0F,d1
        bne.s   .not6                   ; 3-button pad shows D/U here, never both low
        not.b   d0
        andi.b  #$0F,d0
        rts
.not6:
        moveq   #0,d0
        rts

; ---------------------------------------------------------------------------------------------
; In:  d0.w = this player's input (held<<8 | pressed), d7.w = player index
; Out: a4 = $FF8200+d7 (the two instructions this call replaced), d0 d-pad bits replaced
AimHook:
        lea     (FACING).w,a4           ; replaced: lea $8200.w,a4
        adda.w  d7,a4                   ; replaced: adda.w d7,a4
        tst.b   (DEMO_FLAG).w
        bne.s   .done                   ; attract demo: recorded input drives facing
        movem.l d1-d3/a0-a1,-(sp)

        lea     (AIM_RAW).w,a1
        adda.w  d7,a1                   ; a1 -> this player's AIM_RAW; +2/+4/+6 = COMMIT/CAND/CNT
        moveq   #0,d1
        move.b  (a1),d1                 ; d1 = raw aim mask
        lea     DirIndex(pc),a0
        move.b  (a0,d1.w),d2            ; d2 = raw facing index, or -1
        bmi.s   .settle                 ; centred / impossible combo: keep last aim

        moveq   #0,d3
        move.b  2(a1),d3                ; committed mask
        cmp.b   d3,d1
        beq.s   .settle                 ; already aiming here
        tst.b   d3
        beq.s   .commit                 ; first aim: instant
        move.b  (a0,d3.w),d3            ; d3 = committed facing index
        sub.b   d2,d3
        andi.b  #7,d3                   ; angular distance in 45-degree steps
        cmpi.b  #1,d3
        beq.s   .adjacent
        cmpi.b  #7,d3
        bne.s   .commit                 ; >= 90 degrees: instant

.adjacent:
        cmp.b   4(a1),d1
        beq.s   .count
        move.b  d1,4(a1)                ; new candidate: restart the count
        clr.b   6(a1)
.count:
        addq.b  #1,6(a1)
        cmpi.b  #DEBOUNCE,6(a1)
        bcs.s   .apply                  ; not held long enough yet

.commit:
        move.b  d1,2(a1)
.settle:
        clr.b   4(a1)
        clr.b   6(a1)

.apply:
        moveq   #0,d1
        move.b  2(a1),d1
        beq.s   .restore                ; never aimed: original controls
        lsl.w   #8,d1
        andi.w  #$F0FF,d0               ; drop the movement d-pad from the facing input
        or.w    d1,d0                   ; substitute the committed aim
.restore:
        movem.l (sp)+,d1-d3/a0-a1
.done:
        rts

; d-pad mask (b0 U, b1 D, b2 L, b3 R) -> game facing (0 S, 1 SW, 2 W, 3 NW, 4 N, 5 NE, 6 E, 7 SE)
DirIndex:
        dc.b    -1, 4, 0,-1, 2, 3, 1,-1, 6, 5, 7,-1,-1,-1,-1,-1
