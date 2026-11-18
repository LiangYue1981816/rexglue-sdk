/**
 * @file        graphics/native_frontend_snapshot.h
 * @brief       Device snapshot behind the draw packets the game rewrote into native draws
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::graphics::native_frontend {

// The game rewrites a DRAW_INDX or DRAW_INDX_2 packet in place into a type-3 NOP of the same length
// (at least 4 payload words): kPacketMagic, the low and high halves of a host pointer to a
// NativeDrawSnapshot, then padding.
constexpr uint32_t kPacketMagic = 0x5654414E;    // 'NATV' as the guest dword value
constexpr uint32_t kSnapshotMagic = 0x3353444E;  // 'NDS3'

// draw_opcode values
constexpr uint32_t kDrawOpcodeIndx = 0x22;   // PM4_DRAW_INDX
constexpr uint32_t kDrawOpcodeIndx2 = 0x36;  // PM4_DRAW_INDX_2

// Host memory, host byte order. The game defines the identical struct; keep the layout in sync.
struct NativeDrawSnapshot {
  uint32_t magic;
  uint32_t size;
  uint64_t sequence;
  uint32_t draw_words[4];    // draw packet payload words, host order (draw_word_count used)
  uint32_t surf[6];          // registers 0x2000-0x2005
  uint32_t prog[2];          // 0x2180-0x2181
  uint32_t rs[12];           // 0x2200-0x220B
  uint32_t vp[21];           // 0x2100-0x2114
  uint32_t scissor[2];       // 0x2081-0x2082
  uint32_t fetch[192];       // 0x4800-0x48BF
  const uint32_t* alu[128];  // each 16 dwords: registers 0x4000 + 16*i .. +15
  uint32_t window_offset;    // register 0x2080
  uint32_t g2280[21];        // 0x2280-0x2294
  uint32_t g2300[38];        // 0x2300-0x2325
  uint32_t g2380[8];         // 0x2380-0x2387
  uint32_t g2388[24];        // 0x2388-0x239F
  uint32_t bool_consts[40];  // 0x4900-0x4927: 8 BOOL and 32 LOOP constants
  uint32_t vs_address;       // physical guest address of the vertex shader microcode (B)
  uint32_t vs_dwords;
  uint32_t ps_address;  // pixel shader microcode (A); ps_dwords == 0: no pixel shader for this draw
  uint32_t ps_dwords;
  uint32_t flags;  // kSnapshotFlag*
  uint32_t reserved;
  uint32_t draw_opcode;      // kDrawOpcodeIndx or kDrawOpcodeIndx2: the packet the game rewrote
  uint32_t draw_word_count;  // payload words of draw_words in use, 1-4
};

// flags
// The game turned the library's state packets of this draw into NOPs.
constexpr uint32_t kSnapshotFlagSuppressed = 1u << 0;
// The game turned the library's shader packets of this draw into NOPs; load the snapshot's shaders.
constexpr uint32_t kSnapshotFlagShaders = 1u << 1;

static_assert(sizeof(void*) != 8 || offsetof(NativeDrawSnapshot, alu) == 976,
              "NativeDrawSnapshot layout must match the game's");
static_assert(sizeof(void*) != 8 || offsetof(NativeDrawSnapshot, window_offset) == 2000,
              "NativeDrawSnapshot layout must match the game's");
static_assert(sizeof(void*) != 8 || offsetof(NativeDrawSnapshot, draw_opcode) == 2552,
              "NativeDrawSnapshot layout must match the game's");
static_assert(sizeof(void*) != 8 || sizeof(NativeDrawSnapshot) == 2560,
              "NativeDrawSnapshot layout must match the game's");

}  // namespace rex::graphics::native_frontend
