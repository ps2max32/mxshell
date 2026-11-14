/* the layout of room.vsm, by hand (the macros of a compiled program's
   header, libgl's mk/vu1bin.awk reads them) */
#ifndef ROOM_H
#define ROOM_H
/* uniforms, in quadwords: libgl's four (it sets them by name), then the room's */
#define VU_ROOM_MVP 0
#define VU_ROOM_CSCALE 4
#define VU_ROOM_TMS 5
#define VU_ROOM_TMT 6
#define VU_ROOM_LAMP 7     /* the lights' colour 0..255, w: the dither bias */
#define VU_ROOM_LIGHTS 8   /* 4 lights, a lane each: cx, cy, 1/rx, 1/ry, strength */
#define VU_ROOM_SHADOWS 13 /* 3 x 4 shadows, the same five quadwords a group */
#define VU_ROOM_ENTRY 0    /* MSCAL (ITOP = the context) for a draw's first batch, MSCNT for the others */
#define VU_ROOM_BUF_QW 483 /* double buffer size */
#define VU_ROOM_UNI_QW 28  /* uniforms, after the context's GIF tag */
#define VU_ROOM_CTX_AT 966 /* the contexts: GIF tag (NLOOP 0), uniforms */
#define VU_ROOM_CTX_QW 29  /* per context */
#define VU_ROOM_NCTX 2
#define VU_ROOM_ITEM_QW 3  /* quadwords per vertex in */
#define VU_ROOM_OUT_QW 3   /* GIF registers per vertex */
#define VU_ROOM_MAX_N 77   /* vertices per batch */
#define VU_ROOM_ALIGN 4
#define VU_ROOM_COUNTS 0   /* x vertices */
#define VU_ROOM_IN_AT 1
#define VU_ROOM_OUT_AT 241 /* the output GIF tag; vertex k's at 242 + 3 k */
#define VU_ROOM_NREG 3
#define VU_ROOM_REGS 0x512ull /* ST, RGBAQ, XYZ2 */
#define VU_ROOM_NIN 3
#define VU_ROOM_IN_POSITION 0 /* v4_32, 16 bytes */
#define VU_ROOM_IN_TEXCOORD 1 /* v2_32, 8 bytes */
#define VU_ROOM_IN_COLOR 2    /* v4_8u, 4 bytes */
#define VU_ROOM_INPUTS {{0xc, 0, 16, 0x0}, {0x4, 0, 8, 0x0}, {0xe, 1, 4, 0x0}}
#define VU_ROOM_ROW {0, 0, 0, 0}
#endif
