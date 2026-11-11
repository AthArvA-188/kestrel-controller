#include "ks_wifi_proto.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

static void put_name(uint8_t *p, const char *name)
{
    size_t n = strlen(name);
    memset(p, 0, KS_WP_NAME_LEN);
    memcpy(p, name, n < KS_WP_NAME_LEN ? n : KS_WP_NAME_LEN);
}

static void get_name(char *name, const uint8_t *p)
{
    memcpy(name, p, KS_WP_NAME_LEN);
    name[KS_WP_NAME_LEN] = '\0';
}

static size_t packet_len(uint8_t type)
{
    switch (type) {
    case KS_WP_DISCOVER: return KS_WP_DISCOVER_LEN;
    case KS_WP_OFFER: return KS_WP_OFFER_LEN;
    case KS_WP_INPUT: return KS_WP_INPUT_LEN;
    case KS_WP_FEEDBACK: return KS_WP_FEEDBACK_LEN;
    case KS_WP_BYE: return KS_WP_BYE_LEN;
    default: return 0;
    }
}

size_t ks_wp_encode(const ks_wp_packet_t *p, uint8_t *buf, size_t cap)
{
    size_t len = packet_len(p->type);
    if (len == 0 || cap < len) {
        return 0;
    }
    uint8_t *b = buf + KS_WP_HEADER_LEN;
    memset(buf, 0, len);
    buf[0] = 'K';
    buf[1] = 'S';
    buf[2] = KS_WP_VERSION;
    buf[3] = p->type;
    put16(buf + 4, p->seq);
    put16(buf + 6, p->session);

    switch (p->type) {
    case KS_WP_DISCOVER:
        memcpy(b, p->u.discover.device_id, 6);
        put16(b + 6, p->u.discover.fw_version);
        b[8] = p->u.discover.battery_pct;
        b[9] = p->u.discover.flags;
        put_name(b + 10, p->u.discover.name);
        break;
    case KS_WP_OFFER:
        put_name(b, p->u.offer.name);
        put16(b + 16, p->u.offer.rate_hz);
        b[18] = p->u.offer.slot;
        break;
    case KS_WP_INPUT: {
        const ks_gamepad_t *g = &p->u.input.pad;
        put32(b, g->buttons);
        put16(b + 4, (uint16_t)g->lx);
        put16(b + 6, (uint16_t)g->ly);
        put16(b + 8, (uint16_t)g->rx);
        put16(b + 10, (uint16_t)g->ry);
        b[12] = g->lt;
        b[13] = g->rt;
        b[14] = p->u.input.battery_pct;
        b[15] = p->u.input.flags;
        put32(b + 16, p->u.input.time_ms);
        break;
    }
    case KS_WP_FEEDBACK: {
        const ks_wp_feedback_t *f = &p->u.feedback;
        b[0] = f->rumble.left;
        b[1] = f->rumble.right;
        b[2] = f->fx_valid;
        for (int i = 0; i < 2; i++) {
            b[4 + 4 * i] = f->fx[i].effect;
            b[5 + 4 * i] = f->fx[i].start;
            b[6 + 4 * i] = f->fx[i].strength;
            b[7 + 4 * i] = f->fx[i].param;
        }
        put16(b + 12, f->ack_seq);
        break;
    }
    default:
        break;
    }
    return len;
}

bool ks_wp_decode(const uint8_t *buf, size_t len, ks_wp_packet_t *p)
{
    if (len < KS_WP_HEADER_LEN || buf[0] != 'K' || buf[1] != 'S' || buf[2] != KS_WP_VERSION) {
        return false;
    }
    size_t want = packet_len(buf[3]);
    if (want == 0 || len < want) {
        return false;
    }
    const uint8_t *b = buf + KS_WP_HEADER_LEN;
    memset(p, 0, sizeof *p);
    p->type = buf[3];
    p->seq = get16(buf + 4);
    p->session = get16(buf + 6);

    switch (p->type) {
    case KS_WP_DISCOVER:
        memcpy(p->u.discover.device_id, b, 6);
        p->u.discover.fw_version = get16(b + 6);
        p->u.discover.battery_pct = b[8];
        p->u.discover.flags = b[9];
        get_name(p->u.discover.name, b + 10);
        break;
    case KS_WP_OFFER:
        get_name(p->u.offer.name, b);
        p->u.offer.rate_hz = get16(b + 16);
        p->u.offer.slot = b[18];
        break;
    case KS_WP_INPUT: {
        ks_gamepad_t *g = &p->u.input.pad;
        g->buttons = get32(b);
        g->lx = (int16_t)get16(b + 4);
        g->ly = (int16_t)get16(b + 6);
        g->rx = (int16_t)get16(b + 8);
        g->ry = (int16_t)get16(b + 10);
        g->lt = b[12];
        g->rt = b[13];
        p->u.input.battery_pct = b[14];
        p->u.input.flags = b[15];
        p->u.input.time_ms = get32(b + 16);
        break;
    }
    case KS_WP_FEEDBACK: {
        ks_wp_feedback_t *f = &p->u.feedback;
        f->rumble.left = b[0];
        f->rumble.right = b[1];
        f->fx_valid = b[2];
        for (int i = 0; i < 2; i++) {
            f->fx[i] = (ks_trigger_fx_cfg_t){b[4 + 4 * i], b[5 + 4 * i], b[6 + 4 * i],
                                             b[7 + 4 * i]};
            if (f->fx[i].effect >= KS_FX_COUNT) {
                return false;
            }
        }
        f->ack_seq = get16(b + 12);
        break;
    }
    default:
        break;
    }
    return true;
}
