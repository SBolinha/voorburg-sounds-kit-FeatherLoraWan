/* Host-only fixture: compile the real LMIC frame builder, not a crypto mock.
 * No radio is initialized, and no private sketch credentials are included.
 * lmic.c is included so this harness can call its static buildDataFrame().
 */
#include <stdio.h>
#include <stdlib.h>
#include <lmic/lmic.c>

/* Only board/time inputs used while initializing EU868 channel state. */
ostime_t os_getTime(void) { return 0; }
u1_t radio_rand1(void) { return 0; }

void lmic_hal_failed(const char *file, u2_t line) {
    fprintf(stderr, "LMIC assertion: %s:%u\n", file, (unsigned int) line);
    abort();
}

int main(int argc, char **argv) {
    /* Public NIST AES test material. These are never device session keys. */
    u1_t nwk[] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
    };
    u1_t app[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };
    const devaddr_t address = 0x01020304;
    u1_t readNwk[16], readApp[16];
    u4_t readNet;
    devaddr_t readAddress;
    size_t payloadLength;
    char *end;
    unsigned long counter, port;

    if (argc != 4) return 2;
    counter = strtoul(argv[1], &end, 10);
    if (*end || counter >= UINT32_MAX) return 2;
    port = strtoul(argv[2], &end, 10);
    if (*end || port > 255) return 2;
    payloadLength = strlen(argv[3]);
    if ((payloadLength & 1) || payloadLength / 2 > sizeof(LMIC.pendTxData)) return 2;
    payloadLength /= 2;

    memset(&LMIC, 0, sizeof(LMIC));
    LMIC_setSession(0x13, address, nwk, app);
    LMIC_getSessionKeys(&readNet, &readAddress, readNwk, readApp);
    printf("keys_match=%d\n", readNet == 0x13 && readAddress == address &&
        memcmp(readNwk, nwk, sizeof(nwk)) == 0 &&
        memcmp(readApp, app, sizeof(app)) == 0);

    /* Match the Feather's unconfirmed, ADR-disabled uplink state. */
    LMIC_setLinkCheckMode(0);
    LMIC_setAdrMode(0);
    LMIC.datarate = EU868_DR_SF7;
    LMIC.seqnoUp = (u4_t) counter;
    LMIC.opmode |= OP_TXDATA;
    LMIC.pendTxPort = (u1_t) port;
    LMIC.pendTxLen = (u1_t) payloadLength;
    for (size_t i = 0; i < payloadLength; ++i) {
        char byte[3] = { argv[3][i * 2], argv[3][i * 2 + 1], 0 };
        unsigned long value = strtoul(byte, &end, 16);
        if (*end) return 2;
        LMIC.pendTxData[i] = (u1_t) value;
    }
    if (!buildDataFrame()) return 3;
    printf("frame=");
    for (size_t i = 0; i < LMIC.dataLen; ++i) printf("%02x", LMIC.frame[i]);
    printf("\nnext_counter=%lu\n", (unsigned long) LMIC.seqnoUp);
    return 0;
}
