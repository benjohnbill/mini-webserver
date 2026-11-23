#ifndef CLIENT_HELLO_H
#define CLIENT_HELLO_H

#include <stddef.h>
#include <stdint.h>

/* Decision module of the Reverse Proxy: no sockets, no OpenSSL. */

#define CLIENT_HELLO_CIPHER_MAX 128
#define CLIENT_HELLO_EXTENSION_MAX 64
#define CLIENT_HELLO_VERSION_MAX 16
#define CLIENT_HELLO_SIGNATURE_MAX 64
#define CLIENT_HELLO_ALPN_MAX 255

enum client_hello_status {
    CLIENT_HELLO_OK,
    /* The bytes are a correct start of a ClientHello record. Read more. */
    CLIENT_HELLO_INCOMPLETE,
    /* Not a TLS handshake record, not a ClientHello, a length that does
       not fit, or a list longer than its _MAX. */
    CLIENT_HELLO_INVALID,
};

/* The values of one ClientHello in the order of the wire. GREASE values
   stay in the lists. A list is empty when its extension is not there. */
struct client_hello {
    uint16_t legacy_version;
    uint16_t ciphers[CLIENT_HELLO_CIPHER_MAX];
    size_t cipher_count;
    uint16_t extensions[CLIENT_HELLO_EXTENSION_MAX];
    size_t extension_count;
    /* From the supported_versions extension (0x002b). */
    uint16_t versions[CLIENT_HELLO_VERSION_MAX];
    size_t version_count;
    /* From the signature_algorithms extension (0x000d). */
    uint16_t signatures[CLIENT_HELLO_SIGNATURE_MAX];
    size_t signature_count;
    /* The first protocol of the ALPN extension (0x0010), not terminated. */
    unsigned char alpn[CLIENT_HELLO_ALPN_MAX];
    size_t alpn_len;
};

/* Parses one TLS record that starts at bytes[0] and holds a full
   ClientHello. A ClientHello that spans two records is
   CLIENT_HELLO_INVALID. */
enum client_hello_status client_hello_parse(const unsigned char *bytes,
                                            size_t len,
                                            struct client_hello *hello);

#endif
