/**
 * @file crypto.h
 * @brief AES-256-GCM encryption for audio packet payloads.
 *
 * Packet format:  IV(12) || ciphertext || tag(16)
 *
 * Nonce construction:  channel_id(4) + client_id(4) + sequence(4) = 12 bytes
 * AAD: routing header (20 bytes) -- authenticated but not encrypted.
 *
 * This module has two backends:
 *   1. OpenSSL EVP (production, when HAVE_OPENSSL is defined)
 *   2. XOR cipher (POC placeholder, used when OpenSSL is unavailable)
 *
 * The API is identical regardless of backend.
 */

#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Crypto constants */
#define AE_CRYPTO_KEY_SIZE    32   /* AES-256 key */
#define AE_CRYPTO_IV_SIZE     12   /* GCM nonce */
#define AE_CRYPTO_TAG_SIZE    16   /* GCM authentication tag */
#define AE_CRYPTO_AAD_SIZE    20   /* routing header size */

/* Overhead added to plaintext: IV + tag */
#define AE_CRYPTO_OVERHEAD    (AE_CRYPTO_IV_SIZE + AE_CRYPTO_TAG_SIZE)

typedef struct ae_crypto ae_crypto_t;

/**
 * Initialize the crypto subsystem.
 * @return Crypto context, or NULL on failure.
 */
ae_crypto_t *ae_crypto_init(void);

/** Clean up and free the crypto context. */
void ae_crypto_cleanup(ae_crypto_t *ctx);

/**
 * Set the encryption key for a channel.
 * For POC, this derives a key from the channel_id using a simple HMAC-like hash.
 * In production, this would come from a key exchange protocol.
 *
 * @param ctx         Crypto context.
 * @param channel_id  Channel identifier.
 * @param key         32-byte AES-256 key (if NULL, derive from channel_id).
 * @return 0 on success, -1 on failure.
 */
int ae_crypto_set_channel_key(ae_crypto_t *ctx, uint32_t channel_id,
                              const uint8_t *key);

/**
 * Encrypt an Opus payload.
 *
 * Output format: IV(12) || ciphertext(plaintext_len) || tag(16)
 * Total output size = plaintext_len + AE_CRYPTO_OVERHEAD
 *
 * @param ctx            Crypto context.
 * @param channel_id     Channel ID (used in nonce).
 * @param client_id      Client ID (lower 32 bits used in nonce).
 * @param sequence       Packet sequence number (used in nonce).
 * @param aad            Routing header (20 bytes) for authentication.
 * @param aad_len        Length of AAD (must be AE_CRYPTO_AAD_SIZE).
 * @param plaintext      Opus payload to encrypt.
 * @param plaintext_len  Length of plaintext.
 * @param output         Output buffer (must be >= plaintext_len + AE_CRYPTO_OVERHEAD).
 * @param output_size    Size of output buffer.
 * @return Total bytes written to output, or -1 on error.
 */
int ae_crypto_encrypt(ae_crypto_t *ctx,
                      uint32_t channel_id,
                      uint64_t client_id,
                      uint32_t sequence,
                      const uint8_t *aad, int aad_len,
                      const uint8_t *plaintext, int plaintext_len,
                      uint8_t *output, int output_size);

/**
 * Decrypt and verify an encrypted payload.
 *
 * Input format: IV(12) || ciphertext || tag(16)
 *
 * @param ctx           Crypto context.
 * @param channel_id    Channel ID (for key lookup).
 * @param aad           Routing header (20 bytes) for verification.
 * @param aad_len       Length of AAD.
 * @param input         Encrypted data: IV || ciphertext || tag.
 * @param input_len     Length of encrypted data.
 * @param output        Output buffer for decrypted plaintext.
 * @param output_size   Size of output buffer.
 * @return Bytes of plaintext written, or -1 on error/authentication failure.
 */
int ae_crypto_decrypt(ae_crypto_t *ctx,
                      uint32_t channel_id,
                      const uint8_t *aad, int aad_len,
                      const uint8_t *input, int input_len,
                      uint8_t *output, int output_size);

#ifdef __cplusplus
}
#endif

#endif /* CRYPTO_H */
