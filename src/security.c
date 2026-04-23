/*
 * Limceron Compiler — Security Module
 *
 * Implements:
 * 1. Content hashing for .lceron integrity (SHA-256 subset, self-contained)
 * 2. .lceron signature creation and verification
 * 3. Compiler binary self-verification at startup
 *
 * IMPORTANT: No external crypto libraries. Everything is self-contained.
 * This uses a simplified signing scheme for Stage 0. Stage 2 will implement
 * full Ed25519.
 */

#include "lcn.h"

/* ============================================================
 * SHA-256 (self-contained implementation)
 * Based on FIPS 180-4. No external dependencies.
 * ============================================================ */

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buffer[64];
    uint32_t buflen;
} Sha256Ctx;

static uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_init(Sha256Ctx *ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitcount = 0;
    ctx->buflen = 0;
}

static void sha256_transform(Sha256Ctx *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) |
               ((uint32_t)block[i*4+3]);
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_update(Sha256Ctx *ctx, const uint8_t *data, size_t len) {
    size_t i;
    ctx->bitcount += (uint64_t)len * 8;

    for (i = 0; i < len; i++) {
        ctx->buffer[ctx->buflen++] = data[i];
        if (ctx->buflen == 64) {
            sha256_transform(ctx, ctx->buffer);
            ctx->buflen = 0;
        }
    }
}

static void sha256_final(Sha256Ctx *ctx, uint8_t hash[32]) {
    uint32_t i;
    uint8_t pad;

    pad = 0x80;
    sha256_update(ctx, &pad, 1);
    pad = 0x00;
    while (ctx->buflen != 56) {
        sha256_update(ctx, &pad, 1);
    }

    /* Append bit length (big-endian) */
    for (i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(ctx->bitcount >> (56 - i * 8));
        sha256_update(ctx, &b, 1);
    }

    /* Produce hash (big-endian) */
    for (i = 0; i < 8; i++) {
        hash[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        hash[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

void security_hash_buffer(const uint8_t *data, size_t len, uint8_t out[HASH_SIZE]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

bool security_self_verify(const char *exe_path) {
    /*
     * Self-verification: the compiler reads its own binary, hashes everything
     * EXCEPT the embedded hash field, and compares with the stored hash.
     *
     * In Stage 0, we store the expected hash during the build step.
     * The linker patches the hash into a known offset in the binary.
     *
     * For now (bootstrap), we just verify the binary is readable.
     * Full implementation comes in Stage 2 when we control the linker.
     */
    FILE *f = fopen(exe_path, "rb");
    if (!f) {
        fprintf(stderr, "security: cannot read own binary: %s\n", exe_path);
        return false;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return false;
    }

    /* Read entire binary */
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(f);
        return false;
    }

    size_t read = fread(data, 1, (size_t)size, f);
    fclose(f);

    if ((long)read != size) {
        free(data);
        return false;
    }

    /* Hash the binary */
    uint8_t hash[HASH_SIZE];
    security_hash_buffer(data, (size_t)size, hash);
    free(data);

    /*
     * TODO (Stage 2): Compare against embedded hash.
     * For Stage 0, self-verification passes if we can read ourselves.
     * The real implementation will:
     * 1. Read the embedded expected hash from a known offset
     * 2. Zero out that region before hashing
     * 3. Compare computed hash with expected hash
     * 4. Reject execution if mismatch (tampered binary)
     */

    return true;
}

bool security_sign_lceron(LceronObjHeader *header, const uint8_t *data, size_t len,
                        const uint8_t private_key[HASH_SIZE]) {
    /*
     * Sign a .lceron file:
     * 1. Hash the content (code + data + symtab + relocs)
     * 2. Sign the hash with the private key
     * 3. Store hash and signature in the header
     *
     * Stage 0 uses HMAC-SHA256 as a simplified signing scheme.
     * Stage 2 will implement Ed25519 for proper asymmetric signing.
     */

    /* Step 1: Hash the content */
    security_hash_buffer(data, len, header->content_hash);

    /* Step 2: HMAC-SHA256(key, hash) as simplified signature */
    /* HMAC: H((key XOR opad) || H((key XOR ipad) || message)) */
    uint8_t ipad[64], opad[64];
    uint8_t inner_hash[HASH_SIZE];
    Sha256Ctx ctx;
    int i;

    memset(ipad, 0x36, 64);
    memset(opad, 0x5c, 64);
    for (i = 0; i < HASH_SIZE; i++) {
        ipad[i] ^= private_key[i];
        opad[i] ^= private_key[i];
    }

    /* Inner hash */
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, header->content_hash, HASH_SIZE);
    sha256_final(&ctx, inner_hash);

    /* Outer hash */
    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner_hash, HASH_SIZE);
    sha256_final(&ctx, header->signature);

    /* Fill remaining signature bytes with secondary hash for 64-byte sig */
    sha256_init(&ctx);
    sha256_update(&ctx, header->signature, HASH_SIZE);
    sha256_update(&ctx, header->content_hash, HASH_SIZE);
    sha256_final(&ctx, header->signature + HASH_SIZE);

    return true;
}

bool security_verify_lceron(const LceronObjHeader *header, const uint8_t *data, size_t len,
                          const uint8_t public_key[HASH_SIZE]) {
    /*
     * Verify a .lceron file:
     * 1. Check magic number
     * 2. Re-hash the content
     * 3. Verify the hash matches
     * 4. Verify the signature with the public key
     */

    /* Check magic */
    if (header->magic != LCN_MAGIC) {
        fprintf(stderr, "security: invalid .lceron magic number\n");
        return false;
    }

    /* Check version */
    if (header->version != LCERON_OBJ_VERSION) {
        fprintf(stderr, "security: unsupported .lceron version %u\n", header->version);
        return false;
    }

    /* Re-hash content */
    uint8_t computed_hash[HASH_SIZE];
    security_hash_buffer(data, len, computed_hash);

    /* Verify hash matches */
    if (memcmp(computed_hash, header->content_hash, HASH_SIZE) != 0) {
        fprintf(stderr, "security: .lceron content hash mismatch (file corrupted or tampered)\n");
        return false;
    }

    /* Verify signature (recompute HMAC with public key and compare) */
    /* In HMAC-based scheme, signer and verifier share the key */
    /* In Stage 2, this becomes Ed25519 verify with separate pub/priv keys */
    uint8_t ipad[64], opad[64];
    uint8_t inner_hash[HASH_SIZE];
    uint8_t expected_sig[SIGNATURE_SIZE];
    Sha256Ctx ctx;
    int i;

    memset(ipad, 0x36, 64);
    memset(opad, 0x5c, 64);
    for (i = 0; i < HASH_SIZE; i++) {
        ipad[i] ^= public_key[i];
        opad[i] ^= public_key[i];
    }

    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, header->content_hash, HASH_SIZE);
    sha256_final(&ctx, inner_hash);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner_hash, HASH_SIZE);
    sha256_final(&ctx, expected_sig);

    sha256_init(&ctx);
    sha256_update(&ctx, expected_sig, HASH_SIZE);
    sha256_update(&ctx, header->content_hash, HASH_SIZE);
    sha256_final(&ctx, expected_sig + HASH_SIZE);

    if (memcmp(expected_sig, header->signature, SIGNATURE_SIZE) != 0) {
        fprintf(stderr, "security: .lceron signature verification FAILED\n");
        fprintf(stderr, "          this file was not produced by an authorized Limceron compiler\n");
        return false;
    }

    return true;
}
