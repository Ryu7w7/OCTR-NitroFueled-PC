// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <miniz.h>
#include "common/hash.h"
#include "common/serdes.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/recompiler.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/bb_hud_scale.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 12u; // bbport: no NoContraction on macOS; GS emulation
static constexpr u32 ShaderMetaVersion = 10u; // bbport: GS emulation fields in the runtime info
static constexpr u32 PipelineKeyVersion = 8u; // bbport: GS emulation pass in the key
} // namespace Serialization

namespace Vulkan {

/// SacredFire: set while the shader prebuild reads recipe entries (see PrebuildFromRecipe).
thread_local bool g_prebuild_reading = false;

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    // Emulated geometry shader passes embed session-local buffer device addresses.
    if (!Storage::DataBase::Instance().IsOpened() || key.gs_emulation) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    // SacredFire: the vertex buffer descriptors the fetch shader reads through a pointer in the user
    // data. Translation reads them (number conversion and swizzle), so the shader prebuild needs
    // them to translate this permutation again away from the game.
    std::vector<Shader::Info::ReplaySharp> sharps;
    if (fetch_shader_data) {
        for (const auto& attrib : fetch_shader_data->attributes) {
            const auto buffer = info.ReadUdReg<AmdGpu::Buffer>(attrib.sgpr_base, attrib.dword_offset);
            Shader::Info::ReplaySharp sharp{attrib.sgpr_base, attrib.dword_offset, {}};
            std::memcpy(sharp.dwords.data(), &buffer, std::min(sizeof(buffer), sizeof(sharp.dwords)));
            sharps.push_back(sharp);
        }
    }
    Serialization::Writer extra{ar};
    extra.Write(static_cast<u32>(sharps.size()));
    for (const auto& sharp : sharps) {
        extra.Write(&sharp, sizeof(sharp));
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx,
                    std::vector<Shader::Info::ReplaySharp>* sharps = nullptr) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);
    if (sharps) {
        Serialization::Reader extra{ar};
        u32 count{};
        extra.Read(count);
        if (count > 64) {
            return false;
        }
        sharps->resize(count);
        for (auto& sharp : *sharps) {
            extra.Read(&sharp, sizeof(sharp));
        }
    }

    // Motion vertex shaders embed session-local buffer device addresses. They must be
    // recompiled for the current allocation, never loaded from a previous process.
    if (info.hw_stage == Shader::HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors) {
        return false;
    }
    // bbport: so do the stages of an emulated geometry shader (GsEmulation).
    if ((info.hw_stage == Shader::HwStage::Export && spec.runtime_info.hw.es.emulate_store) ||
        (info.hw_stage == Shader::HwStage::Geometry && spec.runtime_info.hw.gs.emulate_as_vertex)) {
        return false;
    }

    // sacredfire: only the vertex stage carries a fetch shader. Stages are loaded in SwStage order
    // (fragment, tessellation, vertex, geometry), and an unconditional assignment let a geometry
    // stage clear the fetch shader the vertex stage had just set: every preloaded pipeline with a
    // geometry shader (CTR's fur) was built without vertex inputs. RefreshGraphicsKey keeps the
    // last stage that had one; do the same here.
    if (spec.fetch_shader_data) {
        fetch_shader_data = spec.fetch_shader_data;
    }
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    // bbport: built before it is inserted: a pipeline the driver rejects while preloading throws
    // (Serialization::CorruptData) and leaves no empty entry behind.
    auto pipeline =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *sel.infos[0], sel.modules[0], sdata, true);
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);
    it.value() = std::move(pipeline);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    sel.graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    auto pipeline = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
        sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, true);
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    ASSERT(is_new);
    it.value() = std::move(pipeline);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, sel.fetch_shader, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    // bbport: a SPIR-V binary starts with its magic number and a 5-word header; anything else is
    // a damaged file (crash or power loss while it was written) the driver must not see.
    if (spv.size() < 5 || spv[0] != 0x07230203u) {
        if (!spv.empty()) {
            throw Serialization::CorruptData{"damaged SPIR-V in the shader cache"};
        }
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};
    // bbport: a module the driver rejects is a damaged entry (WarmUp rebuilds the cache), not a
    // fatal error as in CompileSPV.
    const auto compile = [&] {
        const auto hud = BbHud::ForModule(program->info.pgm_hash, perm_idx, spv);
        const auto& code = hud ? *hud : spv; // the cache keeps the shader as it is
        auto [result, created] = instance.GetDevice().createShaderModule(
            {.codeSize = code.size() * sizeof(u32), .pCode = code.data()});
        if (result != vk::Result::eSuccess) {
            throw Serialization::CorruptData{"cached SPIR-V rejected by the driver"};
        }
        return created;
    };

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = compile();
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = compile();
        }
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    sel.infos[stage] = &it_pgm.value()->info;
    sel.modules[stage] = module;

    return true;
}

