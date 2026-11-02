; SPDX-License-Identifier: MIT
;
; la_sample(ring, ctl, mask): a PRU's sampling loop, never returns.
;
; Two cycles a pass, 8 ns: read the GPI pins (R31) through the mask, and
; loop while they match the last state. On a change it reads IEP0's count
; and writes the edge record, time then state, into the ring in shared RAM
; (la_map.h), then the ring's write count. That takes about a dozen cycles,
; during which a further change waits for the next pass; a pulse shorter
; than that, right after an edge, is not seen. The first pass always writes
; a record, so a stream starts with every pin's state.
;
;   r14  ring      the ring's local address, LA_SRING_SIZE bytes
;   r15  ctl       the write count's local address
;   r16  mask      the PRU's GPI pins
;   r17  the last state      r18  bytes written
;   r19  the ring's offset mask      r20  the record's offset
;   r21  IEP0's count register
;   r4   time, r5 state: one record, stored together

	.text
	.global la_sample
la_sample:
	LDI32	r21, 0x0002e010
	LDI	r19, 0x3ff8
	LDI	r18, 0
	SBBO	&r18, r15, 0, 4
	LDI32	r17, 0xffffffff
la_loop:
	AND	r5, r31, r16
	QBEQ	la_loop, r5, r17
	LBBO	&r4, r21, 0, 4
	AND	r20, r18, r19
	SBBO	&r4, r14, r20, 8
	ADD	r18, r18, 8
	MOV	r17, r5
	SBBO	&r18, r15, 0, 4
	QBA	la_loop
