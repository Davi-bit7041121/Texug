#ifndef TEXUGOS_WPA2_CRYPTO_H
#define TEXUGOS_WPA2_CRYPTO_H

#include <stdint.h>

int wpa2_derive_pmk(const uint8_t *passphrase, uint32_t passphrase_length,
                    const uint8_t *ssid, uint32_t ssid_length,
                    uint8_t pmk[32]);
int wpa2_derive_ptk(const uint8_t pmk[32],
                    const uint8_t authenticator_address[6],
                    const uint8_t supplicant_address[6],
                    const uint8_t authenticator_nonce[32],
                    const uint8_t supplicant_nonce[32], uint8_t ptk[64]);
int wpa2_eapol_mic(const uint8_t kck[16], const uint8_t *eapol,
                   uint32_t eapol_length, uint8_t mic[16]);
int wpa2_ccmp_encrypt(const uint8_t temporal_key[16],
                      const uint8_t nonce[13], const uint8_t *aad,
                      uint32_t aad_length, uint8_t *data,
                      uint32_t data_length, uint8_t mic[8]);
int wpa2_ccmp_decrypt(const uint8_t temporal_key[16],
                      const uint8_t nonce[13], const uint8_t *aad,
                      uint32_t aad_length, uint8_t *data,
                      uint32_t data_length, const uint8_t mic[8]);

#endif