// SacredFire: the shader prebuild. A new player's first races used to stop for tens to hundreds of
// milliseconds whenever a shader or pipeline was met for the first time. The port ships a recipe
// (BB_SHADER_RECIPE: a zip of the .meta and .key entries of long play sessions; no game code, and
// nothing in it depends on the GPU), and scripts/gcn_shaders.py takes the GCN code of the game's
// shaders from the player's own dump (BB_GCN_SHADERS). At the first start on a GPU and driver,
// every recipe entry is translated again with this GPU's profile, checked by the driver and saved
// in the player's cache, which WarmUp then loads as always. Shaders the player's cache already has
// stay as they are, and a pipeline the driver rejects is left out: the game compiles anything
// missing during play, as before.
namespace {

std::filesystem::path EnvPath(const char* name) {
#ifdef _WIN32
    const std::wstring wide(name, name + std::strlen(name));
    const wchar_t* value = _wgetenv(wide.c_str());
#else
    const char* value = std::getenv(name);
#endif
    return value && *value ? std::filesystem::path{value} : std::filesystem::path{};
}

bool ReadWhole(const std::filesystem::path& path, std::vector<u8>& out) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), {});
    return true;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file{path};
    return file ? std::string(std::istreambuf_iterator<char>(file), {}) : std::string{};
}

void WriteText(const std::filesystem::path& path, const std::string& text, bool append = false) {
    std::ofstream file{path, append ? std::ios::app : std::ios::trunc};
    file << text;
}

/// scripts/gcn_shaders.py: "BBGCN002", u32 count, u32 0, count x {u64 hash, u32 offset,
/// u32 bytes, u32 type, u32 0}, then the code of each shader. A geometry shader and its copy
/// shader (type 6) share a hash: copy shaders are kept apart.
struct GcnPack {
    static constexpr u32 CopyShaderType = 6;
    std::vector<u8> data;
    std::unordered_map<u64, std::span<const u32>> code;
    std::unordered_map<u64, std::span<const u32>> copy_code;

    bool Load(const std::filesystem::path& path) {
        constexpr size_t EntrySize = 24;
        if (!ReadWhole(path, data) || data.size() < 16 ||
            std::memcmp(data.data(), "BBGCN002", 8) != 0) {
            return false;
        }
        u32 count{};
        std::memcpy(&count, data.data() + 8, sizeof(count));
        if (16 + u64{count} * EntrySize > data.size()) {
            return false;
        }
        for (u32 i = 0; i < count; ++i) {
            const u8* entry = data.data() + 16 + u64{i} * EntrySize;
            u64 hash{};
            u32 offset{}, bytes{}, type{};
            std::memcpy(&hash, entry, 8);
            std::memcpy(&offset, entry + 8, 4);
            std::memcpy(&bytes, entry + 12, 4);
            std::memcpy(&type, entry + 16, 4);
            if (offset % 4 || bytes % 4 || bytes == 0 || u64{offset} + bytes > data.size()) {
                return false;
            }
            const std::span shader{reinterpret_cast<const u32*>(data.data() + offset), bytes / 4};
            (type == CopyShaderType ? copy_code : code).emplace(hash, shader);
        }
        return true;
    }
};

} // namespace

