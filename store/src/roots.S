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

/* the index's public key (index_key.pem: the catalogue's CI signs with its
   private half), for mbedtls_pk_parse_public_key: its size counts the 0 */
  .globl mxstore_index_key, mxstore_index_key_size
  .balign 4
mxstore_index_key:
  .incbin "index_key.pem"
  .byte 0
mxstore_index_key_end:
  .balign 4
mxstore_index_key_size:
  .word mxstore_index_key_end - mxstore_index_key
