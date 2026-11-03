; SPDX-License-Identifier: MIT
;
; A PRU's converters in conv mode, which never return:
;
;   la_conv(ring, seq, t0)           for la_raw, a sample a register
;   la_conv16(ring, seq, t0, shift)  for la_raw16_0 or _8, two a register,
;                                    each 16 pins from pin shift (0 or 8)
;
; It reads the other PRU's blocks of samples in raw mode (la_raw.asm) from
; the scratchpad banks the two share, 10, 11, and 12, in turn, as they
; come, compares each sample with the one before, and for each change
; writes an edge record into the PRU's ring in shared RAM, as that PRU does
; in edge mode (la_map.h): the time of the first sample with the new pins,
; from t0 and the sample's number, the pins, and the seq, from the one
; given. So the RTU, Linux, and the viewer take raw mode's records as they
; take edge mode's.
;
; A block with no change takes about 26 cycles: of 38, a sample a register,
; or of 76, two. A register is compared with the one before it whole: with
; two samples, the one before holds its last sample twice, so a register
; that matches it holds no change. One that does not goes to its handler,
; which records each change in it, and then writes its last sample over its
; first, for the next register's compare. A change takes about 7 cycles
; more, 9 at two a register. Behind, the converter has two blocks more in
; the other banks before the PRU writes over one; if it finds a block newer
; than the one it wants, it writes a gap record, the samples lost, and takes
; up from there.
;
;   r1 - r19  a block's samples    r20  its number    r21  the pins' shift
;   r22  the register before r1, its last sample twice
;   r23  the block wanted          r24  the time of its first sample
;   r25  scratch, the record's offset
;   r26  the ring's local address
;   r27 - r29  a record: time, pins, seq
;   r0   0, always, for the XINs: la_raw.asm has why

	.text
	.global la_conv
	.global la_conv16

; Store the record in r27 - r29 and move the seq on.
PUT	.macro
	LSL	r25, r29, 17
	LSR	r25, r25, 17
	SBBO	&r27, r26, r25, 12
	ADD	r29, r29, 16
	.endm

; r27 = r24 + off: ADD takes an immediate up to 255.
TIME	.macro	off
	.if	off <= 255
	ADD	r27, r24, off
	.else
	ADD	r27, r24, 255
	ADD	r27, r27, off - 255
	.endif
	.endm

; Compare register k of bank b with the one before it.
CHK	.macro	b, k, W
	.eval	k - 1, j
	.if	k == 1
	QBNE	e:W:_:b:_:k:, r:k:, r22
	.else
	QBNE	e:W:_:b:_:k:, r:k:, r:j:
	.endif
c:W:_:b:_:k::
	.endm

; The record for a change at register k of bank b, W bits a sample.
EDGE	.macro	b, k, W
e:W:_:b:_:k::
	.if	W == 32
	TIME	8 * (:k: - 1)
	MOV	r28, r:k:
	PUT
	.else
	.eval	k - 1, j
	.if	k == 1
	QBEQ	h:W:_:b:_:k:, r:k:.w0, r22.w2
	.else
	QBEQ	h:W:_:b:_:k:, r:k:.w0, r:j:.w2
	.endif
	TIME	16 * (:k: - 1)
	LSL	r28, r:k:.w0, r21
	PUT
h:W:_:b:_:k::
	QBEQ	u:W:_:b:_:k:, r:k:.w2, r:k:.w0
	TIME	16 * (:k: - 1) + 8
	LSL	r28, r:k:.w2, r21
	PUT
u:W:_:b:_:k::
	MOV	r:k:.w0, r:k:.w2
	.endif
	QBA	c:W:_:b:_:k:
	.endm

; r22 from r1's first sample, as if nothing changed at it.
PREV	.macro	W
	.if	W == 32
	MOV	r22, r1
	.else
	MOV	r22.w0, r1.w0
	MOV	r22.w2, r1.w0
	.endif
	.endm

