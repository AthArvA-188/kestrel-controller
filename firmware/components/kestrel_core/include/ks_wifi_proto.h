// UDP packets exchanged with the PC receiver. docs/wifi-protocol.md is the
// reference. All multi-byte fields are little-endian.
#pragma once

#include "ks_types.h"

#define KS_WP_PORT 47810
#define KS_WP_VERSION 1
#define KS_WP_NAME_LEN 16

typedef enum {
    KS_WP_DISCOVER = 1, // controller to broadcast
    KS_WP_OFFER = 2,    // receiver to controller
    KS_WP_INPUT = 3,    // controller to receiver
    KS_WP_FEEDBACK = 4, // receiver to controller
    KS_WP_BYE = 5       // either way
} ks_wp_type_t;

#define KS_WP_HEADER_LEN 8
#define KS_WP_DISCOVER_LEN 34
#define KS_WP_OFFER_LEN 28
#define KS_WP_INPUT_LEN 28
#define KS_WP_FEEDBACK_LEN 22
#define KS_WP_BYE_LEN 8
#define KS_WP_MAX_LEN 34

#define KS_WP_FLAG_CHARGING 0x01
#define KS_WP_FLAG_MENU_OPEN 0x02

typedef struct {
    uint8_t device_id[6];
    uint16_t fw_version;
    uint8_t battery_pct;
    uint8_t flags;
    char name[KS_WP_NAME_LEN + 1];
} ks_wp_discover_t;

typedef struct {
    char name[KS_WP_NAME_LEN + 1];
    uint16_t rate_hz; // input rate the receiver asks for
    uint8_t slot;     // player number, from 1
} ks_wp_offer_t;

typedef struct {
    ks_gamepad_t pad;
    uint8_t battery_pct;
    uint8_t flags;
    uint32_t time_ms;
} ks_wp_input_t;

typedef struct {
    ks_rumble_t rumble;
    uint8_t fx_valid; // KS_FX_TARGET_* bits; a clear bit keeps the profile's effect
    ks_trigger_fx_cfg_t fx[2];
    uint16_t ack_seq; // sequence number of the last input packet received
} ks_wp_feedback_t;

typedef struct {
    uint8_t type;
    uint16_t seq;
    uint16_t session;
    union {
        ks_wp_discover_t discover;
        ks_wp_offer_t offer;
        ks_wp_input_t input;
        ks_wp_feedback_t feedback;
    } u;
} ks_wp_packet_t;

// Returns the encoded length, or 0 when the type is unknown or cap is too small.
size_t ks_wp_encode(const ks_wp_packet_t *p, uint8_t *buf, size_t cap);
// Returns false for a bad magic, version, type, or a packet that is too short.
// Trailing bytes are ignored so that later versions can append fields.
bool ks_wp_decode(const uint8_t *buf, size_t len, ks_wp_packet_t *p);
