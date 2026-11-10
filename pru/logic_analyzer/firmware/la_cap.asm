; SPDX-License-Identifier: MIT
;
; PRU0's sampling loops, which never return (ai64_la.h has the ring):
;
;   la_cap_<rate><lane>(mask, key, &t0, k)
;
; Every period cycles, without a gap, it takes R31 through the mask into
; the next sample of r2 - r16, a block of 15 words: a word a sample, or
; half a word, or a byte, by the lane. After the last sample of a block one
; XOUT hands r2 - r19 to XFR2VBUS's write channel 0 (SPRUJ61 7.3.1): the
; 64 bytes of r2 - r17, the samples and the tag, to the address in r18 and
; r19. XFR2VBUS writes them to DDR on its own, so the PRU never waits for
; DDR. If it is still busy with the block before, when the SoC holds DDR
; for a while, this block is dropped, and its slot keeps what it held:
; Linux sees the tag is not this block's.
;
; The instruction after each sample is the block's work, one each: the
; tag's ADD, the address's ADD and wrap, the XIN of XFR2VBUS's status two
; samples before the end, the QBBS on it, the XOUT after the last sample,
; the jump back; or a MOV that does nothing. Slower rates add a delay after
; each, of a fixed count of cycles, so sample n of the run is exactly n
; periods after the first, whose IEP0 time, less the few cycles to it, goes
; in t0 and t0_hi before the loop starts. The drop path takes the same
; cycles as the XOUT path.
;
; The rates: 2 cycles a sample (125 MS/s), with no delay; 4 (62.5 MS/s),
; with two MOVs; and 4 + 2k (10 cycles and more), with a MOV, a MOV, and k
; passes of a two-cycle loop. Lanes take fewer bytes at the two fastest,
; where the bytes are many; slower ones take every bit (ai64_la.h).
;
;   r2 - r16  the block's samples     r17  its tag      r18  its address
;   r19  0, the address's upper bits  r20  XFR2VBUS's status (XIN)
;   r22  the mask    r23  the delay's count    r24  k
;   r0   0, always: with ICSSG_SPP_REG's XFR_SHIFT_EN set, an XIN or XOUT
;        shifts its registers by R0[4:0]
;   r14  mask, r15  key, r16  &t0, then t0_hi and run, r17  k  (the C arguments)

	.text

XID	.set	0x62

; Sample s of a block (from 1), W bits wide, from R31's field L.
SAMPLE	.macro	s, W, L
	.if	W == 32
	.eval	s + 1, rk
	AND	r:rk:, r31, r22
	.elseif	W == 16
	.eval	(s + 1) / 2 + 1, rk
	.if	(s & 1) == 1
	AND	r:rk:.w0, r31.:L:, r22.:L:
	.else
	AND	r:rk:.w2, r31.:L:, r22.:L:
	.endif
	.else
	.eval	(s + 3) / 4 + 1, rk
	.eval	(s - 1) & 3, by
	AND	r:rk:.b:by:, r31.:L:, r22.:L:
	.endif
	.endm

; The work after sample s of S, for the loop named id.
WORK	.macro	s, S, id
	.if	s == 2
	ADD	r17, r17, 1
	.elseif	s == 3
	ADD	r18, r18, 64
	.elseif	s == 4
	; The ring is 32 MiB at 0xac000000: bit 25 set is one past its end.
	CLR	r18, r18, 25
	.elseif	s == S - 2
	XIN	XID, &r20, 4
	.elseif	s == S - 1
	QBBS	d_:id:, r20, 0
	.elseif	s == S
	XOUT	XID, &r2, 72
	.else
	MOV	r0, r0
	.endif
	.endm

; The cycles after a sample's work: none, two, or 2 + 2k.
DELAY	.macro	kind, id, s
	.if	kind == 1
	MOV	r0, r0
	MOV	r0, r0
	.elseif	kind == 2
	MOV	r23, r24
	MOV	r0, r0
w_:id:_:s::
	SUB	r23, r23, 1
	QBNE	w_:id:_:s:, r23, 0
	.endif
	.endm

; A loop: W-bit samples from R31's field L, delay kind.
CAP	.macro	name, W, L, kind
	.global	:name:
	.eval	15 * 32 / W, S
:name::
	LDI	r0, 0
	MOV	r22, r14
	MOV	r24, r17
	SUB	r17, r15, 1
	LDI32	r18, 0xac000000 - 64
	LDI	r19, 0
	; IEP0's count, both halves, read again if the upper one moved.
	LDI32	r29, 0x4c415255
	LDI32	r25, 0x0002e010
t_:name::
	LBBO	&r28, r25, 4, 4
	LBBO	&r27, r25, 0, 4
	LBBO	&r26, r25, 4, 4
	QBNE	t_:name:, r26, r28
	SBBO	&r27, r16, 0, 12
	SAMPLE	1, W, L
	MOV	r0, r0
l_:name::
	DELAY	kind, :name:, 1
	.asg	2, s
	.loop	S - 1
	SAMPLE	s, W, L
	WORK	s, S, :name:
	DELAY	kind, :name:, s
	.eval	s + 1, s
	.endloop
	SAMPLE	1, W, L
	QBA	l_:name:
	; Dropped: the same cycles from the QBBS on, with no XOUT.
d_:name::
	.eval	S - 1, s
	DELAY	kind, x:name:, s
	SAMPLE	S, W, L
	MOV	r0, r0
	DELAY	kind, x:name:, S
	SAMPLE	1, W, L
	QBA	l_:name:
	.endm

	CAP	la_cap_0all, 32, w0, 0
	CAP	la_cap_0lo16, 16, w0, 0
	CAP	la_cap_0mid16, 16, w1, 0
	CAP	la_cap_0b0, 8, b0, 0
	CAP	la_cap_0b1, 8, b1, 0
	CAP	la_cap_1all, 32, w0, 1
	CAP	la_cap_1lo16, 16, w0, 1
	CAP	la_cap_1mid16, 16, w1, 1
	CAP	la_cap_1b0, 8, b0, 1
	CAP	la_cap_1b1, 8, b1, 1
	CAP	la_cap_2all, 32, w0, 2
