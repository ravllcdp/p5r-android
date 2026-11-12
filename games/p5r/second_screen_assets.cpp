// Extracts companion sprites from the game's own RomFS at first run, so SecondScreenService.java
// can load them without a separate asset bundle. Runs once on a background thread after game init.
//
// Source SPD files (Atlas sprite-package format, embedded DDS textures):
//   EN/BATTLE/GUI/P5_BATTLE_PARTYPANEL.SPD  — party portrait atlas (frames 61-68, 177)
//   EN/INIT/P5CAMP_00SPD.SPD               — status icons (frames 261=HP, 262=SP)
//
// All companion .rgba outputs go into {out_dir}/companion/.
#if defined(__ANDROID__)
#include "second_screen_assets.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>

#include <nx/runtime.h>          // nx::log
#include "game_image.h"          // nx::game_image()
#include "platform.h"            // nx::plat::start_thread

namespace p5r::companion {

namespace {

// ---------------------------------------------------------------------------
// SPD binary structures (Atlus sprite-package format v2, used in P5R)
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct SpdHeader {                  // 0x20 bytes
    uint32_t magic;                 // 0x30525053 = "SPR0"
    uint32_t unk04;
    uint32_t file_size;
    uint32_t unk0c;
    uint32_t unk10;
    uint16_t texture_count;
    uint16_t sprite_count;
    uint32_t texture_start;         // = 0x20
    uint32_t sprite_start;
};

struct SpdTextureEntry {            // 0x30 bytes
    uint32_t id;
    uint32_t unk04;
    uint32_t data_offset;           // from start of SPD file
    uint32_t data_size;
    uint32_t width;
    uint32_t height;
    uint32_t unk18;
    uint32_t unk1c;
    char     desc[16];
};

struct SpdSpriteEntry {             // 0xA0 bytes
    uint32_t id;                    // 0x00
    uint32_t tex_id;                // 0x04
    uint32_t unk08[6];              // 0x08
    uint16_t x;                     // 0x20
    uint16_t y;                     // 0x22
    uint16_t w;                     // 0x24
    uint16_t h;                     // 0x26
    uint32_t unk28[2];              // 0x28
    float    scale_x;               // 0x30
    float    scale_y;               // 0x34
    uint32_t unk38[14];             // 0x38
    char     name[48];              // 0x70
};
static_assert(sizeof(SpdHeader)       == 0x20);
static_assert(sizeof(SpdTextureEntry) == 0x30);
static_assert(sizeof(SpdSpriteEntry)  == 0xA0);
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Minimal DDS / BC1 / BC3 decoder
// ---------------------------------------------------------------------------

static const uint32_t kDdsMagic   = 0x20534444; // "DDS "
static const uint32_t kFourCC_DXT1 = 0x31545844; // "DXT1"
static const uint32_t kFourCC_DXT5 = 0x35545844; // "DXT5"
static const uint32_t kDdsHeaderSize = 124;

// bc1_colors: decode one BC1 4×4 block into out[16][4] (RGBA, alpha=255 always here)
static void bc1_colors(const uint8_t* b, uint8_t out[16][4]) {
    uint16_t c0w = uint16_t(b[0]) | (uint16_t(b[1]) << 8);
    uint16_t c1w = uint16_t(b[2]) | (uint16_t(b[3]) << 8);
    uint8_t cr[4], cg[4], cb[4];
    cr[0] = uint8_t((c0w >> 11) * 255 / 31);
    cg[0] = uint8_t(((c0w >> 5) & 0x3F) * 255 / 63);
    cb[0] = uint8_t((c0w & 0x1F) * 255 / 31);
    cr[1] = uint8_t((c1w >> 11) * 255 / 31);
    cg[1] = uint8_t(((c1w >> 5) & 0x3F) * 255 / 63);
    cb[1] = uint8_t((c1w & 0x1F) * 255 / 31);
    // BC3 color block always uses 4-color mode (c0 and c1 aren't compared)
    cr[2] = uint8_t((2 * cr[0] + cr[1]) / 3);
    cg[2] = uint8_t((2 * cg[0] + cg[1]) / 3);
    cb[2] = uint8_t((2 * cb[0] + cb[1]) / 3);
    cr[3] = uint8_t((cr[0] + 2 * cr[1]) / 3);
    cg[3] = uint8_t((cg[0] + 2 * cg[1]) / 3);
    cb[3] = uint8_t((cb[0] + 2 * cb[1]) / 3);
    uint32_t indices = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) {
        uint32_t idx = (indices >> (2 * i)) & 3;
        out[i][0] = cr[idx]; out[i][1] = cg[idx]; out[i][2] = cb[idx]; out[i][3] = 255;
    }
}

// bc4_alpha: decode one BC4 alpha block (8 bytes) into out[16]
static void bc4_alpha(const uint8_t* b, uint8_t out[16]) {
    int v[8];
    v[0] = b[0]; v[1] = b[1];
    if (v[0] > v[1]) {
        for (int k = 1; k < 7; k++) v[k + 1] = (int)((7 - k) * v[0] + k * v[1]) / 7;
    } else {
        for (int k = 1; k < 5; k++) v[k + 1] = (int)((5 - k) * v[0] + k * v[1]) / 5;
        v[6] = 0; v[7] = 255;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)v[(bits >> (3 * i)) & 7];
}

// decode_bc1: compressed → RGBA8, out must be width*height*4 bytes
static bool decode_bc1(const uint8_t* src, uint32_t w, uint32_t h, uint8_t* out) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = src + (by * bw + bx) * 8;
            uint8_t px[16][4];
            bc1_colors(b, px);
            for (uint32_t y = 0; y < 4; y++) {
                uint32_t py = by * 4 + y; if (py >= h) break;
                for (uint32_t x = 0; x < 4; x++) {
                    uint32_t px_ = bx * 4 + x; if (px_ >= w) break;
                    memcpy(out + (py * w + px_) * 4, px[y * 4 + x], 4);
                }
            }
        }
    return true;
}

