; SPDX-License-Identifier: MIT
;
; la_pack(dst, src, n): an RTU's copy of n edge records from its PRU's ring
; in shared RAM, 16 bytes each (struct la_srec), into its ring in DDR, 8
; bytes each: time and state, without the seq (la_rtu.c). Four records a
; load, packed into one 32-byte store, then one at a time. A word at a
; time, as C writes it, the RTU copied about 9 million records a second,
; half what its PRU can write.
;
;   r14  dst, r15  src, r16  n     (the C arguments)
;   r0   src, r1   dst, r4   n     (r4 saved: the C caller keeps it)
;   r14 - r29  four records in flight

	.text
	.global la_pack
la_pack:
	SUB	r2, r2, 4
	SBBO	&r4, r2, 0, 4
	MOV	r0, r15
	MOV	r1, r14
	MOV	r4, r16
la_pack4:
	QBGT	la_pack1, r4, 4
	LBBO	&r14, r0, 0, 64
	MOV	r16, r18
	MOV	r17, r19
	MOV	r18, r22
	MOV	r19, r23
	MOV	r20, r26
	MOV	r21, r27
	SBBO	&r14, r1, 0, 32
	ADD	r0, r0, 64
	ADD	r1, r1, 32
	SUB	r4, r4, 4
	QBA	la_pack4
la_pack1:
	QBEQ	la_pack_done, r4, 0
	LBBO	&r14, r0, 0, 8
	SBBO	&r14, r1, 0, 8
	ADD	r0, r0, 16
	ADD	r1, r1, 8
	SUB	r4, r4, 1
	QBA	la_pack1
la_pack_done:
	LBBO	&r4, r2, 0, 4
	ADD	r2, r2, 4
	JMP	r3.w2
