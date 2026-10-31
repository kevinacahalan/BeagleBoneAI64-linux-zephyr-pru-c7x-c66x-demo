/* SPDX-License-Identifier: MIT */
/* web/index.html, the page ai64 web serves (web.c), built in. */
	.section .rodata
	.global ai64_web_page
	.global ai64_web_page_end
ai64_web_page:
	.incbin "web/index.html"
ai64_web_page_end:
	.byte 0

	.section .note.GNU-stack,"",%progbits
