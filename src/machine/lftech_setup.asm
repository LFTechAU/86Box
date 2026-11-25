; LFTech Features Setup: the extra page in the LFTech BIOSes' setup menu.
;
; The BIOS's menu item calls a small stub that writes 'L' to port EEh; the
; emulator then copies this image to 8000:0000, fills in the header below
; for the board, and the stub far-calls 8000:0003. The page draws itself in
; the colours of the setup screen it was called from, and on Esc writes its
; settings to two CMOS bytes (the emulator reads them back):
;   byte A: 10xx dddd  - bit 3 logo off, bit 2 spin-up wait off,
;                        bits 1:0 de-turbo speed (1/3, 1/2, 2/3, 8 MHz)
;   byte B: 101i iiii  - extra DRAM wait, index into dram_tab, 1Fh = Auto
;
; Assemble with: nasm -f bin lftech_setup.asm -o lftech_setup.bin
; then regenerate lftech_setup_rom.h (see the end of this file).

bits 16
cpu 386
org 0

    db 0xeb, 0x0f, 0x90             ; header: jump over it (0000)
entry_far:                          ; 0003: far-called by the BIOS stub
    jmp near main
    db 'LFTS'                       ; 0006
cmos_a:     db 0x70                 ; 000A  CMOS index of byte A
cmos_b:     db 0x71                 ; 000B  CMOS index of byte B
dram_max:   db 17                   ; 000C  highest DRAM wait index offered
auto_clk:   db 5                    ; 000D  clocks Auto picks for this CPU
            db 0, 0                 ; 000E

; ---------------------------------------------------------------- data
NITEMS equ 4

lbl_tab:    dw l_det, l_dram, l_spin, l_logo
cnt_tab:    db 4, 0, 2, 2           ; option counts (DRAM from dram_max)
opt_tab:    dw o_det, o_dram, o_en, o_en
help_tab:   dw h_det, h_dram, h_spin, h_logo

l_det:  db 'De-Turbo Speed', 0
l_dram: db 'Extra DRAM Wait (No Cache)', 0
l_spin: db 'Hard Disk Spin-Up Wait', 0
l_logo: db 'LFTech Logo on POST', 0

; option strings, 11 bytes each (zero padded)
%macro opt 1
%%s: db %1
    times 11 - ($ - %%s) db 0
%endmacro
o_det:  opt '1/3 Speed'
        opt '1/2 Speed'
        opt '2/3 Speed'
        opt '8 MHz (AT)'
o_en:   opt 'Enabled'
        opt 'Disabled'
o_dram: opt 'Auto'
        opt 'None'
        opt '+1 Clocks'
        opt '+2 Clocks'
        opt '+3 Clocks'
        opt '+4 Clocks'
        opt '+5 Clocks'
        opt '+6 Clocks'
        opt '+7 Clocks'
        opt '+8 Clocks'
        opt '+10 Clocks'
        opt '+12 Clocks'
        opt '+14 Clocks'
        opt '+16 Clocks'
        opt '+20 Clocks'
        opt '+24 Clocks'
        opt '+32 Clocks'
        opt '+48 Clocks'
        opt '+64 Clocks'

; help text: lines of up to 36 characters, separated by 10, ended by 0
h_det:  db 'How fast the computer runs while', 10
        db 'the front panel Turbo button is', 10
        db 'off. 8 MHz runs close to the speed', 10
        db 'of an IBM AT, for old games that', 10
        db 'run too fast.', 0
h_dram: db 'Wait states added to each memory', 10
        db 'access while the caches are off.', 10
        db 'Sets how fast the memory count', 10
        db 'runs at power-on. Auto picks it', 10
        db 'from the CPU bus speed.', 0
h_spin: db 'Waits for the hard disks to spin', 10
        db 'up before the power-on test, as on', 10
        db 'a cold start.', 0
h_logo: db 'Shows the LFTech logo in the top', 10
        db 'right corner during the power-on', 10
        db 'test.', 0

s_title1: db 'BIOS SETUP PROGRAM - LFTECH FEATURES SETUP', 0
s_title2: db '(C)1994 LFTech, All Rights Reserved', 0
s_avail:  db ' Available Options ', 0
s_keys1:  db ' ESC:Exit  ', 0x18, 0x19, ':Sel  PgUp/PgDn:Modify  +/-:Modify ', 0
s_keys2:  db ' F5:Old Values  F6:LFTech Setup Defaults ', 0

