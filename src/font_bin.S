# MXSHELL2: the font atlases (font/font_<shape>.z, zlib, tools/mk-font.py) in the ELF;
# font_<shape>_z to font_<shape>_z_end
	.section .rodata
	.globl font_hd_z, font_hd_z_end
font_hd_z:
	.incbin "font_hd.z"
font_hd_z_end:
	.globl font_hd540_z, font_hd540_z_end
font_hd540_z:
	.incbin "font_hd540.z"
font_hd540_z_end:
	.globl font_p720_z, font_p720_z_end
font_p720_z:
	.incbin "font_p720.z"
font_p720_z_end:
	.globl font_p576_z, font_p576_z_end
font_p576_z:
	.incbin "font_p576.z"
font_p576_z_end:
	.globl font_p480_z, font_p480_z_end
font_p480_z:
	.incbin "font_p480.z"
font_p480_z_end:
	.globl font_ntsc_z, font_ntsc_z_end
font_ntsc_z:
	.incbin "font_ntsc.z"
font_ntsc_z_end:
	.globl font_pal_z, font_pal_z_end
font_pal_z:
	.incbin "font_pal.z"
font_pal_z_end:
	.globl font_p576w_z, font_p576w_z_end
font_p576w_z:
	.incbin "font_p576w.z"
font_p576w_z_end:
	.globl font_p480w_z, font_p480w_z_end
font_p480w_z:
	.incbin "font_p480w.z"
font_p480w_z_end:
	.globl font_ntscw_z, font_ntscw_z_end
font_ntscw_z:
	.incbin "font_ntscw.z"
font_ntscw_z_end:
	.globl font_palw_z, font_palw_z_end
font_palw_z:
	.incbin "font_palw.z"
font_palw_z_end:
