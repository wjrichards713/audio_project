/**
 * @file crypto.c
 * @brief AES-256-GCM encryption implementation.
 *
 * Two backends:
 *   - OpenSSL EVP (when HAVE_OPENSSL is defined at compile time)
 *   - XOR-based placeholder (POC fallback)
 *
 * The XOR backend is NOT secure -- it exists only so the system can be
 * tested end-to-end without an OpenSSL dependency.  The code is structured
 * so swapping in real AES-GCM is a single recompile with -DHAVE_OPENSSL.
 */

#include "crypto.h"
#include <stdlib.h>
#include <string.h>

/* ─── Maximum channels we track keys for ──────────────────────────── */
#define MAX_CHANNEL_KEYS 64

typedef struct {
    uint32_t channel_id;
    uint8_t  key[AE_CRYPTO_KEY_SIZE];
    int      active;
} channel_key_t;

struct ae_crypto {
    channel_key_t keys[MAX_CHANNEL_KEYS];
    int           num_keys;
};

/* ─── Helper: find key for channel ────────────────────────────────── */

static const uint8_t *find_key(const ae_crypto_t *ctx, uint32_t channel_id)
{
    for (int i = 0; i < ctx->num_keys; i++) {
        if (ctx->keys[i].active && ctx->keys[i].channel_id == channel_id)
            return ctx->keys[i].key;
    }
    return NULL;
}

/* ─── Helper: build 12-byte nonce ─────────────────────────────────── */

static void build_nonce(uint8_t nonce[AE_CRYPTO_IV_SIZE],
                        uint32_t channel_id,
                        uint64_t client_id,
                        uint32_t sequence)
{
    /* channel_id (4 bytes, big-endian) */
    nonce[0] = (uint8_t)(channel_id >> 24);
    nonce[1] = (uint8_t)(channel_id >> 16);
    nonce[2] = (uint8_t)(channel_id >>  8);
    nonce[3] = (uint8_t)(channel_id);

    /* client_id lower 32 bits (4 bytes, big-endian) */
    uint32_t cid_low = (uint32_t)(client_id & 0xFFFFFFFF);
    nonce[4] = (uint8_t)(cid_low >> 24);
    nonce[5] = (uint8_t)(cid_low >> 16);
    nonce[6] = (uint8_t)(cid_low >>  8);
    nonce[7] = (uint8_t)(cid_low);

    /* sequence (4 bytes, big-endian) */
    nonce[8]  = (uint8_t)(sequence >> 24);
    nonce[9]  = (uint8_t)(sequence >> 16);
    nonce[10] = (uint8_t)(sequence >>  8);
    nonce[11] = (uint8_t)(sequence);
}

/* ─── Simple key derivation for POC ───────────────────────────────── */

static void derive_key_from_channel(uint32_t channel_id, uint8_t key[AE_CRYPTO_KEY_SIZE])
{
    /*
     * POC key derivation: fill 32 bytes from channel_id via simple mixing.
     * NOT cryptographically secure -- placeholder for proper key exchange.
     */
    uint32_t seed = channel_id ^ 0xDEADBEEF;
    for (int i = 0; i < AE_CRYPTO_KEY_SIZE; i++) {
        seed = seed * 1103515245 + 12345;
        key[i] = (uint8_t)((seed >> 16) & 0xFF);
    }
}

/* ═══════════════════════════════════════════════════════════════════ */
#ifdef HAVE_OPENSSL
/* ═══════════════════════════════════════════════════════════════════ */

#include <openssl/evp.h>
#include <openssl/err.h>

ae_crypto_t *ae_crypto_init(void)
{
    ae_crypto_t *ctx = (ae_crypto_t *)calloc(1, sizeof(ae_crypto_t));
    return ctx;
}

void ae_crypto_cleanup(ae_crypto_t *ctx)
{
    if (ctx) {
        memset(ctx->keys, 0, sizeof(ctx->keys)); /* wipe keys */
        free(ctx);
    }
}

int ae_crypto_set_channel_key(ae_crypto_t *ctx, uint32_t channel_id,
                              const uint8_t *key)
{
    if (!ctx) return -1;

    /* Look for existing entry or empty slot */
    int slot = -1;
    for (int i = 0; i < ctx->num_keys; i++) {
        if (ctx->keys[i].active && ctx->keys[i].channel_id == channel_id) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (ctx->num_keys >= MAX_CHANNEL_KEYS) return -1;
        slot = ctx->num_keys++;
    }

    ctx->keys[slot].channel_id = channel_id;
    ctx->keys[slot].active = 1;

    if (key) {
        memcpy(ctx->keys[slot].key, key, AE_CRYPTO_KEY_SIZE);
    } else {
        derive_key_from_channel(channel_id, ctx->keys[slot].key);
    }

    return 0;
}

int ae_crypto_encrypt(ae_crypto_t *ctx,
                      uint32_t channel_id,
                      uint64_t client_id,
                      uint32_t sequence,
                      const uint8_t *aad, int aad_len,
                      const uint8_t *plaintext, int plaintext_len,
                      uint8_t *output, int output_size)
{
    if (!ctx || !plaintext || !output)
        return -1;

    int needed = plaintext_len + AE_CRYPTO_OVERHEAD;
    if (output_size < needed)
        return -1;

    const uint8_t *key = find_key(ctx, channel_id);
    if (!key)
        return -1;

    /* Build nonce */
    uint8_t nonce[AE_CRYPTO_IV_SIZE];
    build_nonce(nonce, channel_id, client_id, sequence);

    /* Write IV to output first */
    memcpy(output, nonce, AE_CRYPTO_IV_SIZE);

    /* Encrypt */
    EVP_CIPHER_CTX *evp = EVP_CIPHER_CTX_new();
    if (!evp)
        return -1;

    int ok = 1;
    int out_len = 0, final_len = 0;

    ok = ok && EVP_EncryptInit_ex(evp, EVP_aes_256_gcm(), NULL, NULL, NULL);
    ok = ok && EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_SET_IVLEN,
                                   AE_CRYPTO_IV_SIZE, NULL);
    ok = ok && EVP_EncryptInit_ex(evp, NULL, NULL, key, nonce);

    /* AAD */
    if (ok && aad && aad_len > 0) {
        ok = ok && EVP_EncryptUpdate(evp, NULL, &out_len, aad, aad_len);
    }

    /* Plaintext → ciphertext (written after IV) */
    ok = ok && EVP_EncryptUpdate(evp, output + AE_CRYPTO_IV_SIZE, &out_len,
                                 plaintext, plaintext_len);
    ok = ok && EVP_EncryptFinal_ex(evp, output + AE_CRYPTO_IV_SIZE + out_len,
                                   &final_len);

    /* Get tag and append after ciphertext */
    ok = ok && EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_GET_TAG,
                                   AE_CRYPTO_TAG_SIZE,
                                   output + AE_CRYPTO_IV_SIZE + out_len + final_len);

    EVP_CIPHER_CTX_free(evp);

    if (!ok)
        return -1;

    return AE_CRYPTO_IV_SIZE + out_len + final_len + AE_CRYPTO_TAG_SIZE;
}

int ae_crypto_decrypt(ae_crypto_t *ctx,
                      uint32_t channel_id,
                      const uint8_t *aad, int aad_len,
                      const uint8_t *input, int input_len,
                      uint8_t *output, int output_size)
{
    if (!ctx || !input || !output)
        return -1;

    if (input_len < AE_CRYPTO_OVERHEAD)
        return -1;

    int ciphertext_len = input_len - AE_CRYPTO_OVERHEAD;
    if (output_size < ciphertext_len)
        return -1;

    const uint8_t *key = find_key(ctx, channel_id);
    if (!key)
        return -1;

    /* Extract IV from input */
    const uint8_t *iv         = input;
    const uint8_t *ciphertext = input + AE_CRYPTO_IV_SIZE;
    const uint8_t *tag        = input + AE_CRYPTO_IV_SIZE + ciphertext_len;

    EVP_CIPHER_CTX *evp = EVP_CIPHER_CTX_new();
    if (!evp)
        return -1;

    int ok = 1;
    int out_len = 0, final_len = 0;

    ok = ok && EVP_DecryptInit_ex(evp, EVP_aes_256_gcm(), NULL, NULL, NULL);
    ok = ok && EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_SET_IVLEN,
                                   AE_CRYPTO_IV_SIZE, NULL);
    ok = ok && EVP_DecryptInit_ex(evp, NULL, NULL, key, iv);

    /* AAD */
    if (ok && aad && aad_len > 0) {
        ok = ok && EVP_DecryptUpdate(evp, NULL, &out_len, aad, aad_len);
    }

    /* Ciphertext → plaintext */
    ok = ok && EVP_DecryptUpdate(evp, output, &out_len,
                                 ciphertext, ciphertext_len);

    /* Set expected tag before finalize */
    ok = ok && EVP_CIPHER_CTX_ctrl(evp, EVP_CTRL_GCM_SET_TAG,
                                   AE_CRYPTO_TAG_SIZE, (void *)tag);

    /* Finalize — this verifies the tag */
    int verify_ok = EVP_DecryptFinal_ex(evp, output + out_len, &final_len);

    EVP_CIPHER_CTX_free(evp);

    if (!ok || !verify_ok)
        return -1;

    return out_len + final_len;
}

/* ═══════════════════════════════════════════════════════════════════ */
#else  /* !HAVE_OPENSSL — XOR placeholder */
/* ═══════════════════════════════════════════════════════════════════ */

/*
 * POC XOR "cipher".  NOT SECURE — for development/testing only.
 *
 * Format is the same as AES-GCM:  IV(12) || "ciphertext" || tag(16)
 * The "encryption" is XOR with a key-derived stream.
 * The "tag" is a simple checksum of plaintext + AAD.
 */

ae_crypto_t *ae_crypto_init(void)
{
    ae_crypto_t *ctx = (ae_crypto_t *)calloc(1, sizeof(ae_crypto_t));
    return ctx;
}

void ae_crypto_cleanup(ae_crypto_t *ctx)
{
    if (ctx) {
        memset(ctx->keys, 0, sizeof(ctx->keys));
        free(ctx);
    }
}

int ae_crypto_set_channel_key(ae_crypto_t *ctx, uint32_t channel_id,
                              const uint8_t *key)
{
    if (!ctx) return -1;

    int slot = -1;
    for (int i = 0; i < ctx->num_keys; i++) {
        if (ctx->keys[i].active && ctx->keys[i].channel_id == channel_id) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (ctx->num_keys >= MAX_CHANNEL_KEYS) return -1;
        slot = ctx->num_keys++;
    }

    ctx->keys[slot].channel_id = channel_id;
    ctx->keys[slot].active = 1;

    if (key) {
        memcpy(ctx->keys[slot].key, key, AE_CRYPTO_KEY_SIZE);
    } else {
        derive_key_from_channel(channel_id, ctx->keys[slot].key);
    }

    return 0;
}

/* Simple XOR stream: key XOR nonce, repeated */
static void xor_stream(const uint8_t *key, const uint8_t *nonce,
                        const uint8_t *in, uint8_t *out, int len)
{
    uint8_t stream_key[AE_CRYPTO_KEY_SIZE];
    for (int i = 0; i < AE_CRYPTO_KEY_SIZE; i++)
        stream_key[i] = key[i] ^ nonce[i % AE_CRYPTO_IV_SIZE];

    for (int i = 0; i < len; i++)
        out[i] = in[i] ^ stream_key[i % AE_CRYPTO_KEY_SIZE];
}

