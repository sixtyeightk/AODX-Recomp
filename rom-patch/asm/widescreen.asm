; Arcus Odyssey (USA) — widescreen map drawing (for Genesis Plus GX Wide, 16:9)
;
; The game keeps a 23-metatile-wide (368 px) window of the map in the 64-tile (512 px) plane
; rings: map column ($8100>>4) sits at VRAM x = ($200 - $8110), 16 px left of the screen, and
; hscroll is derived from the same variable, so that relation must not change. To fill a
; 424-px view, every drawer instead covers WIDE_EXTRA more metatiles on each side:
;   map columns [origin - WIDE_EXTRA, origin + 22 + WIDE_EXTRA] = 29 metatiles = 58 tiles.
;
; Most of it is constant edits at the call sites (tools/build.py, WIDE_PATCHES):
;   row / left-column VRAM start   #$200 -> #$1D0   (#$400 -> #$3A0 for the half-speed plane)
;   right-column VRAM x            #$360 -> #$390   (#$6C0 -> #$720)
;   right-column map offset        +$16  -> +$1C    (22 + 2*WIDE_EXTRA, after MapCol's -3)
;   row / level-start widths       23    -> 29
; plus three helpers here:
;   MapColFg / MapColBg  replace "move.w $8100,d0 / lsr #4,d0" (and $8104 / #5): the drawn
;                        window's first map column, moved left with the map's own wrap
;   InitAdvance          the level-start draw writes rows linearly from VRAM column 0 and
;                        never wraps; it now starts at column 58 and wraps within the row

WIDE_EXTRA      equ     3                       ; extra metatiles drawn on each side
MAP_ORIGIN_X    equ     $FFFF8100               ; map x of the drawn window (fg), px
MAP_ORIGIN_BG_X equ     $FFFF8104               ; same for the half-speed plane, px

        org     $100400

; Fixed entry points: the ROM call sites (tools/build.py) target these addresses.
        jmp     (MapColFg).l                    ; $100400
        jmp     (MapColBg).l                    ; $100406
        jmp     (InitAdvance).l                 ; $10040C

; ---------------------------------------------------------------------------------------------
; Out: d0.w = first map metatile column of the drawn window, wrapped into [0, width).
; In:  a2 -> map header ($FF0C00), width in metatiles at (a2) (as the drawers' own wrap uses)
MapColFg:
        move.w  (MAP_ORIGIN_X).w,d0             ; replaced: move.w $8100.w,d0
        lsr.w   #4,d0                           ; replaced: lsr.w #4,d0
        bra.s   Wrap
MapColBg:
        move.w  (MAP_ORIGIN_BG_X).w,d0          ; replaced: move.w $8104.w,d0
        lsr.w   #5,d0                           ; replaced: lsr.w #5,d0
Wrap:
        subq.w  #WIDE_EXTRA,d0
        bcc.s   .done
        add.w   (a2),d0
.done:
        rts

; ---------------------------------------------------------------------------------------------
; Level-start draw: step to the next metatile in the row (2 tiles right, back up to the top
; tile row) and wrap within the 64-tile row instead of running into the next one.
; d7 = VDP write command (address in the high word); d4 is free here (reloaded before use).
InitAdvance:
        subi.l  #$7C0000,d7                     ; replaced: +4 bytes, -1 tile row
        move.l  d7,d4
        swap    d4
        andi.w  #$7E,d4                         ; byte offset within the 128-byte row
        bne.s   .same
        subi.l  #$800000,d7                     ; crossed into the next row: back to column 0
.same:
        rts
