/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * This code is licensed under the GNU GPLv2 and later.
 */

#include "hw/dma/bcm2835_dma.h"

/* DMA CS Control and Status bits */
#define BCM2708_DMA_ACTIVE      (1 << 0)
#define BCM2708_DMA_INT         (1 << 2)
#define BCM2708_DMA_ISPAUSED    (1 << 4)  /* Pause requested or not active */
#define BCM2708_DMA_ISHELD      (1 << 5)  /* Is held by DREQ flow control */
#define BCM2708_DMA_ERR         (1 << 8)
#define BCM2708_DMA_ABORT       (1 << 30) /* stop current CB, go to next, WO */
#define BCM2708_DMA_RESET       (1 << 31) /* WO, self clearing */

#define BCM2708_DMA_END         (1 << 1) /* GE */

/* DMA control block "info" field bits */
#define BCM2708_DMA_INT_EN      (1 << 0)
#define BCM2708_DMA_TDMODE      (1 << 1)
#define BCM2708_DMA_WAIT_RESP   (1 << 3)
#define BCM2708_DMA_D_INC       (1 << 4)
#define BCM2708_DMA_D_WIDTH     (1 << 5)
#define BCM2708_DMA_D_DREQ      (1 << 6)
#define BCM2708_DMA_D_IGNORE    (1 << 7)
#define BCM2708_DMA_S_INC       (1 << 8)
#define BCM2708_DMA_S_WIDTH     (1 << 9)
#define BCM2708_DMA_S_DREQ      (1 << 10)
#define BCM2708_DMA_S_IGNORE    (1 << 11)

#define BCM2708_DMA_BURST(x)    (((x)&0xf) << 12)
#define BCM2708_DMA_PER_MAP(x)  ((x) << 16)
#define BCM2708_DMA_WAITS(x)    (((x)&0x1f) << 21)

#define BCM2708_DMA_DREQ_EMMC   11
#define BCM2708_DMA_DREQ_SDHOST 13

#define BCM2708_DMA_CS          0x00 /* Control and Status */
#define BCM2708_DMA_ADDR        0x04
/* the current control block appears in the following registers - read only */
#define BCM2708_DMA_INFO        0x08
#define BCM2708_DMA_NEXTCB      0x1C
#define BCM2708_DMA_DEBUG       0x20

#define BCM2708_DMA4_CS         (BCM2708_DMA_CHAN(4)+BCM2708_DMA_CS)
#define BCM2708_DMA4_ADDR       (BCM2708_DMA_CHAN(4)+BCM2708_DMA_ADDR)

#define BCM2708_DMA_TDMODE_LEN(w, h) ((h) << 16 | (w))

static void bcm2835_dma_update(BCM2835DmaState *s, int c)
{
    BCM2835DmaChan *ch = &s->chan[c];
    uint32_t data, xlen, ylen;
    int16_t dst_stride, src_stride;

    if (!(s->enable & (1 << c))) {
        return;
    }

    while ((s->enable & (1 << c)) && (ch->conblk_ad != 0)) {
        /* CB fetch */
        ch->ti = ldl_phys(&s->dma_as, ch->conblk_ad);
        ch->source_ad = ldl_phys(&s->dma_as, ch->conblk_ad + 4);
        ch->dest_ad = ldl_phys(&s->dma_as, ch->conblk_ad + 8);
        ch->txfr_len = ldl_phys(&s->dma_as, ch->conblk_ad + 12);
        ch->stride = ldl_phys(&s->dma_as, ch->conblk_ad + 16);
        ch->nextconbk = ldl_phys(&s->dma_as, ch->conblk_ad + 20);

        if (ch->ti & BCM2708_DMA_TDMODE) {
            /* 2D transfer mode */
            ylen = (ch->txfr_len >> 16) & 0x3fff;
            xlen = ch->txfr_len & 0xffff;
            dst_stride = ch->stride >> 16;
            src_stride = ch->stride & 0xffff;
        } else {
            ylen = 1;
            xlen = ch->txfr_len;
            dst_stride = 0;
            src_stride = 0;
        }

        while (ylen != 0) {
            /* Normal transfer mode */
            while (xlen != 0) {
                if (ch->ti & BCM2708_DMA_S_IGNORE) {
                    /* Ignore reads */
                    data = 0;
                } else {
                    data = ldl_phys(&s->dma_as, ch->source_ad);
                }
                if (ch->ti & BCM2708_DMA_S_INC) {
                    ch->source_ad += 4;
                }

                if (ch->ti & BCM2708_DMA_D_IGNORE) {
                    /* Ignore writes */
                } else {
                    stl_phys(&s->dma_as, ch->dest_ad, data);
                }
                if (ch->ti & BCM2708_DMA_D_INC) {
                    ch->dest_ad += 4;
                }

                /* update remaining transfer length */
                xlen -= 4;
                if (ch->ti & BCM2708_DMA_TDMODE) {
                    ch->txfr_len = (ylen << 16) | xlen;
                } else {
                    ch->txfr_len = xlen;
                }
            }

            if (--ylen != 0) {
                ch->source_ad += src_stride;
                ch->dest_ad += dst_stride;
            }
        }
        ch->cs |= BCM2708_DMA_END;
        if (ch->ti & BCM2708_DMA_INT_EN) {
            ch->cs |= BCM2708_DMA_INT;
            s->int_status |= (1 << c);
            qemu_set_irq(ch->irq, 1);
        }

        /* Process next CB */
        ch->conblk_ad = ch->nextconbk;
    }
    ch->cs &= ~BCM2708_DMA_ACTIVE;
}

