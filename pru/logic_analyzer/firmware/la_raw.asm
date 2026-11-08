; SPDX-License-Identifier: MIT
;
; A PRU's raw sampling loops, which never return: the analyzer's raw mode
; (la_map.h), with no edge records here, so no time lost to one.
;
;   la_raw(mask, t0)      every pin, a sample a register
;   la_raw16_0(mask, t0)  pins 0 - 15, two samples a register
;   la_raw16_8(mask, t0)  pins 8 - 23, two samples a register
;   la_raw8_0(mask, t0)   pins 0 - 7, four samples a register, for a burst
;   la_raw8_8(mask, t0)   pins 8 - 15, the same (la_dump.asm)
;
; Every two cycles, 8 ns, without a gap, it takes R31 through the mask
; (all of it, or the 16 pins) into the next register, or half register, of
; r1 - r19; each block of 19 registers goes to a scratchpad bank in one
; XOUT, in turn banks 10, 11, and 12, with the block's number in r20. The
; other PRU, which shares the banks, reads them as they come and writes the
; edge records (la_conv.asm). Two samples a register make a block 304 ns
; long, not 152, so the other PRU has half as many registers to compare in
; the same time, and keeps up with twice as many changes.
;
; The instruction between two samples is the XOUT, the block number's ADD,
; the jump back, or a MOV that does nothing: so sample n of the run is
; taken exactly 8n ns after the first, whose IEP0 time, less the few cycles
; to it, goes in t0 before the loop starts.
;
;   r0   0, always: with ICSSG_SPP_REG's XFR_SHIFT_EN set, an XIN or XOUT
;        shifts its registers by R0[4:0], and the other PRU's XIN must find
;        r1 - r20 where this XOUT put them
;   r14  mask      the PRU's GPI pins to record (R31's interrupt bits never)
;   r15  t0        the address of the word for the first sample's time
;   r1 - r19  the block      r20  its number, from 0      r22  the mask

	.text
	.global la_raw
	.global la_raw16_0
	.global la_raw16_8
	.global la_raw8_0
	.global la_raw8_8

; Sample s of a block (from 1), W bits wide, from R31's field L.
SAMPLE	.macro	s, W, L
	.if	W == 32
	AND	r:s:, r31, r22
	.else
	.eval	(s + 1) / 2, rk
	.if	(s & 1) == 1
	AND	r:rk:.w0, r31.:L:, r22.:L:
	.else
	AND	r:rk:.w2, r31.:L:, r22.:L:
	.endif
	.endif
	.endm

; Samples first to the last of a block for bank b, each followed by its
; spare cycle: the XOUT after the last, the block number's ADD after the 2nd.
BLOCK	.macro	b, first, W, L
	.if	W == 32
	.eval	19, last
	.else
	.eval	38, last
	.endif
	.asg	first, s
	.loop	last + 1 - first
	SAMPLE	s, W, L
	.if	s == last
	XOUT	b, &r1, 80
	.elseif	s == 2
	ADD	r20, r20, 1
	.else
	MOV	r0, r0
	.endif
	.eval	s + 1, s
	.endloop
	.endm

RAW	.macro	name, W, L
:name::
	LDI	r0, 0
	MOV	r22, r14
	LDI32	r20, 0xffffffff
	LDI32	r23, 0x0002e010
	LBBO	&r24, r23, 0, 4
	SBBO	&r24, r15, 0, 4
	SAMPLE	1, W, L
	MOV	r0, r0
:name:_loop:
	BLOCK	10, 2, W, L
	BLOCK	11, 1, W, L
	BLOCK	12, 1, W, L
	SAMPLE	1, W, L
	QBA	:name:_loop
	.endm

	RAW	la_raw, 32, w0
	RAW	la_raw16_0, 16, w0
	RAW	la_raw16_8, 16, w1

; Four samples a register, a byte each, 76 a block, 608 ns: for a burst,
; where the other PRU copies the bytes as they are, and fewer bytes keep
; it up. Unrolled three times, as above, it would not fit in IRAM, so the
; three banks share one body, samples 3 to 75, and the spare slot after
; sample 75 jumps through r23 to a tail of the bank's own: sample 76, the
; XOUT, sample 1, r23 to the next bank's tail, sample 2, and back. The
; jumps take spare slots, so the samples stay exactly 8 ns apart.
SAMPLE8	.macro	s, L
	.eval	(s + 3) / 4, rk
	.eval	(s - 1) & 3, by
	AND	r:rk:.b:by:, r31.:L:, r22.:L:
	.endm

TAIL8	.macro	name, b, nb, L
:name:_t:b::
	SAMPLE8	76, L
	XOUT	b, &r1, 80
	SAMPLE8	1, L
	LDI	r23, $CODE(:name:_t:nb:)
	SAMPLE8	2, L
	QBA	:name:_body
	.endm

RAW8	.macro	name, L
:name::
	LDI	r0, 0
	MOV	r22, r14
	LDI32	r20, 0xffffffff
	LDI32	r23, 0x0002e010
	LBBO	&r24, r23, 0, 4
	SBBO	&r24, r15, 0, 4
	SAMPLE8	1, L
	LDI	r23, $CODE(:name:_t10)
	SAMPLE8	2, L
	MOV	r0, r0
:name:_body:
	SAMPLE8	3, L
	ADD	r20, r20, 1
	.asg	4, s
	.loop	71
	SAMPLE8	s, L
	MOV	r0, r0
	.eval	s + 1, s
	.endloop
	SAMPLE8	75, L
	JMP	r23
	TAIL8	name, 10, 11, L
	TAIL8	name, 11, 12, L
	TAIL8	name, 12, 10, L
	.endm

	RAW8	la_raw8_0, b0
	RAW8	la_raw8_8, b1