vals:   times NITEMS db 0           ; current values
old:    times NITEMS db 0           ; values on entry (F5)
sel:    db 0
frameA: db 0x1f
headA:  db 0x1f
bodyA:  db 0x1f
hiA:    db 0x70
curpos: dw 0
curtyp: dw 0

; ---------------------------------------------------------------- code
main:
    pushad
    push ds
    push es
    push cs
    pop ds
    cld

    ; save the screen and cursor
    push ds
    pop es
    mov ah, 3
    xor bh, bh
    int 0x10
    mov [curpos], dx
    mov [curtyp], cx
    push ds
    mov ax, 0xb800
    mov ds, ax
    xor si, si
    mov di, savebuf
    mov cx, 2000
    rep movsw
    pop ds
    mov ah, 1                       ; hide the cursor
    mov cx, 0x2000
    int 0x10

    call sample_colours
    call fill_year
    call fill_auto
    call load_cmos
    mov si, vals
    mov di, old
    mov cx, NITEMS
    push ds
    pop es
    rep movsb

    call draw_frame
.loop:
    call draw_items
    xor ah, ah
    int 0x16
    cmp ah, 0x01                    ; Esc
    je .exit
    cmp ah, 0x48                    ; Up
    je .up
    cmp ah, 0x50                    ; Down
    je .down
    cmp ah, 0x49                    ; PgUp
    je .prev
    cmp al, '-'
    je .prev
    cmp ah, 0x51                    ; PgDn
    je .next
    cmp al, '+'
    je .next
    cmp ah, 0x3f                    ; F5
    je .oldv
    cmp ah, 0x40                    ; F6
    je .defv
    jmp .loop
.up:
    dec byte [sel]
    jns .loop
    mov byte [sel], NITEMS - 1
    jmp .loop
.down:
    inc byte [sel]
    cmp byte [sel], NITEMS
    jb .loop
    mov byte [sel], 0
    jmp .loop
.prev:
    call cur_item
    dec byte [vals + bx]
    jns .loop
    dec al
    mov [vals + bx], al
    jmp .loop
.next:
    call cur_item
    inc byte [vals + bx]
    cmp [vals + bx], al
    jb .loop
    mov byte [vals + bx], 0
    jmp .loop
.oldv:
    mov si, old
.copyv:
    mov di, vals
    push ds
    pop es
    mov cx, NITEMS
    rep movsb
    jmp .loop
.defv:
    call set_defaults
    jmp .loop

.exit:
    call save_cmos
    ; put the screen and cursor back
    mov ax, 0xb800
    mov es, ax
    xor di, di
    mov si, savebuf
    mov cx, 2000
    rep movsw
    mov ah, 1
    mov cx, [curtyp]
    int 0x10
    mov ah, 2
    xor bh, bh
    mov dx, [curpos]
    int 0x10
    pop es
    pop ds
    popad
    retf

; BX = selected item, AL = its option count
cur_item:
    movzx bx, byte [sel]
    mov al, [cnt_tab + bx]
    cmp bl, 1
    jne .r
    mov al, [dram_max]
    add al, 2                       ; Auto, then None..dram_max
.r:
    ret

set_defaults:
    mov byte [vals + 0], 0
    mov byte [vals + 1], 0          ; Auto
    mov byte [vals + 2], 0
    mov byte [vals + 3], 0
    ret

; ---------------------------------------------------------------- CMOS
cmos_rd:                            ; AL = index -> AL = value
    or al, 0x80
    out 0x70, al
    jmp $+2
    in al, 0x71
    ret
cmos_wr:                            ; AL = index, AH = value
    or al, 0x80
    out 0x70, al
    jmp $+2
    mov al, ah
    out 0x71, al
    ret

load_cmos:
    call set_defaults
    mov al, [cmos_a]
    call cmos_rd
    mov dl, al
    mov al, [cmos_b]
    call cmos_rd
    mov dh, al
    mov al, dl
    and al, 0xf0
    cmp al, 0x80
    jne .r
    mov al, dh
    and al, 0xe0
    cmp al, 0xa0
    jne .r
    mov al, dh
    and al, 0x1f
    cmp al, 0x1f                    ; Auto
    je .auto
    cmp al, [dram_max]
    jbe .dok
    mov al, [dram_max]
.dok:
    inc al
    jmp .dset
.auto:
    xor al, al
