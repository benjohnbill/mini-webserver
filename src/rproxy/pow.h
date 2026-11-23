#ifndef POW_H
#define POW_H

#include <stdbool.h>
#include <stddef.h>

/* Decision module of the Reverse Proxy: the Proof of work. The server
   sends "X-PoW-Challenge: NONCE BITS". The client sends
   "X-PoW: NONCE:COUNTER". SHA-256 of the text "NONCE:COUNTER" must start
   with BITS zero bits. The nonce is HMAC-SHA256 of the client IP and the
   time window, so the server keeps no state. Time and the secret are
   arguments: no clock, no I/O. */

/* A nonce is the HMAC in lowercase hex. */
#define POW_NONCE_LEN 64
/* The time step of RFC 6238 (TOTP), section 5.2. A nonce is valid in its
   window and in the next one, so for 30 to 60 seconds. */
#define POW_WINDOW_SECONDS 30
/* SHA-256 has 256 bits. */
#define POW_BITS_MAX 256
/* "NONCE BITS": 64 + 1 + 3. */
#define POW_CHALLENGE_MAX (POW_NONCE_LEN + 1 + 3)

/* Writes the value of X-PoW-Challenge, "NONCE BITS", and a terminating NUL
   into challenge, which has POW_CHALLENGE_MAX + 1 bytes. now is seconds
   since the Epoch. bits is 0 to POW_BITS_MAX. */
void pow_format_challenge(const char *client_ip, double now,
                          const unsigned char *secret, size_t secret_len,
                          int bits, char *challenge);

/* Asks if solution, the value of X-PoW, solves a challenge of bits for
   client_ip at now. The nonce must be the nonce of client_ip for the
   window of now or for the window before it. The counter is 1 to 20
   decimal digits. Returns false if solution is NULL or malformed. */
bool pow_is_solved(const char *solution, const char *client_ip, double now,
                   const unsigned char *secret, size_t secret_len, int bits);

#endif
