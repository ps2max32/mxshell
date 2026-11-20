/* The root certificates (certs/, one PEM text), for mbedtls_x509_crt_parse:
   its size counts the closing 0. */
  .section .rodata
  .globl mxstore_roots, mxstore_roots_size
  .balign 4
mxstore_roots:
  .incbin "roots.pem"
  .byte 0
mxstore_roots_end:
  .balign 4
mxstore_roots_size:
  .word mxstore_roots_end - mxstore_roots