// decode_bc3: compressed → RGBA8
static bool decode_bc3(const uint8_t* src, uint32_t w, uint32_t h, uint8_t* out) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = src + (by * bw + bx) * 16;
            uint8_t a[16]; bc4_alpha(b, a);
            uint8_t px[16][4]; bc1_colors(b + 8, px);
            for (int i = 0; i < 16; i++) px[i][3] = a[i];
            for (uint32_t y = 0; y < 4; y++) {
                uint32_t py = by * 4 + y; if (py >= h) break;
                for (uint32_t x = 0; x < 4; x++) {
                    uint32_t px_ = bx * 4 + x; if (px_ >= w) break;
                    memcpy(out + (py * w + px_) * 4, px[y * 4 + x], 4);
                }
            }
        }
    return true;
}

// Decode a DDS buffer (starting at "DDS ") to RGBA8.
// Returns false on unknown format; writes w, h and fills out_rgba.
static bool decode_dds(const uint8_t* dds, size_t size,
                       uint32_t& out_w, uint32_t& out_h,
                       std::vector<uint8_t>& out_rgba) {
    if (size < 4 + kDdsHeaderSize) return false;
    uint32_t magic; memcpy(&magic, dds, 4);
    if (magic != kDdsMagic) return false;

    uint32_t h, w;
    memcpy(&h, dds + 12, 4);
    memcpy(&w, dds + 16, 4);
    uint32_t fourcc; memcpy(&fourcc, dds + 84, 4); // ddspf.dwFourCC at byte 128-44=84

    const uint8_t* pixels = dds + 4 + kDdsHeaderSize; // 128 bytes in
    size_t data_size = size - (4 + kDdsHeaderSize);

    out_w = w; out_h = h;
    out_rgba.resize((size_t)w * h * 4, 0);

    if (fourcc == kFourCC_DXT1) {
        size_t need = ((w + 3) / 4) * ((h + 3) / 4) * 8;
        if (data_size < need) return false;
        return decode_bc1(pixels, w, h, out_rgba.data());
    } else if (fourcc == kFourCC_DXT5) {
        size_t need = ((w + 3) / 4) * ((h + 3) / 4) * 16;
        if (data_size < need) return false;
        return decode_bc3(pixels, w, h, out_rgba.data());
    }
    return false; // unsupported format
}

