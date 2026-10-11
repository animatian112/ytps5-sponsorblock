// youtube-tv - PS5 hardware video decode (Videodec2) session.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// READ THIS BEFORE TRUSTING A LINE BELOW.
//
// The argument layouts of libSceVideodec2 are no longer a reconstruction. The
// structs, constants and call order below are the ones EVO-PLAYER-PS5 published
// after running them on hardware (PS5 12.70, 2026-09-01; a 1080p H.264 IDR to a
// valid NV12 frame, every call rc=0). Sources, both public:
//
//   * projects/evoplayer/media/include/sce/sce_videodec2.h
//     (EVO-PLAYER-PS5, sainsaji) - struct layouts, codec/level constants, the
//     memory-typing crib;
//   * docs/evo-pro/videodec2-abi.md - the exact bring-up sequence and the
//     per-region protection table.
//
// They in turn transcribe blackbearreloaded/ProsperoLight, the production
// Moonlight client that decodes on the VCN block, and the sequence was
// re-confirmed by an on-console self-test. docs/HARDWARE_DECODE.md in this repo
// documents the proven *behaviour*; these references supply the layouts that
// document deliberately leaves out.
//
// The verified session order is:
//
//   QueryComputeMemoryInfo -> AllocateComputeQueue -> (config) ->
//   QueryDecoderMemoryInfo -> map each returned region -> CreateDecoder ->
//   Reset -> Decode/Flush -> DeleteDecoder.
//
// The same references show that the library's own InitLibrary/MapDirectMemory
// path is NOT what the proven bring-up uses: it maps every region itself with
// the kernel direct-memory family (type 12, 16 KiB alignment) and the CPU
// workspace with named flexible memory. This file does the same, so no call
// below passes a guessed argument shape.
//
// Every step logs once with its return value and the import count, the first
// failure names the step and the return value in *error, a null import is never
// called, and the caller falls back to the software decoder.

#include "platform/ps5/videodec.hpp"

#include "platform/ps5/system.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

extern "C"
{
    // Console libkernel/libc. sceKernelMapNamedFlexibleMemory and
    // sceKernelReleaseFlexibleMemory are the flexible-memory pair the verified
    // path uses for the CPU workspace; the direct-memory family is the same
    // clean-room surface the OpenGL runtime already uses.
    std::size_t sceKernelGetDirectMemorySize();
    int sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end,
                                      std::size_t length, std::size_t alignment, int memory_type,
                                      std::int64_t *physical_address);
    int sceKernelMapDirectMemory(void **address, std::size_t length, int protection, int flags,
                                 std::int64_t physical_address, std::size_t alignment);
    int sceKernelReleaseDirectMemory(std::int64_t address, std::size_t length);
    int sceKernelMapNamedFlexibleMemory(void **address, std::size_t length, int protection,
                                        int flags, const char *name);
    int sceKernelReleaseFlexibleMemory(void *address, std::size_t length);
    int munmap(void *address, std::size_t length);
}