/* Simple checksum "tag" for POC integrity check */
static void compute_tag(const uint8_t *key, const uint8_t *data, int data_len,
                        const uint8_t *aad, int aad_len,
                        uint8_t tag[AE_CRYPTO_TAG_SIZE])
{
    memset(tag, 0, AE_CRYPTO_TAG_SIZE);

    /* Mix key into tag */
    for (int i = 0; i < AE_CRYPTO_KEY_SIZE; i++)
        tag[i % AE_CRYPTO_TAG_SIZE] ^= key[i];

    /* Mix data */
    for (int i = 0; i < data_len; i++)
        tag[i % AE_CRYPTO_TAG_SIZE] ^= data[i];

    /* Mix AAD */
    if (aad) {
        for (int i = 0; i < aad_len; i++)
            tag[i % AE_CRYPTO_TAG_SIZE] ^= aad[i];
    }

    /* Final mixing pass */
    for (int i = AE_CRYPTO_TAG_SIZE - 1; i > 0; i--)
        tag[i - 1] ^= tag[i];
}

int ae_crypto_encrypt(ae_crypto_t *ctx,
                      uint32_t channel_id,
                      uint64_t client_id,
                      uint32_t sequence,
                      const uint8_t *aad, int aad_len,
                      const uint8_t *plaintext, int plaintext_len,
                      uint8_t *output, int output_size)
{
    if (!ctx || !plaintext || !output)
        return -1;

    int needed = plaintext_len + AE_CRYPTO_OVERHEAD;
    if (output_size < needed)
        return -1;

    const uint8_t *key = find_key(ctx, channel_id);
    if (!key)
        return -1;

    /* Build nonce */
    uint8_t nonce[AE_CRYPTO_IV_SIZE];
    build_nonce(nonce, channel_id, client_id, sequence);

    /* Write IV */
    memcpy(output, nonce, AE_CRYPTO_IV_SIZE);

    /* "Encrypt" with XOR */
    xor_stream(key, nonce, plaintext,
               output + AE_CRYPTO_IV_SIZE, plaintext_len);

    /* Compute and append tag (over plaintext + aad for integrity) */
    compute_tag(key, plaintext, plaintext_len, aad, aad_len,
                output + AE_CRYPTO_IV_SIZE + plaintext_len);

    return needed;
}

int ae_crypto_decrypt(ae_crypto_t *ctx,
                      uint32_t channel_id,
                      const uint8_t *aad, int aad_len,
                      const uint8_t *input, int input_len,
                      uint8_t *output, int output_size)
{
    if (!ctx || !input || !output)
        return -1;

    if (input_len < AE_CRYPTO_OVERHEAD)
        return -1;

    int ciphertext_len = input_len - AE_CRYPTO_OVERHEAD;
    if (output_size < ciphertext_len)
        return -1;

    const uint8_t *key = find_key(ctx, channel_id);
    if (!key)
        return -1;

    const uint8_t *iv         = input;
    const uint8_t *ciphertext = input + AE_CRYPTO_IV_SIZE;
    const uint8_t *tag        = input + AE_CRYPTO_IV_SIZE + ciphertext_len;

    /* "Decrypt" with XOR */
    xor_stream(key, iv, ciphertext, output, ciphertext_len);

    /* Verify tag */
    uint8_t expected_tag[AE_CRYPTO_TAG_SIZE];
    compute_tag(key, output, ciphertext_len, aad, aad_len, expected_tag);

    /* Constant-time compare (as much as XOR POC warrants) */
    int diff = 0;
    for (int i = 0; i < AE_CRYPTO_TAG_SIZE; i++)
        diff |= tag[i] ^ expected_tag[i];

    if (diff != 0) {
        memset(output, 0, (size_t)ciphertext_len);
        return -1;
    }

    return ciphertext_len;
}

#endif /* HAVE_OPENSSL */
