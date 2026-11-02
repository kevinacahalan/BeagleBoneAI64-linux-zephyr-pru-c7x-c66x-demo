/* SPDX-License-Identifier: MIT */
/* web/index.html, the page ai64 web serves (web.c), built in. */
	.section .rodata
	.global ai64_web_page
	.global ai64_web_page_end
ai64_web_page:
	.incbin "web/index.html"
ai64_web_page_end:
	.byte 0

/* web/la.html, the logic analyzer's viewer, at /la. */
	.global ai64_la_page
	.global ai64_la_page_end
ai64_la_page:
	.incbin "web/la.html"
ai64_la_page_end:
	.byte 0

	.section .note.GNU-stack,"",%progbits
