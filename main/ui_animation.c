/*
 * SPDX-License-Identifier: Apache-2.0
 * EAF 格式/调色板规则参考 Espressif esp_emote_gfx 3.0.5 的 gfx_eaf_dec.c。
 * 本工程实现固定资源所需的无分配子集，新增长度、帧索引及输出容量校验。
 * 来源和许可证见 assets/face/README.md；不依赖原组件的绘图任务。
 */
#include "ui_animation.h"
#include <string.h>

#define EAF_HEADER_SIZE 16u
#define EAF_MAX_FRAMES 256u
#define EAF_MAX_WIDTH 125u
#define EAF_MAX_HEIGHT 160u

/* Flash 中的表项可能未对齐，禁止把字节指针强转为 uint32_t *。 */
static uint16_t read16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool range_valid(size_t offset, size_t length, size_t size)
{
    return offset <= size && length <= size - offset;
}

static bool frame_view(const ui_animation_t *animation, unsigned index,
                       const uint8_t **data, size_t *size)
{
    if (!animation || !animation->data || index >= animation->table_count) return false;
    size_t base = EAF_HEADER_SIZE + (size_t)animation->table_count * 8;
    if (base > animation->size) return false;
    const uint8_t *entry = animation->data + EAF_HEADER_SIZE + index * 8;
    size_t length = read32(entry), offset = read32(entry + 4);
    if (length < 4 || !range_valid(offset, length, animation->size - base)) return false;
    const uint8_t *frame = animation->data + base + offset;
    if (read16(frame) != 0x5a5a) return false;
    *data = frame + 2;
    *size = length - 2;
    return true;
}

/* pixels=NULL 时执行同样的流校验，用于启动验证所有帧而不申请临时图像。 */
static bool decode_frame(const ui_animation_t *animation, unsigned index,
                         uint16_t *pixels, size_t capacity, uint16_t background)
{
    const uint8_t *frame;
    size_t size;
    if (!frame_view(animation, index, &frame, &size) || size < 18 ||
        memcmp(frame, "_S", 2) || frame[9] != 8) return false;
    unsigned width = read16(frame + 10), height = read16(frame + 12);
    unsigned blocks = read16(frame + 14), block_height = read16(frame + 16);
    if (width != animation->width || height != animation->height ||
        !block_height || block_height > EAF_MAX_HEIGHT ||
        blocks != (height + block_height - 1) / block_height) return false;
    size_t pos = 18 + (size_t)blocks * 4;
    if (!range_valid(pos, 1024, size)) return false;
    const uint8_t *palette = frame + pos;
    pos += 1024;
    if (pixels && capacity < (size_t)width * height) return false;
    uint16_t colors[256];
    if (pixels) {
        for (unsigned i = 0; i < 256; ++i) {
            const uint8_t *c = palette + i * 4; /* BGRA，四字节全零表示透明。 */
            colors[i] = !(c[0] | c[1] | c[2] | c[3]) ? background :
                (uint16_t)(((c[2] & 0xf8) << 8) | ((c[1] & 0xfc) << 3) | (c[0] >> 3));
        }
    }
    for (unsigned block = 0; block < blocks; ++block) {
        size_t length = read32(frame + 18 + block * 4);
        if (length < 3 || !range_valid(pos, length, size) ||
            frame[pos] != 0 || (length - 1) % 2) return false;
        unsigned rows = height - block * block_height;
        if (rows > block_height) rows = block_height;
        size_t expected = (size_t)width * rows, written = 0;
        size_t end = pos + length;
        for (++pos; pos < end; pos += 2) {
            unsigned count = frame[pos], color = frame[pos + 1];
            if (!count || count > expected - written) return false;
            if (pixels) {
                size_t start = (size_t)block * block_height * width + written;
                for (unsigned j = 0; j < count; ++j) pixels[start + j] = colors[color];
            }
            written += count;
        }
        if (written != expected) return false;
    }
    return pos == size;
}

bool ui_animation_open(const void *data, size_t size, ui_animation_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!data || size < EAF_HEADER_SIZE) return false;
    const uint8_t *bytes = data;
    if (memcmp(bytes, "\x89" "EAF", 4)) return false;
    uint32_t count = read32(bytes + 4), payload = read32(bytes + 12);
    if (!count || count > EAF_MAX_FRAMES || payload != size - EAF_HEADER_SIZE || count * 8 > payload)
        return false;
    uint32_t checksum = 0;
    for (size_t i = EAF_HEADER_SIZE; i < size; ++i) checksum += bytes[i];
    if (checksum != read32(bytes + 8)) return false;
    ui_animation_t candidate = {.data = bytes, .size = size, .table_count = (uint16_t)count};
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *frame;
        size_t length;
        if (!frame_view(&candidate, i, &frame, &length)) return false;
        if (!memcmp(frame, "_C", 2)) {
            if (i != count - 1) return false;
            continue;
        }
        if (length < 18) return false;
        if (!i) {
            candidate.width = read16(frame + 10);
            candidate.height = read16(frame + 12);
            if (!candidate.width || candidate.width > EAF_MAX_WIDTH ||
                !candidate.height || candidate.height > EAF_MAX_HEIGHT) return false;
        }
        if (!decode_frame(&candidate, i, NULL, 0, 0)) return false;
        ++candidate.frame_count;
    }
    if (!candidate.frame_count) return false;
    *out = candidate;
    return true;
}

bool ui_animation_pack_open(const void *data, size_t size, ui_animation_t out[UI_ANIM_COUNT])
{
    if (!out) return false;
    memset(out, 0, sizeof(*out) * UI_ANIM_COUNT);
    if (!data || size < 16 + UI_ANIM_COUNT * 8) return false;
    const uint8_t *bytes = data;
    if (memcmp(bytes, "VFACE01\0", 8) || read32(bytes + 12) != UI_ANIM_COUNT) return false;
    size_t total = read32(bytes + 8), end = 16 + UI_ANIM_COUNT * 8;
    if (total > size || total < end) return false;
    ui_animation_t candidates[UI_ANIM_COUNT];
    for (unsigned i = 0; i < UI_ANIM_COUNT; ++i) {
        size_t offset = read32(bytes + 16 + i * 8), length = read32(bytes + 20 + i * 8);
        if (offset != end || !range_valid(offset, length, total) ||
            !ui_animation_open(bytes + offset, length, &candidates[i])) return false;
        unsigned width = i == UI_ANIM_LISTEN ? 100 : 125;
        unsigned height = i == UI_ANIM_LISTEN ? 46 : 160;
        if (candidates[i].width != width || candidates[i].height != height) return false;
        end = offset + length;
    }
    if (end != total) return false;
    memcpy(out, candidates, sizeof(candidates));
    return true;
}

bool ui_animation_decode(const ui_animation_t *animation, unsigned frame,
                         uint16_t *pixels, size_t capacity, uint16_t background)
{
    if (!animation || !pixels || frame >= animation->frame_count) return false;
    return decode_frame(animation, frame, pixels, capacity, background);
}
