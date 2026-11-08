; SPDX-License-Identifier: MIT
;
; la_dump(buf, t0): a PRU's copier in dump mode, which never returns.
;
; It reads the other PRU's blocks of samples in raw mode (la_raw.asm) from
; the scratchpad banks the two share, 10, 11, and 12, in turn, as la_conv.asm
; does, and copies each, as it is, into the burst buffer in DDR
; (ai64_la.h): one XIN, one 80-byte store. So what it costs a block is the
; same whatever the pins do, where the converter's grows with every change,
; and Linux finds the changes after (ai64 la burst). If it finds a block
; newer than the one it wants, the blocks between are lost: it takes the
; newer one, and Linux sees the jump in the numbers.
;
; First the header: AI64_LA_BURST_MAGIC, t0, the first block's number, and
; the slots. Each block's number goes out XOR t0, so a slot an earlier
; burst left reads as no block of this one.
;
;   r1 - r19  a block's samples    r20  its number, then XOR t0
;   r21 - r24 the header, then r22 t0, r24 scratch
;   r23  the block wanted          r25  the slot's offset in the buffer
;   r26  the buffer's address      r27  the offset past the last slot
;   r0   0, always, for the XINs: la_raw.asm has why
;   r14  buf, r15  t0's address    (the C arguments)

	.text
	.global la_dump

; Bank b: wait for block r23 in it, store it in the next slot, on to bank nb.
DBANK	.macro	b, nb
dw_:b::
	XIN	b, &r1, 80
	QBEQ	ds_:b:, r20, r23
	SUB	r24, r20, r23
	QBBS	dw_:b:, r24, 31
	; Newer: the ones between are lost.
	MOV	r23, r20
ds_:b::
	XOR	r20, r20, r22
	SBBO	&r1, r26, r25, 80
	ADD	r25, r25, 80
	QBGT	dk_:b:, r25, r27
	LDI	r25, 64
dk_:b::
	ADD	r23, r23, 1
	QBA	dw_:nb:
	.endm

la_dump:
	LDI	r0, 0
	MOV	r26, r14
	; No block yet in any bank, as la_conv.asm marks it.
	LDI32	r20, 0xfffffff0
	XOUT	10, &r20, 4
	XOUT	11, &r20, 4
	XOUT	12, &r20, 4
la_dump_first:
	XIN	10, &r20, 4
	QBBS	la_dump_first, r20, 31
	MOV	r23, r20
	; The raw PRU wrote t0 before its first block.
	LBBO	&r22, r15, 0, 4
	LDI32	r21, 0x42525354
	LDI32	r24, 419429
	SBBO	&r21, r26, 0, 16
	LDI	r25, 64
	LDI32	r27, 64 + 419429 * 80
	DBANK	10, 11
	DBANK	11, 12
	DBANK	12, 10
