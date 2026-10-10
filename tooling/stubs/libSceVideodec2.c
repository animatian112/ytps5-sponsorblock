/*
 * youtube-tv - Import stub for the console's media decoder.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The public PS5 payload SDK ships stubs for VideoOut, Pad, AudioOut and
 * friends, but not for libSceVideodec2 - the fixed-function decoder behind
 * PlayStation's own media apps. This file supplies the symbol names so the
 * linker can emit the console's NID imports for them (the toolchain derives
 * each NID from the name, see prospero-nid); tools/build-import-stubs.sh
 * compiles it into tooling/stubs/libSceVideodec2.so with the SONAME the
 * console's loader expects.
 *
 * WHAT IS PROVEN (see docs/HARDWARE_DECODE.md): the decoder exists, decodes
 * AVC/HEVC/VP9 into caller-owned direct memory and shares that surface with
 * AGC. WHAT IS NOT: the argument layouts. So nothing in this app calls these
 * functions yet - they are referenced only through a table of addresses
 * (src/platform/ps5/videodec_probe.cpp), which is enough for the loader to
 * resolve the imports and for a console run to show that the library is
 * reachable before any call shape is written.
 *
 * The names are the complete set published for the PS5 firmware's
 * libSceVideodec2 (a public symbol dump), grouped by the stage of the
 * pipeline they belong to. Each function has an empty body so the stub's
 * symbol table carries it; the bodies themselves never run, because the
 * console's own library answers the NID imports.
 */

typedef int sce_videodec2_result;

/* Library lifecycle. */
sce_videodec2_result sceVideodec2InitLibrary(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2TermLibrary(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2QueryLibraryInfo(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2QueryDecoderInfo(void)
{
    return 0; /* never executed: the console library binds the real code */
}

/* Decoder creation: the generic entry, the bid variant and the HEVC one the
 * separate HEVC entry point suggests. */
sce_videodec2_result sceVideodec2CreateDecoder(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2CreateDecoderBid(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2CreateHevcDecoder(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2DeleteDecoder(void)
{
    return 0; /* never executed: the console library binds the real code */
}

/* Configuration presets: one per codec this app cares about (VP9 first). */
sce_videodec2_result sceVideodec2PresetDecoderConfigInfo(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2PresetDecoderConfigInfoAvc(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2PresetDecoderConfigInfoHevc(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2PresetDecoderConfigInfoVp9(void)
{
    return 0; /* never executed: the console library binds the real code */
}

/* Memory: the app allocates the compressed-input and frame slots, the
 * decoder reports what it needs and maps them. */
sce_videodec2_result sceVideodec2QueryDecoderMemoryInfo(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2QueryHevcDecoderMemoryInfo(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2QueryComputeMemoryInfo(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2AllocateComputeQueue(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2ReleaseComputeQueue(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2MapDirectMemory(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2MapMemory(void)
{
    return 0; /* never executed: the console library binds the real code */
}

/* The pipeline: submit one access unit, drain it, read the picture info. */
sce_videodec2_result sceVideodec2Decode(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2Flush(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2Reset(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2GetPictureInfo(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2GetAvcPictureInfo(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2GetHevcPictureInfo(void)
{
    return 0; /* never executed: the console library binds the real code */
}
sce_videodec2_result sceVideodec2GetVp9PictureInfo(void)
{
    return 0; /* never executed: the console library binds the real code */
}
