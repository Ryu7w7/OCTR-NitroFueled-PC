#include "clan_labels.h"
#include "../../src/runtime_clan_tags.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <list>
#include <string>

namespace ClanLabels {
namespace {
struct Picture {
    std::string tag;
    ImFontAtlasRectId id;
    int width;
};
std::list<Picture> cache; // accessed only under the overlay's ImGui lock

const Picture& picture(std::string_view tag) {
    auto hit = std::find_if(cache.begin(), cache.end(), [tag](const Picture& p) { return p.tag == tag; });
    if (hit != cache.end()) {
        cache.splice(cache.begin(), cache, hit);
        return cache.front();
    }
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (cache.size() == 128) {
        if (cache.back().id != ImFontAtlasRectId_Invalid) atlas->RemoveCustomRect(cache.back().id);
        cache.pop_back();
    }
    Picture& p = cache.emplace_front(Picture{std::string(tag), ImFontAtlasRectId_Invalid, 0});
    std::array<unsigned char, CLAN_RASTER_WIDTH * CLAN_RASTER_HEIGHT> alpha{};
    p.width = bbgpu_clan_raster(p.tag.c_str(), alpha.data());
    if (!p.width) return p;
    ImFontAtlasRect r;
    p.id = atlas->AddCustomRect(p.width, CLAN_RASTER_HEIGHT, &r);
    if (p.id == ImFontAtlasRectId_Invalid) return p;
    ImTextureData* tex = atlas->TexData;
    for (int y = 0; y < r.h; ++y) {
        const auto* row = alpha.data() + y * CLAN_RASTER_WIDTH;
        if (tex->Format == ImTextureFormat_Alpha8) {
            std::copy_n(row, r.w, static_cast<unsigned char*>(tex->GetPixelsAt(r.x, r.y + y)));
        } else {
            auto* dst = static_cast<ImU32*>(tex->GetPixelsAt(r.x, r.y + y));
            for (int x = 0; x < r.w; ++x) dst[x] = IM_COL32(255,255,255,row[x]);
        }
    }
    return p;
}

struct Parts {
    std::string_view prefix, suffix;
    const Picture* picture;
};
Parts split(std::string_view s) {
    const auto left = s.find('[');
    if (left == s.npos) return {s, {}, nullptr};
    const auto end = s.find(']', left+1);
    if (end == s.npos) return {s, {}, nullptr};
    const auto tag = s.substr(left+1, end-left-1);
    if (tag.empty() || tag.size() >= CLAN_TAG_CAP ||
        std::all_of(tag.begin(), tag.end(), [](unsigned char c) { return c < 128; })) return {s, {}, nullptr};
    const auto& p = picture(tag);
    if (p.id == ImFontAtlasRectId_Invalid) return {s, {}, nullptr};
    return {s.substr(0,left+1), s.substr(end), &p};
}
float width(ImFont* font, float size, std::string_view text) {
    return text.empty() ? 0 : font->CalcTextSizeA(size, FLT_MAX, 0, text.data(), text.data()+text.size()).x;
}
float draw(ImDrawList* list, ImFont* font, float size, ImVec2 pos, ImU32 color, const Parts& parts) {
    const float first = width(font,size,parts.prefix);
    if (!parts.prefix.empty()) list->AddText(font,size,pos,color,parts.prefix.data(),parts.prefix.data()+parts.prefix.size());
    if (!parts.picture) return first;
    const float image = parts.picture->width * size / CLAN_RASTER_HEIGHT;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    ImFontAtlasRect r;
    pos.x += first;
    if (atlas->GetCustomRect(parts.picture->id, &r))
        list->AddImage(atlas->TexRef,pos,ImVec2(pos.x+image,pos.y+size),r.uv0,r.uv1,color);
    pos.x += image;
    list->AddText(font,size,pos,color,parts.suffix.data(),parts.suffix.data()+parts.suffix.size());
    return first + image + width(font,size,parts.suffix);
}
}

// A player swatch paints exactly the account name. Prefix icons, suffix text
// and the shaped clan tag retain the colour supplied by the caller.
static float draw_label(ImDrawList* list, ImFont* font, float size, ImVec2 pos, ImU32 color,
                        std::string_view label, const char* nickname) {
    const int id = nickname ? runtime_player_swatch(nickname) : 0;
    const auto at = id > 0 ? label.find(nickname) : label.npos;
    if (at == label.npos) return draw(list,font,size,pos,color,split(label));
    const float start = pos.x;
    pos.x += draw(list,font,size,pos,color,split(label.substr(0,at)));
    const unsigned rgb = runtime_swatch_rgb(id);
    const ImU32 ink = IM_COL32((rgb>>16)&255,(rgb>>8)&255,rgb&255,(color>>IM_COL32_A_SHIFT)&255);
    pos.x += draw(list,font,size,pos,ink,split(nickname));
    pos.x += draw(list,font,size,pos,color,split(label.substr(at+std::string_view(nickname).size())));
    return pos.x-start;
}

void Draw(ImDrawList* list, ImFont* font, float size, ImVec2 pos, ImU32 color,
          std::string_view label, const char* nickname) {
    draw_label(list,font,size,pos,color,label,nickname);
}
void Text(std::string_view label, ImVec4 color, const char* nickname) {
    const float size = ImGui::GetFontSize();
    const float w = draw_label(ImGui::GetWindowDrawList(),ImGui::GetFont(),size,
                               ImGui::GetCursorScreenPos(),ImGui::GetColorU32(color),label,nickname);
    ImGui::Dummy(ImVec2(w,size));
}
void Reset() { cache.clear(); } // a new context owns a new atlas
}
