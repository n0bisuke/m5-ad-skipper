#include "gifenc.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static void ge_write_u8(int fd, uint8_t v) {
    (void)!write(fd, &v, 1);
}

static void ge_write_u16(int fd, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };
    (void)!write(fd, b, 2);
}

static void ge_write_bytes(int fd, const void *buf, size_t n) {
    (void)!write(fd, buf, n);
}

typedef struct {
    int fd;
    uint8_t block[255];
    int n;
} ge_blockw_t;

static void bw_init(ge_blockw_t *bw, int fd) {
    bw->fd = fd;
    bw->n = 0;
}

static void bw_putc(ge_blockw_t *bw, uint8_t c) {
    bw->block[bw->n++] = c;
    if (bw->n == 255) {
        ge_write_u8(bw->fd, (uint8_t)bw->n);
        ge_write_bytes(bw->fd, bw->block, bw->n);
        bw->n = 0;
    }
}

static void bw_flush(ge_blockw_t *bw) {
    if (bw->n) {
        ge_write_u8(bw->fd, (uint8_t)bw->n);
        ge_write_bytes(bw->fd, bw->block, bw->n);
        bw->n = 0;
    }
    ge_write_u8(bw->fd, 0);
}

typedef struct {
    ge_blockw_t *bw;
    uint32_t bits;
    int nbits;
} ge_bitstr_t;

static void bs_init(ge_bitstr_t *bs, ge_blockw_t *bw) {
    bs->bw = bw;
    bs->bits = 0;
    bs->nbits = 0;
}

static void bs_put(ge_bitstr_t *bs, uint32_t code, int width) {
    bs->bits |= (code & ((1u << width) - 1u)) << bs->nbits;
    bs->nbits += width;
    while (bs->nbits >= 8) {
        bw_putc(bs->bw, (uint8_t)(bs->bits & 0xFFu));
        bs->bits >>= 8;
        bs->nbits -= 8;
    }
}

static void bs_flush(ge_bitstr_t *bs) {
    if (bs->nbits) {
        bw_putc(bs->bw, (uint8_t)(bs->bits & 0xFFu));
        bs->bits = 0;
        bs->nbits = 0;
    }
}

ge_GIF *ge_new_gif(
    const char *fname, uint16_t width, uint16_t height,
    uint8_t *palette, int depth, int bgindex, int loop
) {
    if (depth < 2) depth = 2;
    if (depth > 8) depth = 8;

    ge_GIF *gif = (ge_GIF *)calloc(1, sizeof(ge_GIF));
    if (!gif) return NULL;
    gif->w = width;
    gif->h = height;
    gif->depth = depth;
    gif->bgindex = bgindex;
    gif->nframes = 0;
    gif->fd = open(fname, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (gif->fd < 0) {
        free(gif);
        return NULL;
    }
    size_t npix = (size_t)width * (size_t)height;
    gif->frame = (uint8_t *)malloc(npix);
    gif->back  = (uint8_t *)malloc(npix);
    if (!gif->frame || !gif->back) {
        ge_close_gif(gif);
        return NULL;
    }

    ge_write_bytes(gif->fd, "GIF89a", 6);

    ge_write_u16(gif->fd, width);
    ge_write_u16(gif->fd, height);
    uint8_t gct_flag = 1u << 7;
    uint8_t color_res = (uint8_t)((depth - 1) & 0x07) << 4;
    uint8_t sort_flag = 0u << 3;
    uint8_t gct_size  = (uint8_t)((depth - 1) & 0x07);
    ge_write_u8(gif->fd, (uint8_t)(gct_flag | color_res | sort_flag | gct_size));
    ge_write_u8(gif->fd, (uint8_t)bgindex);
    ge_write_u8(gif->fd, 0);

    size_t gct_entries = 1u << depth;
    ge_write_bytes(gif->fd, palette, gct_entries * 3u);

    if (loop >= 0) {
        ge_write_u8(gif->fd, 0x21);
        ge_write_u8(gif->fd, 0xFF);
        ge_write_u8(gif->fd, 11);
        ge_write_bytes(gif->fd, "NETSCAPE2.0", 11);
        ge_write_u8(gif->fd, 3);
        ge_write_u8(gif->fd, 1);
        uint16_t loops = (loop == 0 ? 0 : (uint16_t)loop);
        ge_write_u16(gif->fd, loops);
        ge_write_u8(gif->fd, 0);
    }

    return gif;
}

void ge_add_frame(ge_GIF *gif, uint16_t delay) {
    if (!gif) return;

    ge_write_u8(gif->fd, 0x21);
    ge_write_u8(gif->fd, 0xF9);
    ge_write_u8(gif->fd, 0x04);
    ge_write_u8(gif->fd, 0x00);
    ge_write_u16(gif->fd, delay);
    ge_write_u8(gif->fd, 0x00);
    ge_write_u8(gif->fd, 0x00);

    ge_write_u8(gif->fd, 0x2C);
    ge_write_u16(gif->fd, 0);
    ge_write_u16(gif->fd, 0);
    ge_write_u16(gif->fd, gif->w);
    ge_write_u16(gif->fd, gif->h);
    ge_write_u8(gif->fd, 0x00);

    int lzw_min = gif->depth;
    if (lzw_min < 2) lzw_min = 2;
    if (lzw_min > 8) lzw_min = 8;
    ge_write_u8(gif->fd, (uint8_t)lzw_min);

    uint32_t clear = (uint32_t)(1u << lzw_min);
    uint32_t stop  = clear + 1u;
    int code_size  = lzw_min + 1;

    ge_blockw_t bw; bw_init(&bw, gif->fd);
    ge_bitstr_t bs; bs_init(&bs, &bw);

    bs_put(&bs, clear, code_size);
    size_t npix = (size_t)gif->w * (size_t)gif->h;
    int codes_since_clear = 0;
    const int clear_interval = 240;  // keep dictionary small to avoid code-size growth
    for (size_t i = 0; i < npix; ++i) {
        bs_put(&bs, gif->frame[i], code_size);
        codes_since_clear++;
        if (codes_since_clear >= clear_interval) {
            bs_put(&bs, clear, code_size);
            codes_since_clear = 0;
        }
    }
    bs_put(&bs, stop, code_size);
    bs_flush(&bs);
    bw_flush(&bw);

    gif->nframes++;
}

void ge_close_gif(ge_GIF* gif) {
    if (!gif) return;
    ge_write_u8(gif->fd, 0x3B);
    if (gif->fd >= 0) {
        close(gif->fd);
        gif->fd = -1;
    }
    if (gif->frame) free(gif->frame);
    if (gif->back)  free(gif->back);
    free(gif);
}
