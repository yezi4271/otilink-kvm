/*
 * otimock.c —— 见 otimock.h
 */
#include "otimock.h"

#include <stdio.h>
#include <string.h>

/* ---- 两条管道模型（NOTES §34）：消息管道 16B / 帧管道 64KB ---- */
static void mock_pop_msg(struct otimock *m, uint8_t *data)
{
    memset(data, 0, 16);
    if (m->count > 0) {                 /* 0x05：对端有数据，be16 为帧数 */
        data[0] = 0x05;
        data[1] = (uint8_t)(m->count >> 8);
        data[2] = (uint8_t)(m->count & 0xff);
    } else {
        data[0] = 0x07;                  /* 0x07：发送授权 */
    }
}

static void mock_pop_frame(struct otimock *m, uint8_t *data)
{
    if (m->count == 0) {
        memset(data, 0, OTI_FRAME_SIZE); /* 空闲帧：全零 */
        m->rd_idle++;
        return;
    }
    memcpy(data, m->q[m->head], OTI_FRAME_SIZE);
    m->head = (m->head + 1) % OTIMOCK_QSLOTS;
    m->count--;
    m->rd_data++;
}


static int mock_exec(void *ctx, const uint8_t cdb[16], void *data, unsigned len,
                     int to_dev, struct oti_scsi_result *res)
{
    struct otimock *m = ctx;
    (void)to_dev;
    if (res)
        memset(res, 0, sizeof(*res));

    /* 魔数校验：厂商 CDB 尾两字节固定 'O','T' */
    if (cdb[14] != 'O' || cdb[15] != 'T') {
        m->bad_magic++;
        if (res) {
            res->status = 0x02;              /* CHECK CONDITION */
            res->sense[0] = 0x70;
            res->sense[2] = 0x05;            /* ILLEGAL REQUEST */
            res->sense_len = 18;
            res->rc = 0x100 | 0x02;
        }
        return 0x100 | 0x02;
    }

    switch (cdb[0]) {
    case OTI_OP_QUERY:
        if (cdb[1] == OTI_SUB_LOCK) {
            m->locked = cdb[2] != 0;
            return 0;
        }
        if (cdb[1] == OTI_SUB_USB_RESTART) {
            m->usb_restarts++;
            return 0;
        }
        m->queries++;
        /* 设备信息块：前 12 字节 IC 版本，[12..13]=侧别，[14..15]=功能类型（仿真约定） */
        if (data && len) {
            uint8_t blk[64];
            memset(blk, 0, sizeof(blk));
            for (int i = 0; i < 16; i++)
                blk[i] = m->status[i];
            blk[12] = 1;
            blk[13] = 0;                /* side type = 1 */
            blk[14] = 2;
            blk[15] = 0;                /* function type = 2 */
            unsigned n = len < 24 ? len : 24;
            memcpy(data, blk, n);
            if (res)
                res->resid = (int)(len - n);
        }
        return 0;

    case OTI_OP_WRITE:
        if (cdb[1] == OTI_SUB_DEV_MODE) {
            if (to_dev) {                /* 写模式 */
                if (data && len)
                    m->dev_mode = ((uint8_t *)data)[0];
                return 0;
            }
            if (data && len) {           /* 读模式 */
                ((uint8_t *)data)[0] = m->dev_mode;
                if (res)
                    res->resid = (int)(len - 1);
            }
            return 0;
        }
        if (cdb[1] == OTI_SUB_DATA) {
            if (len != OTI_FRAME_SIZE) {
                m->wr_rej++;
                if (res) {
                    res->status = 0x02;
                    res->rc = 0x100 | 0x02;
                    res->resid = (int)len;
                }
                return 0x100 | 0x02;
            }
            if (m->tail_check &&
                memcmp(data, (uint8_t *)data + OTI_FRAME_SIZE - OTI_FRAME_HEAD,
                       OTI_FRAME_HEAD) != 0) {
                m->wr_rej++;
                if (res) {
                    res->status = 0x02;      /* 首尾头不一致 → 设备拒收 */
                    res->sense[0] = 0x70;
                    res->sense[2] = 0x0C;    /* WRITE ERROR */
                    res->sense_len = 18;
                    res->rc = 0x100 | 0x02;
                }
                return 0x100 | 0x02;
            }
            if (m->peer) {
                if (m->peer->count >= OTIMOCK_QSLOTS) {
                    m->peer->q_drop++;
                } else {
                    memcpy(m->peer->q[m->peer->tail], data, OTI_FRAME_SIZE);
                    m->peer->tail = (m->peer->tail + 1) % OTIMOCK_QSLOTS;
                    m->peer->count++;
                }
            }
            m->wr_ok++;
            return 0;
        }
        if (cdb[1] == OTI_SUB_FRAME_RD) {      /* 0xD9/0x28/0x64：帧管道读（64KB IN） */
            if (to_dev || len != OTI_FRAME_SIZE) {
                if (res) { res->status = 0x02; res->rc = 0x100 | 0x02; }
                return 0x100 | 0x02;
            }
            mock_pop_frame(m, data);
            return 0;
        }
        if (cdb[1] == OTI_SUB_HID_TYPE1 || cdb[1] == OTI_SUB_HID_TYPE2 ||
            cdb[1] == OTI_SUB_HID_TYPE3) {
            memcpy(m->last_hid, &cdb[2], OTI_HID_CDB_COPY);
            m->last_hid_sub = cdb[1];
            m->hid_pkts++;
            return 0;
        }
        if (res) {
            res->status = 0x02;
            res->rc = 0x100 | 0x02;
        }
        return 0x100 | 0x02;

    case OTI_OP_READ: {
        if (to_dev && cdb[1] == OTI_SUB_REMOTE_ST) {   /* 0xD8/0x01：设置远端主机状态 */
            m->remote_status = (uint16_t)(((const uint8_t *)data)[0] |
                                          (((const uint8_t *)data)[1] << 8));
            m->rs_writes++;
            return 0;
        }
        if (to_dev) {                    /* 0xD8 的其它写方向：本仿真不支持 */
            if (res) {
                res->status = 0x02;
                res->rc = 0x100 | 0x02;
            }
            return 0x100 | 0x02;
        }
        if (len == 16) {                 /* 消息管道：0xD8/0x00/0x03 + IN 16B */
            mock_pop_msg(m, data);
            return 0;
        }
        if (len != OTI_FRAME_SIZE) {
            if (res) {
                res->status = 0x02;
                res->rc = 0x100 | 0x02;
            }
            return 0x100 | 0x02;
        }
        /* 兼容旧调用（64KB）：直接投递一帧 */
        mock_pop_frame(m, data);
        return 0;
    }
    default:
        if (res) {
            res->status = 0x02;
            res->rc = 0x100 | 0x02;
        }
        return 0x100 | 0x02;
    }
}


void otimock_init(struct otimock *m)
{
    memset(m, 0, sizeof(*m));
    m->tail_check = 1;
    for (int i = 0; i < 16; i++)
        m->status[i] = (uint8_t)(0xA0 + i);
}

void otimock_pair(struct otilink_dev *a, struct otimock *ma,
                  struct otilink_dev *b, struct otimock *mb)
{
    otimock_init(ma);
    otimock_init(mb);
    ma->peer = mb;
    mb->peer = ma;

    memset(a, 0, sizeof(*a));
    memset(b, 0, sizeof(*b));
    a->sg = -1;
    b->sg = -1;
    snprintf(a->sg_path, sizeof(a->sg_path), "mock:A");
    snprintf(b->sg_path, sizeof(b->sg_path), "mock:B");
    struct oti_scsi_ops oa = {.exec = mock_exec, .ctx = ma};
    struct oti_scsi_ops ob = {.exec = mock_exec, .ctx = mb};
    otilink_set_ops(a, &oa);
    otilink_set_ops(b, &ob);
}
