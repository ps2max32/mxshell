	/* sent by DMA to the GS: core (the shell's VM build, mx OS plan 3d) */
	.section .rodata.mx.core, "a"
	.balign 64
	.globl font_bin
font_bin:
	.incbin "shell/font.bin"