// ---------------------------------------------------------------------------
// File I/O helpers
// ---------------------------------------------------------------------------

static bool mkdir_p(const std::string& path) {
    // Create directory and all parents.
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            std::string sub = path.substr(0, i);
            if (::mkdir(sub.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

static bool write_rgba(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgba) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(&w, 4, 1, f);
    fwrite(&h, 4, 1, f);
    fwrite(rgba, 1, (size_t)w * h * 4, f);
    fclose(f);
    return true;
}

// Crop a sub-rectangle from src (src_w × src_h RGBA8) into dst.
static std::vector<uint8_t> crop_rgba(const uint8_t* src, uint32_t src_w, uint32_t src_h,
                                       uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    std::vector<uint8_t> out((size_t)w * h * 4, 0);
    for (uint32_t row = 0; row < h; row++) {
        uint32_t sy = y + row;
        if (sy >= src_h) break;
        for (uint32_t col = 0; col < w; col++) {
            uint32_t sx = x + col;
            if (sx >= src_w) break;
            memcpy(out.data() + (row * w + col) * 4,
                   src + (sy * src_w + sx) * 4, 4);
        }
    }
    return out;
}

// Multiply every pixel's RGB by the given 8-bit tint (alpha unchanged).
static void tint_rgba(uint8_t* rgba, size_t pixel_count, uint8_t tr, uint8_t tg, uint8_t tb) {
    for (size_t i = 0; i < pixel_count; i++) {
        rgba[i * 4 + 0] = uint8_t((rgba[i * 4 + 0] * tr) / 255);
        rgba[i * 4 + 1] = uint8_t((rgba[i * 4 + 1] * tg) / 255);
        rgba[i * 4 + 2] = uint8_t((rgba[i * 4 + 2] * tb) / 255);
    }
}

// ---------------------------------------------------------------------------
// SPD sprite extraction helpers
// ---------------------------------------------------------------------------

struct SpriteInfo { uint16_t x, y, w, h; uint32_t tex_id; };

// Read the whole SPD file from romfs into a buffer.
static bool read_romfs_file(const std::string& path, std::vector<uint8_t>& out) {
    const nxi::GameImage* img = nx::game_image();
    if (!img) return false;
    const nxi::GameImage::Entry* entry = img->find(path);
    if (!entry || entry->dir || entry->size == 0) return false;
    out.resize(entry->size);
    uint64_t n = img->read(*entry, 0, out.data(), entry->size);
    return n == entry->size;
}

// Return the first existing path in a list of candidates.
static std::string find_romfs_path(const std::vector<std::string>& candidates) {
    const nxi::GameImage* img = nx::game_image();
    if (!img) return {};
    for (const auto& p : candidates)
        if (img->find(p) != nullptr) return p;
    return {};
}

// Parse sprites from SPD buffer; return list indexed by sprite-order (0-based).
static bool parse_spd_sprites(const std::vector<uint8_t>& spd,
                               std::vector<SpriteInfo>& sprites,
                               std::vector<SpdTextureEntry>& textures) {
    if (spd.size() < sizeof(SpdHeader)) return false;
    SpdHeader hdr; memcpy(&hdr, spd.data(), sizeof(hdr));
    if (hdr.magic != 0x30525053u) return false; // "SPR0"
    if (hdr.texture_start + (uint64_t)hdr.texture_count * sizeof(SpdTextureEntry) > spd.size()) return false;
    if (hdr.sprite_start  + (uint64_t)hdr.sprite_count  * sizeof(SpdSpriteEntry)  > spd.size()) return false;

    textures.resize(hdr.texture_count);
    for (uint16_t i = 0; i < hdr.texture_count; i++)
        memcpy(&textures[i], spd.data() + hdr.texture_start + i * sizeof(SpdTextureEntry), sizeof(SpdTextureEntry));

    sprites.resize(hdr.sprite_count);
    for (uint16_t i = 0; i < hdr.sprite_count; i++) {
        SpdSpriteEntry e;
        memcpy(&e, spd.data() + hdr.sprite_start + i * sizeof(SpdSpriteEntry), sizeof(SpdSpriteEntry));
        sprites[i] = {e.x, e.y, e.w, e.h, e.tex_id};
    }
    return true;
}

// Decode the DDS texture from an SPD buffer given a texture entry.
static bool decode_spd_texture(const std::vector<uint8_t>& spd, const SpdTextureEntry& tex,
                                uint32_t& tw, uint32_t& th, std::vector<uint8_t>& rgba) {
    if (tex.data_offset + tex.data_size > spd.size()) return false;
    return decode_dds(spd.data() + tex.data_offset, tex.data_size, tw, th, rgba);
}

// ---------------------------------------------------------------------------
// Main extraction logic
// ---------------------------------------------------------------------------

static std::atomic<bool> g_done{false};

static void extract_thread(std::string out_dir) {
    // Ensure output directories exist.
    mkdir_p(out_dir + "/companion/portraits");
    mkdir_p(out_dir + "/companion/icons");

    // --- Portraits from P5_BATTLE_PARTYPANEL.SPD ---
    // Frame indices in the SPD (0-based):
    //   61=Joker, 62=Ryuji, 63=Morgana, 64=Ann, 65=Yusuke,
    //   66=Makoto, 67=Futaba, 68=Haru, 177=Lavenza (Kasumi=same as Haru placeholder)
    // Member IDs (kUnitMemberId): 0=Joker..7=Haru, 8=Akechi, 9=Kasumi, 10=Lavenza
    // We map: ID 0..7 → frame 61..68, ID 10 → frame 177; ID 8,9 share fallback.
    static const struct { int member_id; int frame; } kPortraits[] = {
        {0,  61}, {1,  62}, {2,  63}, {3,  64},
        {4,  65}, {5,  66}, {6,  67}, {7,  68},
        {10, 177},
    };

    // Try multiple possible romfs paths for the portrait SPD.
    std::string portrait_spd_path = find_romfs_path({
        "EN/BATTLE/GUI/P5_BATTLE_PARTYPANEL.SPD",
        "en/BATTLE/GUI/P5_BATTLE_PARTYPANEL.SPD",
        "BATTLE/GUI/P5_BATTLE_PARTYPANEL.SPD",
        "EN/BATTLE/GUI/p5_battle_partypanel.spd",
    });

    if (portrait_spd_path.empty()) {
        nx::log("companion: P5_BATTLE_PARTYPANEL.SPD not found in romfs");
    } else {
        std::vector<uint8_t> spd;
        if (!read_romfs_file(portrait_spd_path, spd)) {
            nx::log("companion: failed to read %s", portrait_spd_path.c_str());
        } else {
            std::vector<SpriteInfo> sprites;
            std::vector<SpdTextureEntry> textures;
            if (!parse_spd_sprites(spd, sprites, textures)) {
                nx::log("companion: failed to parse portrait SPD");
            } else {
                // Cache decoded textures so we don't decode the same one twice.
                std::vector<std::vector<uint8_t>> decoded(textures.size());
                std::vector<uint32_t> tex_w(textures.size(), 0), tex_h(textures.size(), 0);

                for (const auto& pm : kPortraits) {
                    int frame = pm.frame;
                    if (frame < 0 || (size_t)frame >= sprites.size()) continue;
                    const SpriteInfo& s = sprites[frame];
                    uint32_t ti = s.tex_id;
                    if (ti >= textures.size()) continue;

                    // Decode texture if not already done.
                    if (decoded[ti].empty()) {
                        if (!decode_spd_texture(spd, textures[ti], tex_w[ti], tex_h[ti], decoded[ti])) {
                            nx::log("companion: failed to decode portrait texture %u", ti);
                            continue;
                        }
                    }

                    std::vector<uint8_t> crop = crop_rgba(decoded[ti].data(), tex_w[ti], tex_h[ti],
                                                           s.x, s.y, s.w, s.h);
                    std::string path = out_dir + "/companion/portraits/p" + std::to_string(pm.member_id) + ".rgba";
                    if (!write_rgba(path, s.w, s.h, crop.data()))
                        nx::log("companion: failed to write %s", path.c_str());
                }
            }
        }
    }

    // --- Icons from P5CAMP_00SPD.SPD ---
    // Frame 261 = HP icon (tint #66FDDC), frame 262 = SP icon (tint #FF64E8)
    static const struct { const char* name; int frame; uint8_t tr, tg, tb; } kIcons[] = {
        {"hp", 261, 0x66, 0xFD, 0xDC},
        {"sp", 262, 0xFF, 0x64, 0xE8},
    };

    std::string icon_spd_path = find_romfs_path({
        "EN/INIT/P5CAMP_00SPD.SPD",
        "en/INIT/P5CAMP_00SPD.SPD",
        "INIT/P5CAMP_00SPD.SPD",
        "EN/INIT/p5camp_00spd.spd",
    });

    if (icon_spd_path.empty()) {
        nx::log("companion: P5CAMP_00SPD.SPD not found in romfs");
    } else {
        std::vector<uint8_t> spd;
        if (!read_romfs_file(icon_spd_path, spd)) {
            nx::log("companion: failed to read %s", icon_spd_path.c_str());
        } else {
            std::vector<SpriteInfo> sprites;
            std::vector<SpdTextureEntry> textures;
            if (!parse_spd_sprites(spd, sprites, textures)) {
                nx::log("companion: failed to parse icon SPD");
            } else {
                std::vector<std::vector<uint8_t>> decoded(textures.size());
                std::vector<uint32_t> tex_w(textures.size(), 0), tex_h(textures.size(), 0);

                for (const auto& ic : kIcons) {
                    int frame = ic.frame;
                    if (frame < 0 || (size_t)frame >= sprites.size()) continue;
                    const SpriteInfo& s = sprites[frame];
                    uint32_t ti = s.tex_id;
                    if (ti >= textures.size()) continue;

                    if (decoded[ti].empty()) {
                        if (!decode_spd_texture(spd, textures[ti], tex_w[ti], tex_h[ti], decoded[ti])) {
                            nx::log("companion: failed to decode icon texture %u", ti);
                            continue;
                        }
                    }

                    std::vector<uint8_t> crop = crop_rgba(decoded[ti].data(), tex_w[ti], tex_h[ti],
                                                           s.x, s.y, s.w, s.h);
                    tint_rgba(crop.data(), (size_t)s.w * s.h, ic.tr, ic.tg, ic.tb);
                    std::string path = out_dir + "/companion/icons/";
                    path += ic.name; path += ".rgba";
                    if (!write_rgba(path, s.w, s.h, crop.data()))
                        nx::log("companion: failed to write %s", path.c_str());
                }
            }
        }
    }

    nx::log("companion: asset extraction complete");
    g_done.store(true, std::memory_order_release);
}

}  // namespace

void start_asset_extraction(const std::string& out_dir) {
    static std::once_flag once;
    std::call_once(once, [&] {
        std::thread([out_dir] { extract_thread(out_dir); }).detach();
    });
}

bool assets_done() { return g_done.load(std::memory_order_acquire); }

}  // namespace p5r::companion
#endif  // __ANDROID__
