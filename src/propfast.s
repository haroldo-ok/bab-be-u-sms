; Fast class-property lookups.
;
; SDCC sdcccall(1) for unsigned char f(unsigned char cls, unsigned char prop):
;   A = cls, L = prop, result in A.  Only caller-saved registers are used.
; The C versions spent most of their time in IX frame setup and repeated
; 2D-array address calculations; these loops are on the hottest engine path.

        .module propfast
        .globl  _has_prop_c
        .globl  _has_anti_c
        .globl  _build_rows
        .globl  _draw_clear_cells
        .globl  _draw_scan_changed
        .globl  _scratch
        .globl  _sh_key
        .globl  _need_full_redraw
        .globl  _nclass
        .globl  _nunits
        .globl  _u_cls
        .globl  _u_y
        .globl  _u_next
        .globl  _row_head
        .globl  _cls_nprops
        .globl  _cls_props
        .globl  _cls_props_cnt

        .area   _DATA
propfast_prop:
        .ds     1
dsc_key:
        .ds     2
dsc_cnt:
        .ds     1

        .area   _CODE

; ---------------------------------------------------------------------------
; unsigned char has_prop_c(unsigned char cls, unsigned char prop)
; normal property counts are 1..250; 0xFE=anti, 0xFF=blocked.
_has_prop_c:
        ld      c, a                    ; C = cls
        ld      a, l
        ld      (propfast_prop), a      ; D/E/HL are needed for pointers
        ld      a, (_nclass)
        cp      c                       ; cls > nclass -> carry, = -> zero
        jr      c, hpc_no
        jr      z, hpc_no
        ld      b, #0
        ld      hl, #_cls_nprops
        add     hl, bc
        ld      b, (hl)                 ; B = property count
        ld      a, b
        or      a
        jr      z, hpc_no
        ; HL = cls * 6
        ld      l, c
        ld      h, #0
        add     hl, hl                  ; *2 (<=70, no overflow below)
        ld      a, l
        add     a, c
        ld      l, a                    ; *3
        add     hl, hl                  ; *6
        push    hl                      ; offset
        ld      de, #_cls_props
        add     hl, de                  ; HL = props for class
        pop     de                      ; DE = offset
        push    hl
        ld      hl, #_cls_props_cnt
        add     hl, de
        ex      de, hl                  ; DE = counts for class
        pop     hl                      ; HL = props for class
        ld      a, (propfast_prop)
        ld      c, a                    ; C = prop (cls no longer needed)
hpc_loop:
        ld      a, (hl)
        cp      c
        jr      nz, hpc_next
        ld      a, (de)
        cp      #0xFE
        jr      c, hpc_yes
hpc_next:
        inc     hl
        inc     de
        djnz    hpc_loop
hpc_no:
        xor     a
        ret
hpc_yes:
        ld      a, #1
        ret

; ---------------------------------------------------------------------------
; unsigned char has_anti_c(unsigned char cls, unsigned char prop)
_has_anti_c:
        ld      c, a
        ld      a, l
        ld      (propfast_prop), a
        ld      a, (_nclass)
        cp      c
        jr      c, hac_no
        jr      z, hac_no
        ld      b, #0
        ld      hl, #_cls_nprops
        add     hl, bc
        ld      b, (hl)
        ld      a, b
        or      a
        jr      z, hac_no
        ld      l, c
        ld      h, #0
        add     hl, hl
        ld      a, l
        add     a, c
        ld      l, a
        add     hl, hl
        push    hl
        ld      de, #_cls_props
        add     hl, de
        pop     de
        push    hl
        ld      hl, #_cls_props_cnt
        add     hl, de
        ex      de, hl
        pop     hl
        ld      a, (propfast_prop)
        ld      c, a
hac_loop:
        ld      a, (hl)
        cp      c
        jr      nz, hac_next
        ld      a, (de)
        cp      #0xFE
        jr      z, hac_yes
hac_next:
        inc     hl
        inc     de
        djnz    hac_loop
hac_no:
        xor     a
        ret
hac_yes:
        ld      a, #1
        ret

; ---------------------------------------------------------------------------
; void build_rows(void)
; Rebuild the per-row unit chains.  Unit indices are 0..254 and 255 is
; NIL, so the whole loop is 8-bit.  Insertion is in ascending unit order
; at each row head, exactly as the former C version (chains therefore
; walk in descending unit order).
_build_rows:
        push    ix
        push    iy
        ; row_head[0..127] = 0xFF
        ld      hl, #_row_head
        ld      de, #_row_head+1
        ld      bc, #127
        ld      (hl), #0xFF
        ldir
        ld      a, (_nunits)
        or      a
        jr      z, br_done
        ld      b, a                    ; B = remaining units
        ld      c, #0                   ; C = unit index
        ld      de, #_u_cls             ; DE = class pointer
        ld      iy, #_u_y               ; IY = y pointer
        ld      ix, #_u_next            ; IX = next pointer
br_loop:
        ld      a, (de)
        inc     de
        inc     a                       ; 0xFF (dead) wraps to zero
        jr      z, br_dead
        ld      a, 0 (iy)
        ld      hl, #_row_head
        add     a, l
        ld      l, a
        jr      nc, br_no_carry
        inc     h
br_no_carry:
        ld      a, (hl)                 ; old head
        ld      0 (ix), a
        ld      (hl), c                 ; new head = this unit
        jr      br_next
br_dead:
        ld      0 (ix), #0xFF
br_next:
        inc     iy
        inc     ix
        inc     c
        djnz    br_loop
br_done:
        pop     iy
        pop     ix
        ret

