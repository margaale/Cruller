// mbedTLS (the Pico SDK's 3.6) for tls.c: a TLS 1.2 client for Cruller's firmware downloads from
// GitHub, and nothing else. github.com has an ECDSA P-256 certificate (Sectigo, a P-384 chain); its
// asset host, release-assets.githubusercontent.com, an RSA one (Let's Encrypt, RSA 4096 root).
// Found as "mbedtls_config.h" through the SDK's pico_mbedtls_config.h.

#ifndef CRULLER_MBEDTLS_CONFIG_H
#define CRULLER_MBEDTLS_CONFIG_H

// Memory: tls.c points mbedTLS at the FreeRTOS heap (the C library's is what RAM has left, ~24 KB).
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

// Randomness: the RP2350's (pico_mbedtls.c's mbedtls_hardware_poll, from pico_rand).
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_CTR_DRBG_C

// No clock: certificates' dates aren't checked (MBEDTLS_HAVE_TIME_DATE stays off).

// Ciphers: AES-GCM, tables in flash.
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

// Public keys: ECDHE and ECDSA on P-256, P-384 and X25519; RSA up to 4096 bits.
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_MPI_MAX_SIZE 512
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

// Certificates.
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

// TLS 1.2 client. Records from the servers can be 16 KB; what goes out is a request line.
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_CIPHERSUITES \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#endif