.dset:
    mov [vals + 1], al
    mov al, dl
    and al, 3
    mov [vals + 0], al
    mov al, dl
    shr al, 2
    and al, 1
    mov [vals + 2], al
    mov al, dl
    shr al, 3
    and al, 1
    mov [vals + 3], al
.r:
    ret

save_cmos:
    mov ah, [vals + 3]
    shl ah, 1
    or ah, [vals + 2]
    shl ah, 2
    or ah, [vals + 0]
    or ah, 0x80
    mov al, [cmos_a]
    call cmos_wr
    mov ah, [vals + 1]
    dec ah                          ; option 0 (Auto) -> 1Fh
    and ah, 0x1f
    or ah, 0xa0
    mov al, [cmos_b]
    call cmos_wr
    ret

; ---------------------------------------------------------------- screen
; the colours of the screen we were called from
sample_colours:
    push ds
    mov ax, 0xb800
    mov ds, ax
    mov al, [1]                     ; top left corner: the frame
    mov bl, [(5 * 80 + 2) * 2 + 1]  ; inside: the body
    mov bh, al
    mov si, (1 * 80 + 1) * 2        ; first letter of the title
    mov cx, 78
.f: cmp byte [si], ' '
    ja .g
    add si, 2
    loop .f
    mov ah, al
    jmp .h
.g: mov ah, [si + 1]
.h: pop ds
    mov [frameA], bh
    mov [headA], ah
    mov [bodyA], bl
    mov al, bl                      ; highlight: the body colour as text on black
    shr al, 4
    and al, 7
    jz .inv
    or al, 8
    jmp .s
.inv:
    mov al, 0x70
.s: mov [hiA], al
    ret

; DH = row, DL = column -> ES:DI
at_rc:
    push ax
    mov ax, 0xb800
    mov es, ax
    mov al, dh
    mov ah, 80
    mul ah
    add al, dl
    adc ah, 0
    shl ax, 1
    mov di, ax
    pop ax
    ret

; DH,DL position, AL char, AH attr, CX count
fill:
    call at_rc
    rep stosw
    ret

; DH,DL position, DS:SI string, AH attr; stops at 0 or 10
puts:
    call at_rc
.l: lodsb
    or al, al
    jz .e
    cmp al, 10
    je .e
    stosw
    jmp .l
.e: ret

; centred string: DH row, DS:SI string, AH attr
putc_c:
    push si
    xor cx, cx
.l: lodsb
    or al, al
    jz .e
    inc cx
    jmp .l
.e: pop si
    mov dl, 80
    sub dl, cl
    shr dl, 1
    jmp puts

; horizontal rule: DH row, AL left, BL middle, BH right (middle at col 40,
; BL = 0 for none), line char is double
hrule:
    push ax
    mov ah, [frameA]
    mov dl, 0
    call at_rc
    stosw
    mov al, 0xcd
    mov cx, 78
    rep stosw
    mov al, bh
    stosw
    or bl, bl
    jz .n
    mov dl, 40
    call at_rc
    mov al, bl
    stosw
.n: pop ax
    ret

draw_frame:
    ; whole screen in the body colour, header rows in the header colour
    mov dx, 0
    mov ax, 0x0020
    mov ah, [bodyA]
    mov cx, 2000
    call fill
    mov dx, 0x0100
    mov ah, [headA]
    mov cx, 160
    call fill
    ; frame
    mov dh, 0
    mov al, 0xc9
    mov bx, 0xbb00
    call hrule
    mov dh, 3
    mov al, 0xcc
    mov bx, 0xb9cb
    call hrule
    mov dh, 22
    mov al, 0xcc
    mov bx, 0xb9ca
    call hrule
    mov dh, 23
    mov al, 0xcc
    mov bx, 0xb900
    call hrule
    mov dh, 24
    mov al, 0xc8
    mov bx, 0xbc00
    call hrule
    mov dh, 1
.side:
    cmp dh, 3
    je .ns
    mov ah, [frameA]
    mov al, 0xba
    mov dl, 0
    call at_rc
    stosw
    mov dl, 79
    call at_rc
    stosw
    cmp dh, 4
    jb .ns
    cmp dh, 21
    ja .ns
    mov dl, 40
    call at_rc
    stosw
.ns:
    inc dh
    cmp dh, 21
    jbe .side
    ; titles and key lines
    mov ah, [headA]
    mov dh, 1
    mov si, s_title1
    call putc_c
    mov dh, 2
    mov si, s_title2
    call putc_c
    mov dh, 23
    mov si, s_keys1
    call keyline
    mov dh, 24
    mov si, s_keys2
    call keyline
    ret