; ---------------------------------------------------------------------------
; Draw helpers.  Fixed scratch layout owned by draw_board in render.c:
;   cell_key   = scratch+448  (192 u16, seed 0x8000)
;   cell_count = scratch+832  (192 bytes)
;   cell_top   = scratch+1024 (192 bytes, 0xFF = none)
;   changed    = scratch+1216 (192 bytes out)
; void draw_clear_cells(void)
_draw_clear_cells:
        ld      hl, #_scratch+448
        ld      (hl), #0
        inc     hl
        ld      (hl), #0x80
        dec     hl
        ld      d, h
        ld      e, l
        inc     de
        inc     de
        ld      bc, #382
        ldir                            ; overlapping copy repeats 00 80
        ld      hl, #_scratch+832
        ld      (hl), #0
        ld      d, h
        ld      e, l
        inc     de
        ld      bc, #191
        ldir
        ld      hl, #_scratch+1024
        ld      (hl), #0xFF
        ld      d, h
        ld      e, l
        inc     de
        ld      bc, #191
        ldir
        ret

; unsigned char draw_scan_changed(void)
; For each of 192 cells: key = count ? cell_key : 0 (0 -> 0x8001 when
; count!=0).  If need_full_redraw or key != sh_key[i], append i to the
; changed list and store key into sh_key[i].  Returns the count in A.
_draw_scan_changed:
        push    ix
        push    iy
        ld      hl, #_scratch+448       ; cell_key
        ld      de, #_sh_key
        ld      ix, #_scratch+832       ; cell_count
        ld      iy, #_scratch+1216      ; changed out
        ld      b, #192
        ld      c, #0                   ; cell index
        ld      a, (_need_full_redraw)
        or      a
        jp      nz, dsc_loop            ; full redraw: simple loop below
        ; Fast path: compare/store keys in place, no memory temporaries.
dscf_loop:
        ld      a, 0 (ix)
        inc     ix
        or      a
        jr      z, dscf_empty
        ld      a, (hl)                 ; key == 0?
        inc     hl
        or      (hl)
        dec     hl                      ; HL -> key lo (flags preserved)
        jr      z, dscf_zero
        ld      a, (de)                 ; compare key with sh_key
        cp      (hl)
        jr      nz, dscf_copy_key
        inc     hl
        inc     de
        ld      a, (de)
        cp      (hl)
        jr      nz, dscf_copy_key_hi
        inc     hl                      ; same: advance both past hi
        inc     de
        jr      dscf_next
dscf_copy_key_hi:
        dec     hl
        dec     de
dscf_copy_key:                          ; HL -> key lo, DE -> sh lo
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        jr      dscf_append
dscf_zero:                              ; virtual key 0x8001, HL -> key lo
        ld      a, (de)
        cp      #1
        jr      nz, dscf_store8001
        inc     de
        ld      a, (de)
        cp      #0x80
        jr      nz, dscf_store8001_hi
        inc     de
        inc     hl
        inc     hl
        jr      dscf_next
dscf_store8001_hi:
        dec     de
dscf_store8001:
        ld      a, #1
        ld      (de), a
        inc     de
        ld      a, #0x80
        ld      (de), a
        inc     de
        inc     hl
        inc     hl
        jr      dscf_append
dscf_empty:                             ; virtual key 0, HL -> key lo
        ld      a, (de)
        or      a
        jr      nz, dscf_store0
        inc     de
        ld      a, (de)
        or      a
        jr      nz, dscf_store0_hi
        inc     de
        inc     hl
        inc     hl
        jr      dscf_next
dscf_store0_hi:
        dec     de
dscf_store0:
        xor     a
        ld      (de), a
        inc     de
        ld      (de), a
        inc     de
        inc     hl
        inc     hl
dscf_append:
        ld      a, c
        ld      0 (iy), a
        inc     iy
dscf_next:
        inc     c
        dec     b
        jp      nz, dscf_loop
        jp      dsc_done
dsc_loop:
        ld      a, 0 (ix)
        inc     ix
        ld      (dsc_cnt), a
        ld      a, (hl)
        inc     hl
        ld      (dsc_key), a
        ld      a, (hl)
        inc     hl
        ld      (dsc_key+1), a
        ld      a, (dsc_cnt)
        or      a
        jr      nz, dsc_nonempty
        xor     a
        ld      (dsc_key), a
        ld      (dsc_key+1), a
        jr      dsc_cmp
dsc_nonempty:
        ld      a, (dsc_key)
        or      a
        jr      nz, dsc_cmp
        ld      a, (dsc_key+1)
        or      a
        jr      nz, dsc_cmp
        ld      a, #1
        ld      (dsc_key), a
        ld      a, #0x80
        ld      (dsc_key+1), a
dsc_cmp:
        ld      a, (_need_full_redraw)
        or      a
        jr      nz, dsc_changed
        push    hl
        ld      hl, #dsc_key
        ld      a, (de)
        cp      (hl)
        jr      nz, dsc_chg_pop
        inc     hl
        inc     de
        ld      a, (de)
        cp      (hl)
        dec     de
        jr      nz, dsc_chg_pop
        pop     hl
        jr      dsc_same
dsc_chg_pop:
        pop     hl
dsc_changed:
        ld      a, (dsc_key)
        ld      (de), a
        inc     de
        ld      a, (dsc_key+1)
        ld      (de), a
        dec     de
        ld      a, c
        ld      0 (iy), a
        inc     iy
dsc_same:
        inc     de
        inc     de
        inc     c
        djnz    dsc_loop
dsc_done:
        push    iy
        pop     hl
        ld      bc, #_scratch+1216
        xor     a
        sbc     hl, bc
        ld      a, l
        pop     iy
        pop     ix
        ret
