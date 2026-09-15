// mbedTLS 3.6 configuration for the kiosk's HTTPS client (KIOSK_TLS=1 only; the file is picked up
// through the SDK's pico_mbedtls_config.h shim, which #includes "mbedtls_config.h" from our
// include path).
//
// Trimmed from pico-examples/pico_w/wifi/mbedtls_config_examples_common.h to a TLS 1.2 client
// that can reach a Let's Encrypt / DigiCert / Google-issued server and nothing else. Every
// feature left out is RAM or flash: the server-side code, the extra curves, SHA-1/MD5 and the
// RSA key exchange in the example add ~30 KB of flash and make the per-connection SSL context
// bigger. The record buffers dominate RAM: MBEDTLS_SSL_IN_CONTENT_LEN (16 KB, the protocol
// maximum: the server may send full records and we must be able to hold one) plus
// MBEDTLS_SSL_OUT_CONTENT_LEN (2 KB: we only ever send a < 1 KB GET).
#ifndef KIOSK_MBEDTLS_CONFIG_H
#define KIOSK_MBEDTLS_CONFIG_H

/* Workaround for some mbedtls source files using INT_MAX without including limits.h */
#include <limits.h>

// ---- platform ----------------------------------------------------------------------------
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY        // no /dev/urandom; entropy comes from...
#define MBEDTLS_ENTROPY_HARDWARE_ALT       // ...mbedtls_hardware_poll (pico_mbedtls.c, get_rand_64 → ROSC + timer)
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_ALLOW_PRIVATE_ACCESS       // altcp_tls_mbedtls.c reads ssl_context.out_left etc.
// No RTC. MBEDTLS_HAVE_TIME is required by the SDK's altcp_tls_mbedtls.c (session start
// stamps) and is served by mbedtls_ms_time() from http_client.c (milliseconds since boot).
// MBEDTLS_HAVE_TIME_DATE stays undefined, so x509 skips the notBefore/notAfter check and
// certificate validity dates are
// not enforced on the device (chain of trust, hostname and signatures still are). See
// docs/NETWORK.md.

// ---- TLS: 1.2 client only ----------------------------------------------------------------
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION // SNI: Traefik/Cloudflare front many hosts per IP
#define MBEDTLS_SSL_IN_CONTENT_LEN     16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN    2048
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH    // ask the server for smaller records where it honours the extension (Let's Encrypt-fronted Traefik does not, Cloud Run does)
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED   // Let's Encrypt ECDSA chains, Google
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED     // RSA leaf certificates (Traefik default)
#define MBEDTLS_ERROR_C                    // mbedtls_strerror for the console log; ~6 KB flash, worth it while bringing up

// ---- public key --------------------------------------------------------------------------
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15                  // certificate signatures on the roots above are PKCS#1 v1.5
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED   // P-256: every leaf and intermediate we meet
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED   // P-384: ISRG Root X2, GTS Root R4 (the roots' own keys)
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED  // x25519 ECDHE: cheapest handshake for the M0+ when the server offers it (Cloudflare, Caddy, Traefik do); costs ~2 KB flash
#define MBEDTLS_ECP_NIST_OPTIM             // NIST reduction: 3–4× faster P-256 on a core without a multiplier pipeline
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C               // ECDSA signature encoding in the ECDHE verify step
#define MBEDTLS_OID_C

// ---- symmetric / hashes ------------------------------------------------------------------
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES           // −6 KB of flash tables for a few % throughput on a link that is idle 99% of the time
#define MBEDTLS_GCM_C                      // AES-GCM: the only AEAD every modern server offers for TLS 1.2
#define MBEDTLS_CIPHER_MODE_CBC            // AES-CBC with HMAC: kept for servers restricted to legacy TLS 1.2 suites (some corporate proxies); ~1 KB flash
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C                   // 3.6 wants it alongside SHA256 when SHA256_SMALLER is on
#define MBEDTLS_SHA256_SMALLER             // −2 KB flash, SHA-256 ~30% slower: hashing is not the bottleneck
#define MBEDTLS_SHA384_C                   // SHA-384 signatures on the P-384 roots' chains
#define MBEDTLS_SHA512_C

// ---- X.509 -------------------------------------------------------------------------------
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PEM_PARSE_C                // the CA bundle is PEM (ca_certs.h)
#define MBEDTLS_BASE64_C

// ---- RNG ---------------------------------------------------------------------------------
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

#endif // KIOSK_MBEDTLS_CONFIG_H