keyline:                            ; legend set into a rule: ╡ text ╞
    mov ah, [headA]
    push si
    xor cx, cx
.l: lodsb
    or al, al
    jz .e
    inc cx
    jmp .l
.e: pop si
    mov dl, 78
    sub dl, cl
    shr dl, 1
    call at_rc
    mov al, 0xb5
    push ax
    mov ah, [frameA]
    stosw
    pop ax
    inc dl
    call puts
    mov ah, [frameA]
    mov al, 0xc6
    stosw
    ret

draw_items:
    xor bx, bx
.item:
    mov dh, bl
    add dh, 4
    mov ah, [bodyA]
    cmp bl, [sel]
    jne .c
    mov ah, [hiA]
.c: push ax
    mov dl, 1                       ; clear the line
    mov al, ' '
    mov cx, 39
    call fill
    pop ax
    push bx
    shl bx, 1
    mov si, [lbl_tab + bx]
    pop bx
    mov dl, 2
    call puts
    mov dl, 28
    call at_rc
    mov al, ':'
    stosw
    push ax
    call opt_str                    ; SI = value string
    pop ax
    mov dl, 30
    call puts
    inc bx
    cmp bx, NITEMS
    jb .item

    ; right side: the options of the selected item, then its help
    mov dx, 0x0429
    mov al, ' '
    mov ah, [bodyA]
.clr:
    push dx
    mov cx, 38
    call fill
    pop dx
    inc dh
    cmp dh, 22
    jb .clr
    mov dx, 0x0429
    mov ah, [headA]
    mov si, s_avail
    call puts
    movzx bx, byte [sel]
    push bx
    call cur_item                   ; AL = count
    pop bx
    mov cl, al
    xor ch, ch                      ; CH = option index
.opt:
    push cx
    mov dh, ch                      ; nine to a column
    mov dl, 43
    cmp dh, 10
    jb .col
    sub dh, 10
    mov dl, 60
.col:
    add dh, 6
    mov ah, [bodyA]
    mov al, ' '
    cmp ch, [vals + bx]
    jne .m
    mov al, 0x10
.m: call at_rc
    stosw
    push bx
    mov al, ch
    call opt_n
    pop bx
    add dl, 2
    mov ah, [bodyA]
    call puts
    pop cx
    inc ch
    cmp ch, cl
    jb .opt
    mov dh, 16                      ; help below the options
    shl bx, 1
    mov si, [help_tab + bx]
    mov ah, [bodyA]
.hl:
    mov dl, 42
    call puts
    inc dh
    cmp byte [si - 1], 0
    jne .hl
    ret

; the year from the real-time clock into the copyright line
fill_year:
    mov ah, 4
    int 0x1a
    jc .r
    mov di, s_title2 + 3
    mov al, ch
    call bcd2
    mov al, cl
    call bcd2
.r: ret
bcd2:                               ; AL = BCD byte -> two digits at [DI]
    mov ah, al
    shr al, 4
    add al, '0'
    mov [di], al
    mov al, ah
    and al, 0x0f
    add al, '0'
    mov [di + 1], al
    add di, 2
    ret

; 'Auto (+n)' from the clocks the board picked
fill_auto:
    mov di, o_dram + 4
    mov al, [auto_clk]
    mov word [di], ' (' 
    mov byte [di + 2], '+'
    add di, 3
    xor ah, ah
    mov bl, 10
    div bl
    or al, al
    jz .one
    add al, '0'
    mov [di], al
    inc di
.one:
    add ah, '0'
    mov [di], ah
    mov word [di + 1], ')'
    ret

; SI = string of the value of item BX
opt_str:
    mov al, [vals + bx]
; SI = string of option AL of item BX
opt_n:
    push bx
    shl bx, 1
    mov si, [opt_tab + bx]
    pop bx
    mov ah, 11
    mul ah
    add si, ax
    mov ah, 0
    ret

    align 16
savebuf:                            ; 4000 bytes, not part of the image

; lftech_setup_rom.h is generated from the assembled image with:
;   python3 -c "d=open('lftech_setup.bin','rb').read(); print('/* Generated from lftech_setup.asm with nasm; see the end of that file. */'); print('static const uint8_t lftech_setup_rom[] = {'); [print('    '+', '.join('0x%02x'%b for b in d[i:i+12])+',') for i in range(0,len(d),12)]; print('};')" > lftech_setup_rom.h
