// Shape a whole tag using the platform's font fallback, bidi and script shaping.
#include "../../src/runtime_clan_tags.h"
#include <pango/pangocairo.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace {
template<class T, auto release> using Owned = std::unique_ptr<T, decltype(release)>;
void put_tile(unsigned code, char*& out) {
    *out++ = char(0xe0 | code >> 12);
    *out++ = char(0x80 | ((code >> 6) & 63));
    *out++ = char(0x80 | (code & 63));
}
}

extern "C" int bbgpu_clan_raster(const char* tag, unsigned char* pixels) {
    if (!tag || !*tag || !g_utf8_validate(tag, -1, nullptr) || g_str_is_ascii(tag)) return 0;
    constexpr int width = CLAN_RASTER_WIDTH, height = CLAN_RASTER_HEIGHT;
    Owned<cairo_surface_t, cairo_surface_destroy> surface(cairo_image_surface_create_for_data(pixels, CAIRO_FORMAT_A8, width, height, width), cairo_surface_destroy);
    Owned<cairo_t, cairo_destroy> cr(cairo_create(surface.get()), cairo_destroy);
    Owned<PangoLayout, g_object_unref> layout(pango_cairo_create_layout(cr.get()), g_object_unref);
    Owned<PangoFontDescription, pango_font_description_free> font(pango_font_description_from_string("sans-serif semibold"), pango_font_description_free);
    pango_font_description_set_absolute_size(font.get(), 36 * PANGO_SCALE);
    pango_layout_set_font_description(layout.get(), font.get());
    pango_layout_set_text(layout.get(), tag, -1);
    if (pango_layout_get_unknown_glyphs_count(layout.get())) return 0;
    PangoRectangle ink;
    pango_layout_get_pixel_extents(layout.get(), &ink, nullptr);
    if (ink.width <= 0 || ink.height <= 0) return 0;
    const double scale = std::min({1.0, double(width-2) / ink.width, double(height-2) / ink.height});
    cairo_translate(cr.get(), 1 - scale*ink.x, (height - scale*ink.height)/2 - scale*ink.y);
    cairo_scale(cr.get(), scale, scale);
    cairo_set_source_rgba(cr.get(), 1, 1, 1, 1);
    pango_cairo_show_layout(cr.get(), layout.get());
    cairo_surface_flush(surface.get());
    if (cairo_status(cr.get()) != CAIRO_STATUS_SUCCESS) return 0;
    return std::min(width, 2 + int(std::ceil(scale*ink.width)));
}

extern "C" int bbgpu_clan_tiles(const char* tag, char out[CLAN_TILES_CAP]) {
    unsigned char pixels[CLAN_RASTER_WIDTH * CLAN_RASTER_HEIGHT]{};
    const int width = bbgpu_clan_raster(tag, pixels);
    const int stride = CLAN_RASTER_WIDTH, height = CLAN_RASTER_HEIGHT;
    char* cursor = out;
    for (int x = 0; x < width; x += 2) {
        for (int y = 0; y < height; y += 2) {
            unsigned tile = 0;
            for (int bit = 0; bit < 4; ++bit) {
                const unsigned alpha = pixels[(y + bit/2)*stride + x + bit%2];
                tile |= ((alpha + 42) / 85) << (bit*2);
            }
            if (tile) put_tile(CLAN_TILE_FIRST + (y/2)*CLAN_TILE_LEVELS + tile, cursor);
        }
        put_tile(CLAN_TILE_ADVANCE, cursor);
    }
    *cursor = 0;
    return cursor != out;
}