; Bank b holds another block than r23: an older one, not written yet, or
; a newer one, and the blocks between are lost.
LATE	.macro	b, W
n:W:_:b::
	SUB	r25, r20, r23
	QBBS	w:W:_:b:, r25, 31
	; r24 moves on by the lost blocks' time, and a gap record says how
	; many samples: 152 ns and 19 a block, or 304 ns and 38.
	.if	W == 32
	LSL	r27, r25, 7
	ADD	r24, r24, r27
	LSL	r27, r25, 4
	ADD	r24, r24, r27
	LSL	r27, r25, 3
	ADD	r24, r24, r27
	LSL	r28, r25, 4
	LSL	r27, r25, 1
	ADD	r28, r28, r27
	ADD	r28, r28, r25
	.else
	LSL	r27, r25, 8
	ADD	r24, r24, r27
	LSL	r27, r25, 5
	ADD	r24, r24, r27
	LSL	r27, r25, 4
	ADD	r24, r24, r27
	LSL	r28, r25, 5
	LSL	r27, r25, 2
	ADD	r28, r28, r27
	LSL	r27, r25, 1
	ADD	r28, r28, r27
	.endif
	LDI32	r27, 0x81000000
	OR	r28, r28, r27
	MOV	r27, r24
	PUT
	MOV	r23, r20
	PREV	W
	QBA	g:W:_:b:
	.endm

; Bank b: wait for block r23 in it, then its 19 registers, then on to
; bank nb; and, out of the way but within a branch's reach, the records
; for its changes and the gap for a late block.
BANK	.macro	b, nb, W
w:W:_:b::
	XIN	b, &r1, 80
g:W:_:b::
	QBNE	n:W:_:b:, r20, r23
	.asg	1, k
	.loop	19
	CHK	b, k, W
	.eval	k + 1, k
	.endloop
	MOV	r22, r19
	ADD	r23, r23, 1
	.if	W == 32
	ADD	r24, r24, 152
	.else
	ADD	r24, r24, 255
	ADD	r24, r24, 49
	.endif
	JMP	w:W:_:nb:
	.asg	1, k
	.loop	19
	EDGE	b, k, W
	.eval	k + 1, k
	.endloop
	LATE	b, W
	.endm

; A converter: the start, then the three banks in turn, for W-bit samples.
CONV	.macro	name, W
:name::
	LDI	r0, 0
	MOV	r26, r14
	MOV	r29, r15
	MOV	r25, r16
	MOV	r21, r17
	; No block yet in any bank: a number older than any the PRU writes,
	; however its difference from the one wanted is taken (LATE): -16.
	LDI32	r20, 0xfffffff0
	XOUT	10, &r20, 4
	XOUT	11, &r20, 4
	XOUT	12, &r20, 4
:name:_first:
	; The first block the PRU puts in bank 10: block 0, or a later one if
	; it started first. Take up from there.
	XIN	10, &r20, 4
	QBBS	:name:_first, r20, 31
	MOV	r23, r20
	; The PRU wrote t0 before its first block; block r23 starts 152 or
	; 304 ns a block after it. Its first sample's pins start the stream:
	; a record of every pin, then the changes after. 24 ns more puts the
	; times where edge mode puts them: it reads IEP0 a few cycles after it
	; sees a change, and t0 is read a few before the first sample. Slice
	; 1's hardware PWM, on IEP1 from the same clock, read 24 to 28 ns later
	; in edge mode than in raw mode without it.
	LBBO	&r24, r25, 0, 4
	ADD	r24, r24, 24
	.if	W == 32
	LSL	r27, r23, 7
	ADD	r24, r24, r27
	LSL	r27, r23, 4
	ADD	r24, r24, r27
	LSL	r27, r23, 3
	ADD	r24, r24, r27
	.else
	LSL	r27, r23, 8
	ADD	r24, r24, r27
	LSL	r27, r23, 5
	ADD	r24, r24, r27
	LSL	r27, r23, 4
	ADD	r24, r24, r27
	.endif
	XIN	10, &r1, 4
	PREV	W
	MOV	r27, r24
	.if	W == 32
	MOV	r28, r1
	.else
	LSL	r28, r1.w0, r21
	.endif
	PUT
	BANK	10, 11, W
	BANK	11, 12, W
	BANK	12, 10, W
	.endm

	CONV	la_conv, 32
	CONV	la_conv16, 16
