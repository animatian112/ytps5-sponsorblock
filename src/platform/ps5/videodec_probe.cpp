// youtube-tv - Import probe for the console's media decoder.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoder has no public API documentation: the research that proved it
// (docs/HARDWARE_DECODE.md) documents behaviour, not argument layouts. So this
// app carries the imports and nothing else: every symbol is referenced through
// the table below, which makes the linker emit the console's NID imports, and
// the startup log says how many were bound. The first console run answers
// whether the library is reachable for a homebrew title before a single call
// shape has to be written.
//
// Nothing here calls the functions. When the pipeline is implemented, this
// file becomes src/platform/ps5/videodec.cpp with real signatures.

#include "platform/ps5/system.hpp"

#include <cstddef>

namespace hui::ps5
{

namespace
{

// Signatures are deliberately absent: only the addresses matter here.
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
// picture info. Kept as one table so a console log can count what the loader
// resolved (a missing library leaves these at null on the console).
// volatile: an optimizer that folds "the address of an extern function is
// non-null" would delete every reference, and with them the imports.
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

constexpr std::size_t kDecoderImportCount = sizeof(kDecoderImports) / sizeof(kDecoderImports[0]);

} // namespace

// Logs how many decoder entry points the process carries and how many of them
// the loader bound. Called once at startup: on the console it is the first
// answer to "is libSceVideodec2 reachable from a homebrew title?".
std::size_t report_decoder_imports()
{
    std::size_t bound = 0;
    for (std::size_t index = 0; index < kDecoderImportCount; ++index)
    {
        if (kDecoderImports[index] != nullptr)
            ++bound;
    }
    sys::log("[YTV] videodec2 imports: %zu/%zu reachable", bound, kDecoderImportCount);
    return bound;
}

} // namespace hui::ps5
