; SPDX-License-Identifier: MIT
;
; la_sample(ring, mask, seq): a PRU's sampling loop in edge mode, never
; returns; its first record's seq is seq (la_map.h).
;
; Two cycles a pass, 8 ns: read the GPI pins (R31) through the mask, and
; loop while they match the last state. On a change it reads IEP0's count
; and stores the edge record, time, state, and seq, in one store into the
; ring in shared RAM (la_map.h): ten cycles, 40 ns, from one record's time
; to the next one's when the pins change again at once. A change in that
; time waits for the next pass; a pulse shorter than that, right after an
; edge, is not seen. The first pass always writes a record, so a stream
; starts with every pin's state.
;
; Two copies of the loop, each with its own registers for the record it
; writes, take turns: the state just stored is the one the other loop
; compares against, so an edge needs no move, and one in two no jump.
; There is no write count to store: the seq in each record says it is new
; (la_map.h).
;
; The first look at the pins after a record comes straight after its
; store. A change there came while the PRU was busy with the record, so
; its time is late, and a pulse in that time may be missing: its record
; goes out with AI64_LA_LATE (bit 23, ai64_la.h) set, through a third and
; fourth copy, two cycles longer, which Linux counts. The look takes the
; place of the loop's first pass, so an edge on time costs no more.
;
;   r14  ring      the ring's local address, LA_SRING_SIZE bytes
;   r15  mask      the PRU's GPI pins to record
;   r16  seq       the first record's seq
;   r4 - r6   record A: time, state, seq      r7 - r9   record B
;   r19  the ring's offset mask      r20  the record's offset
;   r21  IEP0's count register

	.text
	.global la_sample
la_sample:
	LDI32	r21, 0x0002e010
	LDI32	r19, 0x00007ff0
	; No state yet, so the first pass writes a record, with seq r16.
	LDI32	r5, 0xffffffff
	SUB	r6, r16, 16
la_loop_a:
	AND	r8, r31, r15
	QBEQ	la_loop_a, r8, r5
	LBBO	&r7, r21, 0, 4
	ADD	r9, r6, 16
	AND	r20, r9, r19
	SBBO	&r7, r14, r20, 12
	AND	r5, r31, r15
	QBNE	la_late_a, r5, r8
la_loop_b:
	AND	r5, r31, r15
	QBEQ	la_loop_b, r5, r8
	LBBO	&r4, r21, 0, 4
	ADD	r6, r9, 16
	AND	r20, r6, r19
	SBBO	&r4, r14, r20, 12
	AND	r8, r31, r15
	QBEQ	la_loop_a, r8, r5
	; A change while busy: on into record B, late.
la_late_b:
	LBBO	&r7, r21, 0, 4
	SET	r8, r8, 23
	ADD	r9, r6, 16
	AND	r20, r9, r19
	SBBO	&r7, r14, r20, 12
	CLR	r8, r8, 23
	AND	r5, r31, r15
	QBEQ	la_loop_b, r5, r8
la_late_a:
	LBBO	&r4, r21, 0, 4
	SET	r5, r5, 23
	ADD	r6, r9, 16
	AND	r20, r6, r19
	SBBO	&r4, r14, r20, 12
	CLR	r5, r5, 23
	AND	r8, r31, r15
	QBEQ	la_loop_a, r8, r5
	QBA	la_late_b