namespace hui::ps5
{

namespace
{

// ---------------------------------------------------------------------------
// The 26 imports, moved here from videodec_probe.cpp.

// Signatures are deliberately absent: only the addresses matter. The "shape"
// section below casts each address to the prototype this file calls it with,
// which keeps every name in one table and makes the reachability count real.
extern "C"
{
    void sceVideodec2InitLibrary();
    void sceVideodec2TermLibrary();
    void sceVideodec2QueryLibraryInfo();
    void sceVideodec2QueryDecoderInfo();
    void sceVideodec2CreateDecoder();
    void sceVideodec2CreateDecoderBid();
    void sceVideodec2CreateHevcDecoder();
    void sceVideodec2DeleteDecoder();
    void sceVideodec2PresetDecoderConfigInfo();
    void sceVideodec2PresetDecoderConfigInfoAvc();
    void sceVideodec2PresetDecoderConfigInfoHevc();
    void sceVideodec2PresetDecoderConfigInfoVp9();
    void sceVideodec2QueryDecoderMemoryInfo();
    void sceVideodec2QueryHevcDecoderMemoryInfo();
    void sceVideodec2QueryComputeMemoryInfo();
    void sceVideodec2AllocateComputeQueue();
    void sceVideodec2ReleaseComputeQueue();
    void sceVideodec2MapDirectMemory();
    void sceVideodec2MapMemory();
    void sceVideodec2Decode();
    void sceVideodec2Flush();
    void sceVideodec2Reset();
    void sceVideodec2GetPictureInfo();
    void sceVideodec2GetAvcPictureInfo();
    void sceVideodec2GetHevcPictureInfo();
    void sceVideodec2GetVp9PictureInfo();
}

using Symbol = void (*)();

// The order follows the pipeline: library, decoder, presets, memory, decode,
// picture info. The loader leaves entries null when the console refuses the
// library to a homebrew title, so nothing below may call one without checking.
// volatile: an optimizer that folds "the address of an extern function is
// non-null" would delete the references, and with them the console imports.
//
// Several entries - the library lifecycle, the presets, MapDirectMemory,
// MapMemory and the picture-info family - have no public reference and are NOT
// called by this file. They stay in the table because the shipped eboot must
// carry all 26 NIDs (docs/HARDWARE_DECODE.md); the verified path below reaches
// the decoder without them.
volatile Symbol const kDecoderImports[] = {
    &sceVideodec2InitLibrary,
    &sceVideodec2TermLibrary,
    &sceVideodec2QueryLibraryInfo,
    &sceVideodec2QueryDecoderInfo,
    &sceVideodec2CreateDecoder,
    &sceVideodec2CreateDecoderBid,
    &sceVideodec2CreateHevcDecoder,
    &sceVideodec2DeleteDecoder,
    &sceVideodec2PresetDecoderConfigInfo,
    &sceVideodec2PresetDecoderConfigInfoAvc,
    &sceVideodec2PresetDecoderConfigInfoHevc,
    &sceVideodec2PresetDecoderConfigInfoVp9,
    &sceVideodec2QueryDecoderMemoryInfo,
    &sceVideodec2QueryHevcDecoderMemoryInfo,
    &sceVideodec2QueryComputeMemoryInfo,
    &sceVideodec2AllocateComputeQueue,
    &sceVideodec2ReleaseComputeQueue,
    &sceVideodec2MapDirectMemory,
    &sceVideodec2MapMemory,
    &sceVideodec2Decode,
    &sceVideodec2Flush,
    &sceVideodec2Reset,
    &sceVideodec2GetPictureInfo,
    &sceVideodec2GetAvcPictureInfo,
    &sceVideodec2GetHevcPictureInfo,
    &sceVideodec2GetVp9PictureInfo,
};

enum ImportIndex : std::size_t
{
    kInitLibrary = 0,
    kTermLibrary,
    kQueryLibraryInfo,
    kQueryDecoderInfo,
    kCreateDecoder,
    kCreateDecoderBid,
    kCreateHevcDecoder,
    kDeleteDecoder,
    kPresetDecoderConfigInfo,
    kPresetDecoderConfigInfoAvc,
    kPresetDecoderConfigInfoHevc,
    kPresetDecoderConfigInfoVp9,
    kQueryDecoderMemoryInfo,
    kQueryHevcDecoderMemoryInfo,
    kQueryComputeMemoryInfo,
    kAllocateComputeQueue,
    kReleaseComputeQueue,
    kMapDirectMemory,
    kMapMemory,
    kDecode,
    kFlush,
    kReset,
    kGetPictureInfo,
    kGetAvcPictureInfo,
    kGetHevcPictureInfo,
    kGetVp9PictureInfo,
    kImportCount,
};

static_assert(kImportCount == static_cast<std::size_t>(kVideodecImportCount),
              "the table and the header's import count must agree");

// ---------------------------------------------------------------------------
// Memory typing. docs/evo-pro/videodec2-abi.md §2 ("alloc_direct") and the
// memory-typing crib at the bottom of sce_videodec2.h:
//
//   alloc_direct(size, prot) =
//     sceKernelAllocateDirectMemory(0, limit, size, 0x4000, 12, &start)
//     sceKernelMapDirectMemory(&ptr, size, prot, 0, start, 0x4000)
//
//   | prot | meaning             | used for                                   |
//   | 0x33 | CPU r/w + GPU all   | compute cpu_gpu, decoder cpu_gpu           |
//   | 0x32 | CPU w   + GPU all   | decoder gpu, AU pool, frame pool           |
//   | 0x03 | flexible, CPU r/w   | decoder cpu workspace (named flexible)     |
//
// 12 is the only memory type the verified path passes; 0x4000 is the alignment
// it uses everywhere. (The current evo_vdec_native.c passes 0x33 for all four
// direct regions; the abi document distinguishes 0x32 for the decoder gpu and
// frame pools, which is what this file follows.)
constexpr std::size_t kDirectAlignment = 0x4000;
constexpr int kDirectMemoryType = 12;
constexpr int kProtectionCpuGpu = 0x33;
constexpr int kProtectionGpu = 0x32;
constexpr int kProtectionFlexibleCpu = 0x03;

// Codec identifiers and the resource selector, from sce_videodec2.h.
constexpr std::uint32_t kCodecAvc = 1;        // SCE_VIDEODEC2_CODEC_AVC
constexpr std::uint32_t kCodecVp9 = 2382845u; // SCE_VIDEODEC2_CODEC_VP9 (0x00245BFD)
constexpr std::uint32_t kResourceCompute = 1; // SCE_VIDEODEC2_RESOURCE_COMPUTE

std::size_t align_up(std::size_t value, std::size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// Coded geometry is the requested size rounded up to the next macroblock row /
// 16-pixel block; the verified path passes 1080p as 1920x1088 (abi.md §5,
// roundup16 in evo_vdec_native.c).
std::int32_t roundup16(int value)
{
    return static_cast<std::int32_t>((value + 15) & ~15);
}

// Per-codec configuration, from docs/evo-pro/videodec2-abi.md §5 and the
// g_codec[] table in evo_vdec_native.c:
//   AVC: profile_idc 100 (High); max_level is ITU level x10 - 51 (5.1) up to
//        1080p, 52 above it.
//   VP9: Profile 0; max_level 41 up to 1080p, 51 above (the x10 scale for VP9 is
//        that table's own "best guess", but the VP9 slot itself is
//        hardware-verified).
struct CodecParams
{
    std::uint32_t codec_type;
    std::uint32_t profile;
    int level_1080;
    int level_4k;
};

CodecParams codec_params(yt::Codec codec)
{
    if (codec == yt::Codec::vp9)
        return {kCodecVp9, 0u, 41, 51};
    return {kCodecAvc, 100u, 51, 52};
}

// ---------------------------------------------------------------------------
// Structures. Field names and order are sce_videodec2.h's; the sizes are
// asserted against that header so a transcription slip fails the build instead
// of corrupting the console.

// sce_videodec2.h SceVideodec2DecoderConfigInfo. Everything the decoder needs
// except the memory pointers; resource_type = Compute is what selects the
// compute path, and compute_queue is the handle AllocateComputeQueue returned.
struct DecoderConfigInfo
{
    std::uint64_t this_size;
    std::uint32_t resource_type;
    std::uint32_t codec_type;
    std::uint32_t profile;
    std::uint32_t max_level;
    std::int32_t max_width;
    std::int32_t max_height;
    std::int32_t max_dpb_frames;  // -1 = decoder self-sizes
    std::uint32_t pipeline_depth; // verified depth 1
    std::uint64_t compute_queue;  // handle from AllocateComputeQueue
    std::uint64_t cpu_affinity;   // verified 0x3F (cores 0-5)
    std::int32_t cpu_priority;    // verified 700
    std::uint32_t optimize_progressive;
    std::uint32_t check_memory_type;
    std::uint32_t reserved;
};
static_assert(sizeof(DecoderConfigInfo) == 0x48, "videodec2 config layout");

// sce_videodec2.h SceVideodec2DecoderMemoryInfo. Filled by
// QueryDecoderMemoryInfo; the caller stores each mapped pointer back into it
// before CreateDecoder.
struct DecoderMemoryInfo
{
    std::uint64_t this_size;
    std::uint64_t cpu_memory_size;
    void *cpu_memory;
    std::uint64_t gpu_memory_size;
    void *gpu_memory;
    std::uint64_t cpu_gpu_memory_size;
    void *cpu_gpu_memory;
    std::uint64_t max_frame_buffer_size;
    std::uint32_t frame_buffer_alignment;
    std::uint32_t reserved;
};
static_assert(sizeof(DecoderMemoryInfo) == 0x48, "videodec2 memory-info layout");

// sce_videodec2.h SceVideodec2ComputeMemoryInfo / SceVideodec2ComputeConfigInfo:
// the compute queue's own memory and its pipe/queue selection.
struct ComputeMemoryInfo
{
    std::uint64_t this_size;
    std::uint64_t cpu_gpu_size;
    void *cpu_gpu;
};
static_assert(sizeof(ComputeMemoryInfo) == 0x18, "videodec2 compute-memory layout");

struct ComputeConfigInfo
{
    std::uint64_t this_size;
    std::uint16_t pipe_id;
    std::uint16_t queue_id;
    std::uint8_t check_memory_type;
    std::uint8_t reserved0;
    std::uint16_t reserved1;
};
static_assert(sizeof(ComputeConfigInfo) == 0x10, "videodec2 compute-config layout");

// sce_videodec2.h SceVideodec2InputData: one compressed Annex-B access unit.
struct InputData
{
    std::uint64_t this_size;
    const std::uint8_t *au_data;
    std::uint64_t au_size;
    std::uint64_t presentation_time;
    std::uint64_t decode_time;
    std::uint64_t attached_data;
};
static_assert(sizeof(InputData) == 0x30, "videodec2 input-data layout");

// sce_videodec2.h SceVideodec2FrameBuffer: the caller-owned frame slot offered
// to one decode call. accepted is the decoder's acknowledgement.
struct FrameBuffer
{
    std::uint64_t this_size;
    void *frame_buffer;
    std::uint64_t frame_buffer_size;
    std::uint32_t accepted;
    std::uint32_t reserved;
};
static_assert(sizeof(FrameBuffer) == 0x20, "videodec2 frame-buffer layout");

// sce_videodec2.h SceVideodec2OutputInfo: what one decode call produced.
// pitch is the luma stride in samples, pitch_bytes in bytes.
struct OutputInfo
{
    std::uint64_t this_size;
    std::uint8_t valid;
    std::uint8_t error;
    std::uint8_t picture_count;
    std::uint8_t padding;
    std::uint32_t codec_type;
    std::uint32_t frame_width;
    std::uint32_t frame_pitch;
    std::uint32_t frame_height;
    std::uint32_t reserved;
    void *frame_buffer;
    std::uint64_t frame_buffer_size;
    std::uint32_t frame_format;
    std::uint32_t frame_pitch_in_bytes;
};
static_assert(sizeof(OutputInfo) == 0x38, "videodec2 output-info layout");

// ---------------------------------------------------------------------------
// Prototypes. Each is the shape sce_videodec2.h publishes for that entry point.

using QueryComputeMemoryInfoFn = int (*)(ComputeMemoryInfo *memory);
using AllocateComputeQueueFn = int (*)(const ComputeConfigInfo *config,
                                       const ComputeMemoryInfo *memory, void **compute_queue);
using ReleaseComputeQueueFn = int (*)(void *compute_queue);
using QueryDecoderMemoryInfoFn = int (*)(const DecoderConfigInfo *config,
                                         DecoderMemoryInfo *memory);
using CreateDecoderFn = int (*)(const DecoderConfigInfo *config, const DecoderMemoryInfo *memory,
                                void **decoder_out);
using DeleteDecoderFn = int (*)(void *decoder);
using ResetFn = int (*)(void *decoder);
using DecodeFn = int (*)(void *decoder, const InputData *input, FrameBuffer *frame,
                         OutputInfo *output);
using FlushFn = int (*)(void *decoder, FrameBuffer *frame, OutputInfo *output);

// ---------------------------------------------------------------------------
// Step logging: every step of the sequence writes exactly one line with its
// name, its return value and how many imports the loader bound.

void log_step(const char *step, int result, int reachable)
{
    sys::log("[YTV] videodec2 %s: 0x%08x (imports %d/%d)", step, static_cast<unsigned>(result),
             reachable, kVideodecImportCount);
}

std::string step_error(const char *step, int result)
{
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08x", static_cast<unsigned>(result));
    return std::string(step) + ": " + code;
}

void set_error(std::string *error, std::string message)
{
    if (error != nullptr)
        *error = std::move(message);
}

std::string unbound_error(int reachable)
{
    return "videodec2 imports not bound (" + std::to_string(reachable) + "/" +
           std::to_string(kVideodecImportCount) + ")";
}

// Runs one library step: never calls a null import, logs the step once, and on
// a negative result names the step in *error. Returns false when the caller
// must stop.
template <typename Fn, typename... Args>
bool call_step(const char *name, ImportIndex index, int reachable, std::string *error, Args... args)
{
    const Symbol slot = kDecoderImports[index];
    const auto fn = reinterpret_cast<Fn>(slot);
    if (fn == nullptr)
    {
        sys::log("[YTV] videodec2 %s: not bound (imports %d/%d)", name, reachable,
                 kVideodecImportCount);
        set_error(error, unbound_error(reachable));
        return false;
    }
    const int result = fn(args...);
    log_step(name, result, reachable);
    if (result < 0)
    {
        set_error(error, step_error(name, result));
        return false;
    }
    return true;
}

// One platform (sceKernel*) step: same logging, no import table.
bool platform_step(const char *name, int result, int reachable, std::string *error)
{
    log_step(name, result, reachable);
    if (result < 0)
    {
        set_error(error, step_error(name, result));
        return false;
    }
    return true;
}

// The verified direct-memory pair: allocate from the pool with type 12 and
// 16 KiB alignment, then map with the region's protection. Logs both calls and
// the resulting region so a console run can attribute a later failure.
bool alloc_direct(const char *region, std::size_t bytes, int protection, std::int64_t limit,
                  std::int64_t *physical, void **mapping, int reachable, std::string *error)
{
    char step[64];
    std::snprintf(step, sizeof(step), "sceKernelAllocateDirectMemory[%s]", region);
    if (!platform_step(step,
                       sceKernelAllocateDirectMemory(0, limit, bytes, kDirectAlignment,
                                                     kDirectMemoryType, physical),
                       reachable, error))
        return false;

    std::snprintf(step, sizeof(step), "sceKernelMapDirectMemory[%s]", region);
    if (!platform_step(
            step,
            sceKernelMapDirectMemory(mapping, bytes, protection, 0, *physical, kDirectAlignment),
            reachable, error))
        return false;

    sys::log("[YTV] videodec2 region %s: %zu bytes direct type=%d prot=0x%02x", region, bytes,
             kDirectMemoryType, protection);
    return true;
}

// The CPU workspace: named flexible memory, CPU r/w (0x03), the one region the
// verified path does not put in direct memory.
bool map_cpu_workspace(std::size_t bytes, void **mapping, int reachable, std::string *error)
{
    if (!platform_step("sceKernelMapNamedFlexibleMemory",
                       sceKernelMapNamedFlexibleMemory(mapping, bytes, kProtectionFlexibleCpu, 0,
                                                       "YtvVdecCpu"),
                       reachable, error))
        return false;
    sys::log("[YTV] videodec2 region cpu: %zu bytes flexible prot=0x%02x", bytes,
             kProtectionFlexibleCpu);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// The public surface.

int videodec_imports_reachable()
{
    int bound = 0;
    for (std::size_t index = 0; index < kImportCount; ++index)
    {
        if (kDecoderImports[index] != nullptr)
            ++bound;
    }
    return bound;
}

bool videodec_supports(yt::Codec codec)
{
    // docs/HARDWARE_DECODE.md: a console-proven decode exists for AVC and VP9
    // (and HEVC, which yt::Codec does not carry); AV1 is explicitly not
    // established there, so it stays on the software decoder, and the audio
    // codecs never had a hardware path at all.
    return codec == yt::Codec::avc || codec == yt::Codec::vp9;
}

// Session state: everything open() built and close() must undo. Defined here so
// the header stays free of the unverified structures above.
struct HardwareDecoder::Session
{
    void *decoder = nullptr;

    void *compute_queue = nullptr;
    void *compute_mapping = nullptr;
    std::int64_t compute_physical = 0;
    std::size_t compute_bytes = 0;

    void *cpu_mapping = nullptr; // flexible CPU workspace
    std::size_t cpu_bytes = 0;

    void *gpu_mapping = nullptr;
    std::int64_t gpu_physical = 0;
    std::size_t gpu_bytes = 0;

    void *cpu_gpu_mapping = nullptr;
    std::int64_t cpu_gpu_physical = 0;
    std::size_t cpu_gpu_bytes = 0;

    void *frame_pool = nullptr; // caller-owned decoded frame slot
    std::int64_t frame_physical = 0;
    std::size_t frame_pool_bytes = 0;
    std::size_t frame_slot_bytes = 0;

    int reachable = 0;
};

HardwareDecoder::HardwareDecoder() = default;
HardwareDecoder::HardwareDecoder(HardwareDecoder &&) noexcept = default;
HardwareDecoder &HardwareDecoder::operator=(HardwareDecoder &&) noexcept = default;

HardwareDecoder::~HardwareDecoder()
{
    close();
}

bool HardwareDecoder::open(yt::Codec codec, int width, int height, std::string *error)
{
    close();

    if (!videodec_supports(codec))
    {
        sys::log("[YTV] videodec2 open: no proven hardware path for %s", yt::codec_label(codec));
        set_error(error,
                  std::string("codec has no proven hardware path: ") + yt::codec_label(codec));
        return false;
    }
    if (width <= 0 || height <= 0)
    {
        sys::log("[YTV] videodec2 open: bad geometry %dx%d", width, height);
        set_error(error, "open: invalid picture size");
        return false;
    }

    m_session = std::make_unique<Session>();
    Session &session = *m_session;
    session.reachable = videodec_imports_reachable();
    const int reachable = session.reachable;
    sys::log("[YTV] videodec2 open: %s %dx%d (imports %d/%d)", yt::codec_label(codec), width,
             height, reachable, kVideodecImportCount);

    // The verified sequence (docs/evo-pro/videodec2-abi.md §2) opens with
    // sceSysmoduleLoadModule(207). This app carries libSceVideodec2 as static
    // imports and has no libSceSysmodule import, so the loader binds the 26
    // entry points (videodec_imports_reachable, reported at startup) and the
    // module-load step is the loader's job here.

    // 1. Compute memory and its queue, before any decoder exists: query the
    //    need, map it with the compute protection, then take the queue handle
    //    the decoder config references.
    ComputeMemoryInfo compute{};
    compute.this_size = sizeof(ComputeMemoryInfo);
    if (!call_step<QueryComputeMemoryInfoFn>("sceVideodec2QueryComputeMemoryInfo",
                                             kQueryComputeMemoryInfo, reachable, error, &compute))
    {
        close();
        return false;
    }
    const std::size_t compute_bytes =
        align_up(static_cast<std::size_t>(compute.cpu_gpu_size), kDirectAlignment);
    if (compute_bytes == 0)
    {
        sys::log("[YTV] videodec2 sceVideodec2QueryComputeMemoryInfo: no compute memory");
        set_error(error, "sceVideodec2QueryComputeMemoryInfo: no compute memory reported");
        close();
        return false;
    }

    const std::int64_t limit = static_cast<std::int64_t>(sceKernelGetDirectMemorySize());
    if (!alloc_direct("compute", compute_bytes, kProtectionCpuGpu, limit, &session.compute_physical,
                      &session.compute_mapping, reachable, error))
    {
        close();
        return false;
    }
    session.compute_bytes = compute_bytes;
    compute.cpu_gpu = session.compute_mapping;
    compute.cpu_gpu_size = compute_bytes;

    ComputeConfigInfo queue_config{};
    queue_config.this_size = sizeof(ComputeConfigInfo);
    if (!call_step<AllocateComputeQueueFn>("sceVideodec2AllocateComputeQueue",
                                           kAllocateComputeQueue, reachable, error, &queue_config,
                                           &compute, &session.compute_queue))
    {
        close();
        return false;
    }
    if (session.compute_queue == nullptr)
    {
        sys::log("[YTV] videodec2 sceVideodec2AllocateComputeQueue: null queue");
        set_error(error, "sceVideodec2AllocateComputeQueue: null compute queue");
        close();
        return false;
    }
    sys::log("[YTV] videodec2 compute queue: %p (%zu bytes)", session.compute_queue, compute_bytes);

    // 2. The decoder configuration is built explicitly, not left to a preset:
    //    resource_type = Compute plus the queue handle, an explicit per-codec
    //    profile/level, and the geometry/depth this session fixes.
    const CodecParams params = codec_params(codec);
    const bool high_res = width > 1920 || height > 1088;

    DecoderConfigInfo config{};
    config.this_size = sizeof(DecoderConfigInfo);
    config.resource_type = kResourceCompute;
    config.codec_type = params.codec_type;
    config.profile = params.profile;
    config.max_level = static_cast<std::uint32_t>(high_res ? params.level_4k : params.level_1080);
    config.max_width = roundup16(width);
    config.max_height = roundup16(height);
    config.max_dpb_frames = -1; // AutoFrameSetting, as the verified path passes it
    config.pipeline_depth = 1;  // proven depth, docs/HARDWARE_DECODE.md
    config.compute_queue = reinterpret_cast<std::uint64_t>(session.compute_queue);
    config.cpu_affinity = 0x3F; // cores 0-5, as the verified path passes it
    config.cpu_priority = 700;
    config.optimize_progressive = 1;
    config.check_memory_type = 0;
    sys::log("[YTV] videodec2 config: codec=%u profile=%u level=%u max=%dx%d resource=%u",
             static_cast<unsigned>(config.codec_type), static_cast<unsigned>(config.profile),
             static_cast<unsigned>(config.max_level), static_cast<int>(config.max_width),
             static_cast<int>(config.max_height), static_cast<unsigned>(config.resource_type));

    // 3. Query the decoder's own memory requirement for that configuration.
    DecoderMemoryInfo memory{};
    memory.this_size = sizeof(DecoderMemoryInfo);
    if (!call_step<QueryDecoderMemoryInfoFn>("sceVideodec2QueryDecoderMemoryInfo",
                                             kQueryDecoderMemoryInfo, reachable, error, &config,
                                             &memory))
    {
        close();
        return false;
    }
    sys::log("[YTV] videodec2 memory: cpu=%zu gpu=%zu shared=%zu frame=%zu align=%u",
             static_cast<std::size_t>(memory.cpu_memory_size),
             static_cast<std::size_t>(memory.gpu_memory_size),
             static_cast<std::size_t>(memory.cpu_gpu_memory_size),
             static_cast<std::size_t>(memory.max_frame_buffer_size),
             static_cast<unsigned>(memory.frame_buffer_alignment));

    // 4. Map each returned region the verified way: the CPU workspace is named
    //    flexible memory, the GPU and shared regions are separate direct-memory
    //    allocations (type 12) with their own protections.
    const std::size_t cpu_bytes =
        align_up(static_cast<std::size_t>(memory.cpu_memory_size), kDirectAlignment);
    if (!map_cpu_workspace(cpu_bytes, &memory.cpu_memory, reachable, error))
    {
        close();
        return false;
    }
    session.cpu_mapping = memory.cpu_memory;
    session.cpu_bytes = cpu_bytes;

    const std::size_t gpu_bytes =
        align_up(static_cast<std::size_t>(memory.gpu_memory_size), kDirectAlignment);
    if (!alloc_direct("gpu", gpu_bytes, kProtectionGpu, limit, &session.gpu_physical,
                      &session.gpu_mapping, reachable, error))
    {
        close();
        return false;
    }
    session.gpu_bytes = gpu_bytes;
    memory.gpu_memory = session.gpu_mapping;
    memory.gpu_memory_size = gpu_bytes;

    const std::size_t cpu_gpu_bytes =
        align_up(static_cast<std::size_t>(memory.cpu_gpu_memory_size), kDirectAlignment);
    if (cpu_gpu_bytes != 0)
    {
        if (!alloc_direct("cpu_gpu", cpu_gpu_bytes, kProtectionCpuGpu, limit,
                          &session.cpu_gpu_physical, &session.cpu_gpu_mapping, reachable, error))
        {
            close();
            return false;
        }
        session.cpu_gpu_bytes = cpu_gpu_bytes;
        memory.cpu_gpu_memory = session.cpu_gpu_mapping;
        memory.cpu_gpu_memory_size = cpu_gpu_bytes;
    }

    // The caller-owned frame slot the decoder hands back on a successful decode.
    const std::size_t frame_slot =
        align_up(static_cast<std::size_t>(memory.max_frame_buffer_size), kDirectAlignment);
    if (frame_slot == 0)
    {
        sys::log("[YTV] videodec2 sceVideodec2QueryDecoderMemoryInfo: no frame size");
        set_error(error, "sceVideodec2QueryDecoderMemoryInfo: no frame buffer size");
        close();
        return false;
    }
    if (!alloc_direct("frame", frame_slot, kProtectionGpu, limit, &session.frame_physical,
                      &session.frame_pool, reachable, error))
    {
        close();
        return false;
    }
    session.frame_pool_bytes = frame_slot;
    session.frame_slot_bytes = frame_slot;

    // 5. Create the decoder on that memory and put it in its initial state.
    void *decoder = nullptr;
    if (!call_step<CreateDecoderFn>("sceVideodec2CreateDecoder", kCreateDecoder, reachable, error,
                                    &config, &memory, &decoder))
    {
        close();
        return false;
    }
    session.decoder = decoder;
    if (!call_step<ResetFn>("sceVideodec2Reset", kReset, reachable, error, decoder))
    {
        close();
        return false;
    }

    sys::log("[YTV] videodec2 open: ready slot=%zu compute=%zu (imports %d/%d)",
             session.frame_slot_bytes, session.compute_bytes, reachable, kVideodecImportCount);
    return true;
}

bool HardwareDecoder::ready() const
{
    return m_session != nullptr && m_session->decoder != nullptr;
}

bool HardwareDecoder::decode(const std::uint8_t *data, std::size_t size, DecodedPicture *out,
                             std::string *error)
{
    if (m_session == nullptr || m_session->decoder == nullptr)
    {
        set_error(error, "decode: decoder is not open");
        return false;
    }
    if (data == nullptr || size == 0 || out == nullptr)
    {
        set_error(error, "decode: empty access unit");
        return false;
    }

    Session &session = *m_session;
    const int reachable = session.reachable;
    *out = DecodedPicture{};

    InputData input{};
    input.this_size = sizeof(InputData);
    input.au_data = data;
    input.au_size = size;

    FrameBuffer frame{};
    frame.this_size = sizeof(FrameBuffer);
    frame.frame_buffer = session.frame_pool;
    frame.frame_buffer_size = session.frame_slot_bytes;

    OutputInfo output{};
    output.this_size = sizeof(OutputInfo);

    if (!call_step<DecodeFn>("sceVideodec2Decode", kDecode, reachable, error, session.decoder,
                             &input, &frame, &output))
        return false;

    // At depth one a stream may hold its first picture back. docs/HARDWARE_DECODE.md
    // records exactly this for the tested H.264 streams (no picture from decode,
    // an immediate flush produced it); VP9 returned the picture straight away, so
    // this only runs when decode produced nothing.
    if (!output.valid)
    {
        if (!call_step<FlushFn>("sceVideodec2Flush", kFlush, reachable, error, session.decoder,
                                &frame, &output))
            return false;
    }

    if (!output.valid || output.error || output.picture_count == 0)
    {
        sys::log("[YTV] videodec2 sceVideodec2Decode: no picture (valid=%u error=%u pics=%u)",
                 static_cast<unsigned>(output.valid), static_cast<unsigned>(output.error),
                 static_cast<unsigned>(output.picture_count));
        set_error(error, "sceVideodec2Decode: no picture produced");
        return false;
    }

    // The returned address must be an exact member of the pool this session
    // handed to the decoder; anything else breaks the ownership contract the
    // research relies on for zero-copy presentation.
    const auto base = reinterpret_cast<std::uintptr_t>(session.frame_pool);
    const auto address = reinterpret_cast<std::uintptr_t>(output.frame_buffer);
    if (output.frame_buffer == nullptr || address < base ||
        address >= base + session.frame_pool_bytes)
    {
        sys::log("[YTV] videodec2 sceVideodec2Decode: picture outside the frame pool");
        set_error(error, "sceVideodec2Decode: picture outside the caller frame pool");
        return false;
    }
    if (static_cast<std::size_t>(output.frame_buffer_size) > session.frame_slot_bytes)
    {
        set_error(error, "sceVideodec2Decode: picture does not fit the frame slot");
        return false;
    }

    out->address = output.frame_buffer;
    out->width = static_cast<int>(output.frame_width);
    out->height = static_cast<int>(output.frame_height);
    out->pitch = static_cast<int>(output.frame_pitch_in_bytes != 0 ? output.frame_pitch_in_bytes
                                                                   : output.frame_pitch);
    out->format = output.frame_format;
    return true;
}

void HardwareDecoder::close()
{
    if (m_session == nullptr)
        return;

    Session &session = *m_session;
    std::string ignored;

    if (session.decoder != nullptr)
    {
        // Documented teardown: Flush/Reset -> DeleteDecoder. Nothing to drain
        // at depth one in a healthy session, and a failure here cannot stop the
        // release below.
        FrameBuffer frame{};
        frame.this_size = sizeof(FrameBuffer);
        frame.frame_buffer = session.frame_pool;
        frame.frame_buffer_size = session.frame_slot_bytes;
        OutputInfo output{};
        output.this_size = sizeof(OutputInfo);
        (void)call_step<FlushFn>("sceVideodec2Flush", kFlush, session.reachable, &ignored,
                                 session.decoder, &frame, &output);
        (void)call_step<ResetFn>("sceVideodec2Reset", kReset, session.reachable, &ignored,
                                 session.decoder);
        (void)call_step<DeleteDecoderFn>("sceVideodec2DeleteDecoder", kDeleteDecoder,
                                         session.reachable, &ignored, session.decoder);
        session.decoder = nullptr;
    }

    // Release in the reverse order of bring-up (docs/evo-pro/videodec2-abi.md §4):
    // frame pool, shared region, gpu region, CPU workspace, then the compute
    // queue and its memory last.
    if (session.frame_pool != nullptr)
    {
        (void)munmap(session.frame_pool, session.frame_pool_bytes);
        session.frame_pool = nullptr;
    }
    if (session.frame_physical != 0)
    {
        (void)sceKernelReleaseDirectMemory(session.frame_physical, session.frame_pool_bytes);
        session.frame_physical = 0;
    }
    if (session.cpu_gpu_mapping != nullptr)
    {
        (void)munmap(session.cpu_gpu_mapping, session.cpu_gpu_bytes);
        session.cpu_gpu_mapping = nullptr;
    }
    if (session.cpu_gpu_physical != 0)
    {
        (void)sceKernelReleaseDirectMemory(session.cpu_gpu_physical, session.cpu_gpu_bytes);
        session.cpu_gpu_physical = 0;
    }
    if (session.gpu_mapping != nullptr)
    {
        (void)munmap(session.gpu_mapping, session.gpu_bytes);
        session.gpu_mapping = nullptr;
    }
    if (session.gpu_physical != 0)
    {
        (void)sceKernelReleaseDirectMemory(session.gpu_physical, session.gpu_bytes);
        session.gpu_physical = 0;
    }
    if (session.cpu_mapping != nullptr)
    {
        (void)sceKernelReleaseFlexibleMemory(session.cpu_mapping, session.cpu_bytes);
        (void)munmap(session.cpu_mapping, session.cpu_bytes);
        session.cpu_mapping = nullptr;
    }
    if (session.compute_queue != nullptr)
    {
        (void)call_step<ReleaseComputeQueueFn>("sceVideodec2ReleaseComputeQueue",
                                               kReleaseComputeQueue, session.reachable, &ignored,
                                               session.compute_queue);
        session.compute_queue = nullptr;
    }
    if (session.compute_mapping != nullptr)
    {
        (void)munmap(session.compute_mapping, session.compute_bytes);
        session.compute_mapping = nullptr;
    }
    if (session.compute_physical != 0)
    {
        (void)sceKernelReleaseDirectMemory(session.compute_physical, session.compute_bytes);
        session.compute_physical = 0;
    }

    m_session.reset();
}

} // namespace hui::ps5