void PipelineCache::PrebuildFromRecipe() {
    auto& db = Storage::DataBase::Instance();
    const auto dir = db.FolderPath();
    const auto recipe_path = EnvPath("BB_SHADER_RECIPE");
    const auto gcn_path = EnvPath("BB_GCN_SHADERS");
    if (dir.empty() || recipe_path.empty() || gcn_path.empty()) {
        return;
    }
    std::vector<u8> recipe;
    mz_zip_archive zip{};
    if (!ReadWhole(recipe_path, recipe) ||
        !mz_zip_reader_init_mem(&zip, recipe.data(), recipe.size(), 0)) {
        std::printf("Shaders: the shader recipe %s could not be read\n",
                    recipe_path.string().c_str());
        return;
    }
    struct ZipEnd {
        mz_zip_archive* zip;
        ~ZipEnd() {
            mz_zip_reader_end(zip);
        }
    } zip_end{&zip};
    const u32 files = mz_zip_reader_get_num_files(&zip);

    // This recipe with this build's formats: done once per cache (a new GPU, driver or port build
    // clears the cache, and so the marker).
    u64 id = 0xcbf29ce484222325ull ^ (u64{Serialization::ShaderBinaryVersion} << 40) ^
             (u64{Serialization::ShaderMetaVersion} << 20) ^ Serialization::PipelineKeyVersion;
    for (u32 i = 0; i < files; ++i) {
        mz_zip_archive_file_stat stat{};
        if (mz_zip_reader_file_stat(&zip, i, &stat)) {
            id = (id ^ stat.m_crc32 ^ (u64{stat.m_uncomp_size} << 32)) * 0x100000001b3ull;
        }
    }
    recipe_id = id;
    const std::string id_text = fmt::format("{:016x}", id);
    const auto marker = dir / "recipe.done";
    if (ReadText(marker).starts_with(id_text)) {
        return; // prepared before, or turned off after a damaged cache
    }

    // An entry that stopped the game while it was translated is left out from then on.
    const auto attempt = dir / "recipe.attempt";
    const auto bad_list = dir / "recipe.bad";
    std::unordered_set<u32> bad;
    if (const auto text = ReadText(attempt); text.starts_with(id_text + " ")) {
        WriteText(bad_list, text + "\n", true);
    }
    {
        std::ifstream list{bad_list};
        std::string line_id;
        u32 index{};
        while (list >> line_id >> index) {
            if (line_id == id_text) {
                bad.insert(index);
            }
        }
    }

    GcnPack pack;
    if (!pack.Load(gcn_path)) {
        std::printf("Shaders: no game shaders at %s; the recipe waits for the next start\n",
                    gcn_path.string().c_str());
        return;
    }

    // Programs the player's cache already has (entries of this build's format): theirs stay,
    // and recipe pipelines using them are left out, so no permutation lists get mixed.
    std::unordered_set<u64> own;
    std::unordered_set<std::string> own_keys;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const auto name = entry.path().filename().string();
        if (name.ends_with(".key")) {
            own_keys.insert(name);
            continue;
        }
        if (!name.ends_with(".spv")) {
            continue;
        }
        char* end{};
        const u64 pgm = std::strtoull(name.c_str(), &end, 16);
        if (!end || *end != '_') {
            continue;
        }
        const u64 perm = std::strtoull(end + 1, nullptr, 10);
        std::ifstream meta{dir / fmt::format("{:#018x}.meta", HashCombine(pgm, perm)),
                           std::ios::binary};
        u32 versions[2]{};
        if (meta.read(reinterpret_cast<char*>(versions), sizeof(versions)) &&
            versions[0] == Serialization::ShaderMetaVersion &&
            versions[1] == Serialization::ShaderBinaryVersion) {
            own.insert(pgm);
        }
    }

    const auto started = std::chrono::steady_clock::now();
    u32 metas = 0;
    for (u32 i = 0; i < files; ++i) {
        char name[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE]{};
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        metas += std::string_view{name}.ends_with(".meta");
    }
    std::printf("Shaders: preparing %u shaders of the recipe for this GPU (once)\n", metas);
    std::fflush(stdout);

    std::unordered_set<u64> built; // permutation hashes saved by this pass
    u32 n_built = 0, n_theirs = 0, n_missing = 0, n_skipped = 0, n_failed = 0, seen = 0;
    for (u32 i = 0; i < files; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat) ||
            !std::string_view{stat.m_filename}.ends_with(".meta")) {
            continue;
        }
        if (++seen % 500 == 0) {
            std::printf("Shaders: %u of %u\n", seen, metas);
            std::fflush(stdout);
        }
        if (bad.contains(i)) {
            ++n_failed;
            continue;
        }
        const u64 perm_hash = std::strtoull(stat.m_filename, nullptr, 16);
        std::vector<u8> blob(stat.m_uncomp_size);
        if (!mz_zip_reader_extract_to_mem(&zip, i, blob.data(), blob.size(), 0)) {
            ++n_skipped;
            continue;
        }
        Shader::Info rec{};
        std::optional<Shader::Gcn::FetchShaderData> fetch{};
        Shader::StageSpecialization spec{};
        size_t perm_idx{};
        std::vector<Shader::Info::ReplaySharp> sharps;
        bool loaded = false;
        g_prebuild_reading = true;
        try {
            Serialization::Archive ar{std::move(blob)};
            loaded = LoadShaderMeta(ar, rec, fetch, spec, perm_idx, &sharps);
        } catch (const std::exception&) {
            loaded = false;
        }
        g_prebuild_reading = false;
        const u64 pgm = rec.pgm_hash;
        if (!loaded || HashCombine(pgm, u64{perm_idx}) != perm_hash ||
            rec.flattened_ud_buf.size() < Shader::ShaderParams::NumShaderUserData ||
            (rec.has_fetch_shader && !spec.fetch_shader_data) ||
            // Tessellation constants are read from game memory while translating.
            rec.sw_stage == Shader::SwStage::TessellationControl ||
            rec.sw_stage == Shader::SwStage::TessellationEval) {
            ++n_skipped;
            continue;
        }
        if (own.contains(pgm)) {
            ++n_theirs;
            continue;
        }
        const auto code = pack.code.find(pgm);
        if (code == pack.code.end()) {
            ++n_missing;
            continue;
        }

        std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
        std::copy_n(rec.flattened_ud_buf.begin(), user_data.size(), user_data.begin());
        const Shader::ShaderParams params{
            .user_data = user_data, .code = code->second, .hash = pgm};
        Shader::Info info{rec.hw_stage, rec.sw_stage, params};
        auto runtime_info = spec.runtime_info;
        auto binding = spec.start;
        if (rec.hw_stage == Shader::HwStage::Geometry) {
            // A geometry shader's translation reads its copy shader (recorded as a game address).
            const auto copy = pack.copy_code.find(runtime_info.hw.gs.vs_copy_hash);
            if (copy == pack.copy_code.end()) {
                ++n_missing;
                continue;
            }
            runtime_info.hw.gs.vs_copy = copy->second;
        }

        WriteText(attempt, fmt::format("{} {}", id_text, i));
        // Until the entry is saved: RegisterShaderMeta reads the fetch shader's descriptors too.
        const Shader::Info::Replay replay{&rec.flattened_ud_buf, &spec.fetch_shader_data, sharps};
        Shader::Info::replay = &replay;
        struct ReplayEnd {
            ~ReplayEnd() {
                Shader::Info::replay = nullptr;
            }
        } replay_end;
        std::vector<u32> spv;
        try {
            auto program = Shader::TranslateProgram(params.code, pools, info, runtime_info, profile);
            spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
        } catch (const std::exception&) {
            spv.clear();
        }

        // The program must come out as recorded (its resources are what the specialization and
        // the pipelines were made for).
        const bool same = !spv.empty() && !replay.missed && !info.translation_failed &&
                          info.buffers.size() == rec.buffers.size() &&
                          info.images.size() == rec.images.size() &&
                          info.samplers.size() == rec.samplers.size() &&
                          info.fmasks.size() == rec.fmasks.size() &&
                          info.ud_mask.mask == rec.ud_mask.mask &&
                          info.has_fetch_shader == rec.has_fetch_shader &&
                          info.flattened_ud_buf.size() == rec.flattened_ud_buf.size();
        if (!same) {
            ++n_failed;
            continue;
        }
        // WarmUp takes a module the driver rejects for a damaged cache: checked here instead.
        auto [result, module] = instance.GetDevice().createShaderModule(
            {.codeSize = spv.size() * sizeof(u32), .pCode = spv.data()});
        if (result != vk::Result::eSuccess) {
            ++n_failed;
            continue;
        }
        instance.GetDevice().destroyShaderModule(module);

        RegisterShaderBinary(std::move(spv), pgm, perm_idx);
        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        built.insert(perm_hash);
        ++n_built;
    }
    std::filesystem::remove(attempt, ec);
    db.Flush();
    const auto translated = std::chrono::steady_clock::now();

    // The recipe's pipelines whose shaders were all prepared here. Each is created once now: one
    // the driver rejects is not saved (WarmUp would take it for damage and clear the cache).
    u32 n_keys = 0, n_keys_skipped = 0, n_keys_rejected = 0;
    for (u32 i = 0; i < files; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat) ||
            !std::string_view{stat.m_filename}.ends_with(".key")) {
            continue;
        }
        const std::string name{stat.m_filename};
        if (own_keys.contains(name)) {
            continue;
        }
        std::vector<u8> blob(stat.m_uncomp_size);
        if (!mz_zip_reader_extract_to_mem(&zip, i, blob.data(), blob.size(), 0)) {
            ++n_keys_skipped;
            continue;
        }
        bool usable = false, compute = false;
        try {
            Serialization::Archive ar{std::vector<u8>{blob}};
            Serialization::Reader reader{ar};
            u32 version{}, is_compute{};
            reader.Read(version);
            reader.Read(is_compute);
            compute = is_compute != 0;
            if (version == Serialization::PipelineKeyVersion) {
                if (compute) {
                    ComputePipelineKey key{};
                    key.Deserialize(ar);
                    usable = built.contains(key.value) && !compute_pipelines.contains(key);
                } else {
                    GraphicsPipelineKey key{};
                    key.Deserialize(ar);
                    usable = !graphics_pipelines.contains(key);
                    for (const u64 hash : key.stage_hashes) {
                        usable &= hash == 0 || built.contains(hash);
                    }
                }
            }
        } catch (const std::exception&) {
            usable = false;
        }
        if (!usable) {
            ++n_keys_skipped;
            continue;
        }
        bool created = false;
        try {
            Serialization::Archive ar{std::vector<u8>{blob}};
            Serialization::Reader reader{ar};
            u32 version{}, is_compute{};
            reader.Read(version);
            reader.Read(is_compute);
            created = compute ? LoadComputePipeline(ar) : LoadGraphicsPipeline(ar);
        } catch (const std::exception&) {
            created = false;
            ++n_keys_rejected;
        }
        sel.infos.fill(nullptr);
        sel.modules.fill(nullptr);
        sel.fetch_shader.reset();
        if (!created) {
            ++n_keys_skipped;
            continue;
        }
        db.Save(Storage::BlobType::PipelineKey, name.substr(0, name.size() - 4), std::move(blob));
        ++n_keys;
    }
    db.Flush();

    // Everything is created again from the cache by WarmUp (fast now: the pipeline cache object
    // and the driver have them).
    graphics_pipelines.clear();
    compute_pipelines.clear();
    std::unordered_set<VkShaderModule> modules;
    for (const auto& [_, program] : program_cache) {
        if (!program) {
            continue;
        }
        for (const auto& permutation : program->modules) {
            if (permutation.module) {
                modules.insert(VkShaderModule(permutation.module));
            }
        }
    }
    for (const VkShaderModule module : modules) {
        instance.GetDevice().destroyShaderModule(vk::ShaderModule{module});
    }
    program_cache.clear();

    WriteText(marker, id_text + "\n");
    const auto now = std::chrono::steady_clock::now();
    std::printf("Shaders: recipe prepared in %.1f s: %u shaders in %.1f s (%u already in the cache, "
                "%u not in this game's files, %u not usable here, %u failed), %u pipelines in "
                "%.1f s (%u left out, %u rejected by the driver)\n",
                std::chrono::duration<double>(now - started).count(), n_built,
                std::chrono::duration<double>(translated - started).count(), n_theirs, n_missing,
                n_skipped, n_failed, n_keys, std::chrono::duration<double>(now - translated).count(),
                n_keys_skipped, n_keys_rejected);
    std::fflush(stdout);
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    // SacredFire: the profile blob also names the entry formats, so a port update that changes them
    // starts a fresh cache instead of leaving every old entry behind as stale.
    const u32 versions[3]{Serialization::ShaderMetaVersion, Serialization::ShaderBinaryVersion,
                          Serialization::PipelineKeyVersion};
    std::vector<u8> expected(sizeof(profile) + sizeof(versions));
    std::memcpy(expected.data(), &profile, sizeof(profile));
    std::memcpy(expected.data() + sizeof(profile), versions, sizeof(versions));

    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    bool fresh = false;
    if (profile_data.empty()) {
        Storage::DataBase::Instance().FinishPreload();
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::vector<u8>{expected});
        fresh = true;
    } else {
        Shader::Profile cached_profile{};
        const bool sized = profile_data.size() == expected.size();
        if (sized) {
            std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
        }
        if (!sized || cached_profile != profile ||
            std::memcmp(profile_data.data() + sizeof(profile), versions, sizeof(versions)) != 0) {
            // bbport: upstream closed the cache for the session here, so it was never rewritten
            // and every later session compiled every shader again (stutters on each new area).
            // Start a fresh cache for this build and GPU instead.
            LOG_WARNING(Render,
                        "Pipeline cache isn't compatible with current system: rebuilding it");
            Storage::DataBase::Instance().Clear();
            Storage::DataBase::Instance().FinishPreload();
            Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                               std::vector<u8>{expected});
            fresh = true;
        }
    }
    if (fresh && EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    // SacredFire: a new or rebuilt cache gets the recipe's shaders and pipelines first.
    PrebuildFromRecipe();
    const auto load_started = std::chrono::steady_clock::now();

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    u32 num_damaged{};

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;
            // bbport: a damaged entry (cut short by a crash or a power loss, or rejected by the
            // driver) used to stop the game at every start until the cache was deleted by hand
            // (issues #28, #38, #39). It is counted here and the cache is rebuilt below.
            try {
                Serialization::Archive ar{std::move(data)};
                Serialization::Reader pldata{ar};

                u32 version{};
                pldata.Read(version);
                if (version != Serialization::PipelineKeyVersion) {
                    return;
                }

                u32 is_compute{};
                pldata.Read(is_compute);

                bool result{};
                if (is_compute) {
                    result = LoadComputePipeline(ar);
                } else {
                    result = LoadGraphicsPipeline(ar);
                }

                if (result) {
                    ++num_pipelines;
                }
            } catch (const std::exception& e) {
                if (num_damaged++ == 0) {
                    LOG_WARNING(Render, "Pipeline cache: damaged entry ({})", e.what());
                }
                sel.infos.fill(nullptr);
                sel.modules.fill(nullptr);
                sel.fetch_shader.reset();
            }
        });

    if (num_damaged) {
        // Nothing preloaded is trusted: modules of a damaged entry may sit in programs that later
        // lookups would reuse. Start as with no cache and write a fresh one.
        LOG_WARNING(Render, "Pipeline cache: {} damaged entries, rebuilding it", num_damaged);
        graphics_pipelines.clear();
        compute_pipelines.clear();
        std::unordered_set<VkShaderModule> modules;
        for (const auto& [_, program] : program_cache) {
            if (!program) {
                continue;
            }
            for (const auto& permutation : program->modules) {
                if (permutation.module) {
                    modules.insert(VkShaderModule(permutation.module));
                }
            }
        }
        for (const VkShaderModule module : modules) {
            instance.GetDevice().destroyShaderModule(vk::ShaderModule{module});
        }
        program_cache.clear();
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(expected));
        // SacredFire: the recipe is not prepared again for this cache (Clear removed its marker):
        // whatever damaged it would come back at every other start.
        if (recipe_id) {
            WriteText(Storage::DataBase::Instance().FolderPath() / "recipe.done",
                      fmt::format("{:016x} off\n", recipe_id));
        }
        return;
    }

    if (num_pipelines) {
        std::printf("Shaders: %u pipelines loaded from the cache in %.1f s\n", num_pipelines,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                  load_started)
                        .count());
        std::fflush(stdout);
    }
    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        // bbport: the size is checked before the code is registered (it becomes executable):
        // a cut-short file must not have bytes past its end run as the walker.
        const auto code = ar.CurrPtr();
        ar.Advance(walker_func_size);
        // SacredFire: the shader prebuild reads recipe entries only to translate them again; their
        // walkers would fill the walker code buffer for nothing.
        walker_func = Vulkan::g_prebuild_reading ? nullptr : RegisterWalkerCode(code, walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
