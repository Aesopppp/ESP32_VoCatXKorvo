/* 调用生产解码器：真实资源全帧解码，以及损坏资源/越界拒绝回归。 */
#include "../main/ui_animation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (i * 8));
}
static void checksum(uint8_t *data, size_t size)
{
    uint32_t sum = 0;
    for (size_t i = 16; i < size; ++i) sum += data[i];
    put32(data + 8, sum);
}

int main(void)
{
    FILE *file = fopen("work/ui-checks/ui_assets.bin", "rb");
    assert(file);
    fseek(file, 0, SEEK_END); size_t size = (size_t)ftell(file); rewind(file);
    uint8_t *pack = malloc(size); assert(pack);
    assert(fread(pack, 1, size, file) == size); fclose(file);
    ui_animation_t assets[UI_ANIM_COUNT];
    assert(ui_animation_pack_open(pack, size, assets));
    const unsigned counts[] = {88, 14, 77, 50}; /* 排除 EAF 文件末尾的元信息帧。 */
    uint16_t pixels[20002];
    unsigned decoded = 0;
    for (unsigned a = 0; a < UI_ANIM_COUNT; ++a) {
        assert(assets[a].frame_count == counts[a]);
        size_t count = assets[a].width * assets[a].height;
        pixels[0] = pixels[count + 1] = 0xabcd;
        for (unsigned f = 0; f < assets[a].frame_count; ++f) {
            assert(ui_animation_decode(&assets[a], f, pixels + 1, count, 0));
            assert(pixels[0] == 0xabcd && pixels[count + 1] == 0xabcd);
            ++decoded;
        }
        assert(!ui_animation_decode(&assets[a], counts[a], pixels + 1, count, 0));
        assert(!ui_animation_decode(&assets[a], 0, pixels + 1, count - 1, 0));
        assert(pixels[0] == 0xabcd && pixels[count + 1] == 0xabcd);
    }
    /* 原版第一帧中心色 #e0f4ff，转换至 RGB565 应为 0xe7bf；黑背景不能带脏数据。 */
    assert(ui_animation_decode(&assets[0], 0, pixels, 20000, 0));
    assert(pixels[80 * 125 + 75] == 0xe7bf);
    assert(pixels[0] == 0);

    ui_animation_t invalid[UI_ANIM_COUNT];
    assert(!ui_animation_pack_open(NULL, size, invalid));
    assert(!ui_animation_pack_open(pack, 0, invalid));
    assert(!ui_animation_pack_open(pack, size - 1, invalid));
    uint8_t *bad = malloc(size); assert(bad);
    memcpy(bad, pack, size); bad[0] ^= 1;
    assert(!ui_animation_pack_open(bad, size, invalid));
    memcpy(bad, pack, size); put32(bad + 16, 0xfffffff0);
    assert(!ui_animation_pack_open(bad, size, invalid));
    memcpy(bad, pack, size); put32(bad + 20, 0xffffffff);
    assert(!ui_animation_pack_open(bad, size, invalid));

    const ui_animation_t *base = &assets[0];
    ui_animation_t anim;
    memcpy(bad, base->data, base->size); bad[base->size - 1] ^= 1;
    assert(!ui_animation_open(bad, base->size, &anim));
    memcpy(bad, base->data, base->size); put32(bad + 4, 0xffffffff);
    assert(!ui_animation_open(bad, base->size, &anim));
    /* 更新校验和后仍须拒绝损坏的表项、宽度、编码、RLE 输出长度。 */
    memcpy(bad, base->data, base->size); put32(bad + 20, 0xfffffff0); checksum(bad, base->size);
    assert(!ui_animation_open(bad, base->size, &anim));
    size_t frame = 16 + base->table_count * 8 + 2;
    memcpy(bad, base->data, base->size); bad[frame + 10] = 0xff; checksum(bad, base->size);
    assert(!ui_animation_open(bad, base->size, &anim));
    size_t block = frame + 18 + 5 * 4 + 1024;
    memcpy(bad, base->data, base->size); bad[block] = 5; checksum(bad, base->size);
    assert(!ui_animation_open(bad, base->size, &anim));
    memcpy(bad, base->data, base->size); bad[block + 1] = 0; checksum(bad, base->size);
    assert(!ui_animation_open(bad, base->size, &anim));
    memcpy(bad, base->data, base->size); bad[block + 1] = bad[block + 1] == 255 ? 254 : 255;
    checksum(bad, base->size);
    assert(!ui_animation_open(bad, base->size, &anim));
    free(bad); free(pack);
    printf("EAF checks passed: %u real frames, color/metadata, bounded output, corrupt/truncated assets rejected\n", decoded);
    return 0;
}