static uint64_t bcm2835_dma_read(BCM2835DmaState *s, hwaddr offset,
    unsigned size, int c)
{
    BCM2835DmaChan *ch = &s->chan[c];
    uint32_t res = 0;

    assert(size == 4);

    switch (offset) {
    case 0x0:
        res = ch->cs;
        break;
    case 0x4:
        res = ch->conblk_ad;
        break;
    case 0x8:
        res = ch->ti;
        break;
    case 0xc:
        res = ch->source_ad;
        break;
    case 0x10:
        res = ch->dest_ad;
        break;
    case 0x14:
        res = ch->txfr_len;
        break;
    case 0x18:
        res = ch->stride;
        break;
    case 0x1c:
        res = ch->nextconbk;
        break;
    case 0x20:
        res = ch->debug;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "bcm2835_dma_read: Bad offset %x\n", (int)offset);
        break;
    }
    return res;
}

static void bcm2835_dma_write(BCM2835DmaState *s, hwaddr offset,
    uint64_t value, unsigned size, int c)
{
    BCM2835DmaChan *ch = &s->chan[c];
    uint32_t oldcs = ch->cs;

    assert(size == 4);

    switch (offset) {
    case 0x0:
        if (value & BCM2708_DMA_RESET) {
            ch->cs |= BCM2708_DMA_RESET;
        }
        if (value & BCM2708_DMA_ABORT) {
            ch->cs |= BCM2708_DMA_ABORT;
        }
        if (value & BCM2708_DMA_END) {
            ch->cs &= ~BCM2708_DMA_END;
        }
        if (value & BCM2708_DMA_INT) {
            ch->cs &= ~BCM2708_DMA_INT;
            s->int_status &= ~(1 << c);
            qemu_set_irq(ch->irq, 0);
        }
        ch->cs &= ~0x30ff0001;
        ch->cs |= (value & 0x30ff0001);
        if (!(oldcs & BCM2708_DMA_ACTIVE) && (ch->cs & BCM2708_DMA_ACTIVE)) {
            bcm2835_dma_update(s, c);
        }
        break;
    case 0x4:
        ch->conblk_ad = value;
        break;
    case 0x20:
        ch->debug = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "bcm2835_dma_write: Bad offset %x\n", (int)offset);
        break;
    }
}

/* ==================================================================== */

static uint64_t bcm2835_dma0_14_read(void *opaque, hwaddr offset,
    unsigned size)
{
    BCM2835DmaState *s = (BCM2835DmaState *)opaque;
    if (offset == 0xfe0) {
        return s->int_status;
    }
    if (offset == 0xff0) {
        return s->enable;
    }
    return bcm2835_dma_read(s, (offset & 0xff),
        size, (offset >> 8) & 0xf);
}

static uint64_t bcm2835_dma15_read(void *opaque, hwaddr offset,
                           unsigned size)
{
    return bcm2835_dma_read((BCM2835DmaState *)opaque, (offset & 0xff),
        size, 15);
}

static void bcm2835_dma0_14_write(void *opaque, hwaddr offset,
    uint64_t value, unsigned size)
{
    BCM2835DmaState *s = (BCM2835DmaState *)opaque;
    if (offset == 0xfe0) {
        return;
    }
    if (offset == 0xff0) {
        s->enable = (value & 0xffff);
        return;
    }
    bcm2835_dma_write(s, (offset & 0xff),
        value, size, (offset >> 8) & 0xf);
}

static void bcm2835_dma15_write(void *opaque, hwaddr offset,
    uint64_t value, unsigned size)
{
    bcm2835_dma_write((BCM2835DmaState *)opaque, (offset & 0xff),
        value, size, 15);
}


static const MemoryRegionOps bcm2835_dma0_14_ops = {
    .read = bcm2835_dma0_14_read,
    .write = bcm2835_dma0_14_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps bcm2835_dma15_ops = {
    .read = bcm2835_dma15_read,
    .write = bcm2835_dma15_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_bcm2835_dma = {
    .name = TYPE_BCM2835_DMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_dma_init(Object *obj)
{
    BCM2835DmaState *s = BCM2835_DMA(obj);
    int n;

    memory_region_init_io(&s->iomem0_14, OBJECT(s), &bcm2835_dma0_14_ops, s,
        "bcm2835_dma0_14", 0xf00);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem0_14);

    memory_region_init_io(&s->iomem15, OBJECT(s), &bcm2835_dma15_ops, s,
        "bcm2835_dma15", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem15);

    for (n = 0; n < 16; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(s), &s->chan[n].irq);
    }
}

static void bcm2835_dma_realize(DeviceState *dev, Error **errp)
{
    int n;
    BCM2835DmaState *s = BCM2835_DMA(dev);
    Error *err = NULL;
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "dma_mr", &err);
    if (err || obj == NULL) {
        error_setg(errp, "bcm2835_property: required dma_mr link not found");
        return;
    }

    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, NULL);

    s->enable = 0xffff;
    s->int_status = 0;
    for (n = 0; n < 16; n++) {
        s->chan[n].cs = 0;
        s->chan[n].conblk_ad = 0;
    }
}

static void bcm2835_dma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2835_dma_realize;
    dc->vmsd = &vmstate_bcm2835_dma;
}

static TypeInfo bcm2835_dma_info = {
    .name          = TYPE_BCM2835_DMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835DmaState),
    .class_init    = bcm2835_dma_class_init,
    .instance_init = bcm2835_dma_init,
};

static void bcm2835_dma_register_types(void)
{
    type_register_static(&bcm2835_dma_info);
}

type_init(bcm2835_dma_register_types)
