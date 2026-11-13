# MXSHELL2: the font atlases (font/font_<shape>.bin, tools/mk-font.py) in the ELF
	.section .rodata
	.balign 64
	.globl font_hd_bin
font_hd_bin:
	.incbin "font_hd.bin"
	.balign 64
	.globl font_hd540_bin
font_hd540_bin:
	.incbin "font_hd540.bin"
	.balign 64
	.globl font_p720_bin
font_p720_bin:
	.incbin "font_p720.bin"
	.balign 64
	.globl font_p576_bin
font_p576_bin:
	.incbin "font_p576.bin"
	.balign 64
	.globl font_p480_bin
font_p480_bin:
	.incbin "font_p480.bin"
	.balign 64
	.globl font_ntsc_bin
font_ntsc_bin:
	.incbin "font_ntsc.bin"
	.balign 64
	.globl font_pal_bin
font_pal_bin:
	.incbin "font_pal.bin"
	.balign 64
	.globl font_p576w_bin
font_p576w_bin:
	.incbin "font_p576w.bin"
	.balign 64
	.globl font_p480w_bin
font_p480w_bin:
	.incbin "font_p480w.bin"
	.balign 64
	.globl font_ntscw_bin
font_ntscw_bin:
	.incbin "font_ntscw.bin"
	.balign 64
	.globl font_palw_bin
font_palw_bin:
	.incbin "font_palw.bin"
