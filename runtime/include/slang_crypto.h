/* slang_crypto.h - C declarations of the std.crypto.{aes, chacha, x25519,
 * ecdh, ecdsa, ed25519, rsa, hkdf} externs, the std.crypto.hash/hmac
 * additions and the internal rt* helpers of std.crypto.x509 and
 * std.net.tls. Calling convention: runtime/ABI.md 14.1; semantics: ABI.md
 * section 19. Implemented in src/crypto/api.c. */
#ifndef SLANG_CRYPTO_API_H
#define SLANG_CRYPTO_API_H

#include "slang_rt.h"

void slang_crypto_hash_sha384(const slang_string *data, slang_slice *out);
slang_string *slang_crypto_hash_sha384_hex(const slang_string *data);
void slang_crypto_hash_sum(const slang_string *alg, const slang_slice *data, slang_slice *out, slang_error **err);
void slang_crypto_hash_rt_new(const slang_string *alg, slang_slice *out, slang_error **err);
void slang_crypto_hash_rt_write(const slang_slice *st, const slang_slice *data);
void slang_crypto_hash_rt_sum(const slang_slice *st, slang_slice *out);
void slang_crypto_hmac_sum(const slang_string *alg, const slang_slice *key, const slang_slice *data, slang_slice *out, slang_error **err);
void slang_crypto_hkdf_extract(const slang_string *alg, const slang_slice *salt, const slang_slice *ikm, slang_slice *out, slang_error **err);
void slang_crypto_hkdf_expand(const slang_string *alg, const slang_slice *prk, const slang_slice *info, int64_t n, slang_slice *out, slang_error **err);
void slang_crypto_aes_gcm_seal(const slang_slice *key, const slang_slice *nonce, const slang_slice *pt, const slang_slice *aad, slang_slice *out, slang_error **err);
void slang_crypto_aes_gcm_open(const slang_slice *key, const slang_slice *nonce, const slang_slice *ct, const slang_slice *aad, slang_slice *out, slang_error **err);
void slang_crypto_aes_encrypt_block(const slang_slice *key, const slang_slice *block, slang_slice *out, slang_error **err);
bool slang_crypto_aes_has_hardware(void);
void slang_crypto_chacha_seal(const slang_slice *key, const slang_slice *nonce, const slang_slice *pt, const slang_slice *aad, slang_slice *out, slang_error **err);
void slang_crypto_chacha_open(const slang_slice *key, const slang_slice *nonce, const slang_slice *ct, const slang_slice *aad, slang_slice *out, slang_error **err);
void slang_crypto_x25519_generate_key(slang_slice *out, slang_error **err);
void slang_crypto_x25519_public_key(const slang_slice *priv, slang_slice *out, slang_error **err);
void slang_crypto_x25519_shared_secret(const slang_slice *priv, const slang_slice *peer, slang_slice *out, slang_error **err);
void slang_crypto_ecdh_generate_key(slang_slice *out, slang_error **err);
void slang_crypto_ecdh_public_key(const slang_slice *priv, slang_slice *out, slang_error **err);
void slang_crypto_ecdh_shared_secret(const slang_slice *priv, const slang_slice *peer, slang_slice *out, slang_error **err);
void slang_crypto_ecdsa_generate_key(slang_slice *out, slang_error **err);
void slang_crypto_ecdsa_public_key(const slang_slice *priv, slang_slice *out, slang_error **err);
void slang_crypto_ecdsa_sign(const slang_slice *priv, const slang_slice *digest, slang_slice *out, slang_error **err);
bool slang_crypto_ecdsa_verify(const slang_slice *pub, const slang_slice *digest, const slang_slice *sig);
void slang_crypto_ed25519_generate_key(slang_slice *out, slang_error **err);
void slang_crypto_ed25519_public_key(const slang_slice *seed, slang_slice *out, slang_error **err);
void slang_crypto_ed25519_sign(const slang_slice *seed, const slang_slice *msg, slang_slice *out, slang_error **err);
bool slang_crypto_ed25519_verify(const slang_slice *pub, const slang_slice *msg, const slang_slice *sig);
bool slang_crypto_rsa_verify_pkcs1v15(const slang_slice *n, const slang_slice *e, const slang_string *hash, const slang_slice *digest, const slang_slice *sig);
bool slang_crypto_rsa_verify_pss(const slang_slice *n, const slang_slice *e, const slang_string *hash, const slang_slice *digest, const slang_slice *sig);
void slang_crypto_rsa_sign_pkcs1v15(const slang_slice *key, const slang_string *hash, const slang_slice *digest, slang_slice *out, slang_error **err);
void slang_crypto_rsa_sign_pss(const slang_slice *key, const slang_string *hash, const slang_slice *digest, slang_slice *out, slang_error **err);
slang_string *slang_crypto_x509_rt_system_roots_path(void);
void slang_net_tls_rt_concat(const slang_slice *a, const slang_slice *b, slang_slice *out);
void slang_net_tls_rt_zeros(int64_t n, slang_slice *out);
void slang_net_tls_rt_copy(const slang_slice *dst, int64_t off, const slang_slice *src);
void slang_net_tls_rt_aead_new(int64_t kind, const slang_slice *key, slang_slice *out, slang_error **err);
void slang_net_tls_rt_aead_seal(const slang_slice *ctx, const slang_slice *nonce, const slang_slice *aad, const slang_slice *pt, slang_slice *out);
void slang_net_tls_rt_aead_open(const slang_slice *ctx, const slang_slice *nonce, const slang_slice *aad, const slang_slice *ct, slang_slice *out, slang_error **err);

#endif
