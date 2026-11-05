crypto_ss.add(genh)
crypto_ss.add(files(
  'afsplit.c',
  'akcipher.c',
  'block-luks.c',
  'block-qcow.c',
  'block.c',
  'cipher.c',
  'der.c',
  'hash.c',
  'hmac.c',
  'ivgen-essiv.c',
  'ivgen-plain.c',
  'ivgen-plain64.c',
  'ivgen.c',
  'pbkdf.c',
  'secret_common.c',
  'secret.c',
  'tlscreds.c',
  'tlscredsanon.c',
  'tlscredspsk.c',
  'tlscredsx509.c',
  'tlssession.c',
  'rsakey.c',
  'x509-utils.c',
))

if gnutls.found()
endif

if nettle.found()
  crypto_ss.add(nettle, files(  ))
  if hogweed.found()
    crypto_ss.add(gmp, hogweed)
  endif
elif gcrypt.found()
  crypto_ss.add(gcrypt, files(  ))
elif gnutls_crypto.found()
  crypto_ss.add(gnutls, files(  ))
else
  crypto_ss.add(files('hash-glib.c', 'hmac-glib.c', 'pbkdf-stub.c'))
endif

if have_keyring
endif
if have_afalg
endif


util_ss.add(files(
  'aes.c',
  'aes-helpers.c',
  'clmul.c',
  'init.c',
  'sm4.c',
))
if gnutls.found()
  util_ss.add(gnutls)
endif

if gcrypt.found()
  util_ss.add(gcrypt, files())
elif gnutls.found()
  util_ss.add(gnutls, files())
elif get_option('rng_none')
else
  util_ss.add(files('random-platform.c'))
endif

