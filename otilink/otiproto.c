/*
 * otiproto.c —— 见 otiproto.h
 */
#include "otiproto.h"

#include <string.h>

/* ---------- CRC32 (IEEE 802.3, poly 0xEDB88320 反射) ---------- */
static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

uint32_t oti_crc32(const void *data, size_t len)
{
    if (!crc_ready)
        crc_init();
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------- 小端读写 ---------- */
static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t oti_encode(uint16_t type, uint16_t flags, uint32_t seq,
                  const void *payload, uint32_t len, uint8_t *out, size_t outcap)
{
    if (!out || len > OTI_PAYLOAD_MAX || outcap < OTI_HDR_SIZE + (size_t)len)
        return 0;
    if (len && !payload)
        return 0;
    put32(out + 0, OTI_MSG_MAGIC);
    put16(out + 4, type);
    put16(out + 6, flags);
    put32(out + 8, seq);
    put32(out + 12, len);
    if (len)
        memcpy(out + OTI_HDR_SIZE, payload, len);
    uint32_t crc = oti_crc32(out, 16);
    if (len)
        crc = oti_crc32_append(crc, out + OTI_HDR_SIZE, len);
    put32(out + 16, crc);
    return OTI_HDR_SIZE + (size_t)len;
}

/* 增量 CRC：把前一段的结果接着算下去（避免拼接临时缓冲） */
uint32_t oti_crc32_append(uint32_t crc, const void *data, size_t len)
{
    if (!crc_ready)
        crc_init();
    const uint8_t *p = data;
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

int oti_decode(const uint8_t *buf, size_t len, oti_msg_hdr *hdr, const uint8_t **payload)
{
    if (!buf || len < OTI_HDR_SIZE)
        return -1;
    if (get32(buf + 0) != OTI_MSG_MAGIC)
        return -2;
    uint32_t plen = get32(buf + 12);
    if (plen > OTI_PAYLOAD_MAX || OTI_HDR_SIZE + (size_t)plen > len)
        return -3;
    if (hdr) {
        hdr->magic = OTI_MSG_MAGIC;
        hdr->type = get16(buf + 4);
        hdr->flags = get16(buf + 6);
        hdr->seq = get32(buf + 8);
        hdr->len = plen;
        hdr->crc = get32(buf + 16);
    }
    uint32_t crc = oti_crc32(buf, 16);
    if (plen)
        crc = oti_crc32_append(crc, buf + OTI_HDR_SIZE, plen);
    if (crc != get32(buf + 16))
        return -4;
    if (payload)
        *payload = buf + OTI_HDR_SIZE;
    return 0;
}

/* ---------- 便捷封装 ---------- */
size_t oti_encode_key(uint32_t seq, const oti_key_evt *e, uint8_t *out, size_t cap)
{
    uint8_t p[4];
    put16(p, e->code);
    p[2] = e->value;
    p[3] = e->mods;
    return oti_encode(OTI_MSG_KEY, 0, seq, p, sizeof(p), out, cap);
}

size_t oti_encode_mouse(uint32_t seq, const oti_mouse_evt *e, uint8_t *out, size_t cap)
{
    uint8_t p[8];
    put16(p + 0, (uint16_t)e->dx);
    put16(p + 2, (uint16_t)e->dy);
    put16(p + 4, (uint16_t)e->wheel);
    put16(p + 6, e->buttons);
    return oti_encode(OTI_MSG_MOUSE, 0, seq, p, sizeof(p), out, cap);
}

size_t oti_encode_switch(uint32_t seq, const oti_switch_evt *e, uint8_t *out, size_t cap)
{
    uint8_t p[12];
    p[0] = e->side;
    p[1] = e->use_hotkey_only;
    put16(p + 2, (uint16_t)e->edge_x);
    put16(p + 4, (uint16_t)e->edge_y);
    put16(p + 6, e->screen_w);
    put16(p + 8, e->screen_h);
    put16(p + 10, 0);
    return oti_encode(OTI_MSG_SWITCH, 0, seq, p, sizeof(p), out, cap);
}

size_t oti_encode_clip(uint32_t seq, const oti_clip_hdr *h, const void *data,
                       uint32_t dlen, uint8_t *out, size_t cap)
{
    uint8_t buf[20 + OTI_CLIP_CHUNK_MAX];
    if (dlen > OTI_CLIP_CHUNK_MAX)
        return 0;
    put16(buf + 0, h->format);
    put16(buf + 2, h->flags);
    put32(buf + 4, h->fid);
    put32(buf + 8, h->offset);
    put32(buf + 12, h->total_len);
    put32(buf + 16, dlen);
    if (dlen && data)
        memcpy(buf + 20, data, dlen);
    return oti_encode(OTI_MSG_CLIP, 0, seq, buf, 20 + dlen, out, cap);
}

size_t oti_encode_ack(uint32_t seq, uint32_t crc, uint16_t format, uint8_t *out, size_t cap)
{
    uint8_t buf[8];
    put32(buf + 0, crc);
    put16(buf + 4, format);
    put16(buf + 6, 0);
    return oti_encode(OTI_MSG_ACK, 0, seq, buf, sizeof(buf), out, cap);
}

int oti_decode_ack(const uint8_t *payload, uint32_t len, uint32_t *crc, uint16_t *format)
{
    if (len < 8)
        return -1;
    if (crc)
        *crc = get32(payload + 0);
    if (format)
        *format = get16(payload + 4);
    return 0;
}

size_t oti_encode_ping(uint32_t seq, uint32_t stamp_ms, uint8_t *out, size_t cap)
{
    uint8_t p[4];
    put32(p, stamp_ms);
    return oti_encode(OTI_MSG_PING, 0, seq, p, sizeof(p), out, cap);
}

/* ---------- 解码 ---------- */
int oti_decode_key(const uint8_t *p, uint32_t len, oti_key_evt *out)
{
    if (len < 4)
        return -1;
    out->code = get16(p);
    out->value = p[2];
    out->mods = p[3];
    return 0;
}

int oti_decode_mouse(const uint8_t *p, uint32_t len, oti_mouse_evt *out)
{
    if (len < 8)
        return -1;
    out->dx = (int16_t)get16(p + 0);
    out->dy = (int16_t)get16(p + 2);
    out->wheel = (int16_t)get16(p + 4);
    out->buttons = get16(p + 6);
    return 0;
}

int oti_decode_switch(const uint8_t *p, uint32_t len, oti_switch_evt *out)
{
    if (len < 10)
        return -1;
    out->side = p[0];
    out->use_hotkey_only = p[1];
    out->edge_x = (int16_t)get16(p + 2);
    out->edge_y = (int16_t)get16(p + 4);
    out->screen_w = get16(p + 6);
    out->screen_h = get16(p + 8);
    return 0;
}

int oti_decode_clip(const uint8_t *p, uint32_t len, oti_clip_hdr *h,
                    const uint8_t **data, uint32_t *dlen)
{
    if (len < 20)
        return -1;
    h->format = get16(p + 0);
    h->flags = get16(p + 2);
    h->fid = get32(p + 4);
    h->offset = get32(p + 8);
    h->total_len = get32(p + 12);
    uint32_t dl = get32(p + 16);
    if (20 + (size_t)dl > len)
        return -2;
    if (data)
        *data = p + 20;
    if (dlen)
        *dlen = dl;
    return 0;
}

size_t oti_encode_mouse_abs(uint32_t seq, const oti_mouse_abs_evt *e, uint8_t *out, size_t cap)
{
    uint8_t pl[8];
    int16_t x = e->x, y = e->y, w = e->wheel;
    uint16_t b = e->buttons;
    memcpy(pl + 0, &x, 2);
    memcpy(pl + 2, &y, 2);
    memcpy(pl + 4, &w, 2);
    memcpy(pl + 6, &b, 2);
    return oti_encode(OTI_MSG_MOUSE_ABS, 0, seq, pl, sizeof(pl), out, cap);
}

int oti_decode_mouse_abs(const uint8_t *payload, uint32_t len, oti_mouse_abs_evt *out)
{
    if (len < 8)
        return -1;
    memcpy(&out->x, payload + 0, 2);
    memcpy(&out->y, payload + 2, 2);
    memcpy(&out->wheel, payload + 4, 2);
    memcpy(&out->buttons, payload + 6, 2);
    return 0;
}

size_t oti_encode_hello(uint32_t seq, const oti_hello_evt *e, uint8_t *out, size_t cap)
{
    uint8_t pl[4];
    uint16_t w = e->width, h = e->height;
    memcpy(pl + 0, &w, 2);
    memcpy(pl + 2, &h, 2);
    return oti_encode(OTI_MSG_HELLO, 0, seq, pl, sizeof(pl), out, cap);
}

int oti_decode_hello(const uint8_t *payload, uint32_t len, oti_hello_evt *out)
{
    if (len < 4)
        return -1;
    memcpy(&out->width, payload + 0, 2);
    memcpy(&out->height, payload + 2, 2);
    return 0;
}

size_t oti_encode_mouse_mode(uint32_t seq, uint8_t mode, uint8_t *out, size_t cap)
{
    return oti_encode(OTI_MSG_MOUSE_MODE, 0, seq, &mode, 1, out, cap);
}

int oti_decode_mouse_mode(const uint8_t *payload, uint32_t len, uint8_t *mode_out)
{
    if (len < 1)
        return -1;
    *mode_out = payload[0];
    return 0;
}

/* ---------- 角色协商（type=10）---------- */
size_t oti_encode_role(uint32_t seq, const oti_role_evt *e, uint8_t *out, size_t cap)
{
    uint8_t p[12];
    p[0] = e->has_local_input ? 1 : 0;
    p[1] = e->want;
    p[2] = e->state;
    p[3] = e->flags;
    put32(p + 4, e->boot_id);
    put32(p + 8, e->input_age_ms);
    return oti_encode(OTI_MSG_ROLE, 0, seq, p, sizeof(p), out, cap);
}

int oti_decode_role(const uint8_t *payload, uint32_t len, oti_role_evt *out)
{
    if (!payload || !out || len < 12)
        return -1;
    memset(out, 0, sizeof(*out));
    out->has_local_input = payload[0] ? 1 : 0;
    out->want = payload[1];
    out->state = payload[2];
    out->flags = payload[3];
    out->boot_id = get32(payload + 4);
    out->input_age_ms = get32(payload + 8);
    return 0;
}
