/* zell_crypto.h - C declarations of the std.crypto.{aes, chacha, x25519,
 * ecdh, ecdsa, ed25519, rsa, hkdf} externs, the std.crypto.hash/hmac
 * additions and the internal rt* helpers of std.crypto.x509 and
 * std.net.tls. Calling convention: runtime/ABI.md 14.1; semantics: ABI.md
 * section 19. Implemented in src/crypto/api.c. */
#ifndef ZELL_CRYPTO_API_H
#define ZELL_CRYPTO_API_H

#include "zell_rt.h"

void zell_crypto_hash_sha384(const zell_string *data, zell_slice *out);
zell_string *zell_crypto_hash_sha384_hex(const zell_string *data);
void zell_crypto_hash_sum(const zell_string *alg, const zell_slice *data, zell_slice *out, zell_error **err);
void zell_crypto_hash_rt_new(const zell_string *alg, zell_slice *out, zell_error **err);
void zell_crypto_hash_rt_write(const zell_slice *st, const zell_slice *data);
void zell_crypto_hash_rt_sum(const zell_slice *st, zell_slice *out);
void zell_crypto_hmac_sum(const zell_string *alg, const zell_slice *key, const zell_slice *data, zell_slice *out, zell_error **err);
void zell_crypto_hkdf_extract(const zell_string *alg, const zell_slice *salt, const zell_slice *ikm, zell_slice *out, zell_error **err);
void zell_crypto_hkdf_expand(const zell_string *alg, const zell_slice *prk, const zell_slice *info, int64_t n, zell_slice *out, zell_error **err);
void zell_crypto_aes_gcm_seal(const zell_slice *key, const zell_slice *nonce, const zell_slice *pt, const zell_slice *aad, zell_slice *out, zell_error **err);
void zell_crypto_aes_gcm_open(const zell_slice *key, const zell_slice *nonce, const zell_slice *ct, const zell_slice *aad, zell_slice *out, zell_error **err);
void zell_crypto_aes_encrypt_block(const zell_slice *key, const zell_slice *block, zell_slice *out, zell_error **err);
bool zell_crypto_aes_has_hardware(void);
void zell_crypto_chacha_seal(const zell_slice *key, const zell_slice *nonce, const zell_slice *pt, const zell_slice *aad, zell_slice *out, zell_error **err);
void zell_crypto_chacha_open(const zell_slice *key, const zell_slice *nonce, const zell_slice *ct, const zell_slice *aad, zell_slice *out, zell_error **err);
void zell_crypto_x25519_generate_key(zell_slice *out, zell_error **err);
void zell_crypto_x25519_public_key(const zell_slice *priv, zell_slice *out, zell_error **err);
void zell_crypto_x25519_shared_secret(const zell_slice *priv, const zell_slice *peer, zell_slice *out, zell_error **err);
void zell_crypto_ecdh_generate_key(zell_slice *out, zell_error **err);
void zell_crypto_ecdh_public_key(const zell_slice *priv, zell_slice *out, zell_error **err);
void zell_crypto_ecdh_shared_secret(const zell_slice *priv, const zell_slice *peer, zell_slice *out, zell_error **err);
void zell_crypto_ecdsa_generate_key(zell_slice *out, zell_error **err);
void zell_crypto_ecdsa_public_key(const zell_slice *priv, zell_slice *out, zell_error **err);
void zell_crypto_ecdsa_sign(const zell_slice *priv, const zell_slice *digest, zell_slice *out, zell_error **err);
bool zell_crypto_ecdsa_verify(const zell_slice *pub, const zell_slice *digest, const zell_slice *sig);
void zell_crypto_ed25519_generate_key(zell_slice *out, zell_error **err);
void zell_crypto_ed25519_public_key(const zell_slice *seed, zell_slice *out, zell_error **err);
void zell_crypto_ed25519_sign(const zell_slice *seed, const zell_slice *msg, zell_slice *out, zell_error **err);
bool zell_crypto_ed25519_verify(const zell_slice *pub, const zell_slice *msg, const zell_slice *sig);
bool zell_crypto_rsa_verify_pkcs1v15(const zell_slice *n, const zell_slice *e, const zell_string *hash, const zell_slice *digest, const zell_slice *sig);
bool zell_crypto_rsa_verify_pss(const zell_slice *n, const zell_slice *e, const zell_string *hash, const zell_slice *digest, const zell_slice *sig);
void zell_crypto_rsa_sign_pkcs1v15(const zell_slice *key, const zell_string *hash, const zell_slice *digest, zell_slice *out, zell_error **err);
void zell_crypto_rsa_sign_pss(const zell_slice *key, const zell_string *hash, const zell_slice *digest, zell_slice *out, zell_error **err);
zell_string *zell_crypto_x509_rt_system_roots_path(void);
void zell_net_tls_rt_concat(const zell_slice *a, const zell_slice *b, zell_slice *out);
void zell_net_tls_rt_zeros(int64_t n, zell_slice *out);
void zell_net_tls_rt_copy(const zell_slice *dst, int64_t off, const zell_slice *src);
void zell_net_tls_rt_aead_new(int64_t kind, const zell_slice *key, zell_slice *out, zell_error **err);
void zell_net_tls_rt_aead_seal(const zell_slice *ctx, const zell_slice *nonce, const zell_slice *aad, const zell_slice *pt, zell_slice *out);
void zell_net_tls_rt_aead_open(const zell_slice *ctx, const zell_slice *nonce, const zell_slice *aad, const zell_slice *ct, zell_slice *out, zell_error **err);

#endif
