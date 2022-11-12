/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2022 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blink/address.h"
#include "blink/alu.h"
#include "blink/assert.h"
#include "blink/bitscan.h"
#include "blink/case.h"
#include "blink/endian.h"
#include "blink/flags.h"
#include "blink/fpu.h"
#include "blink/log.h"
#include "blink/machine.h"
#include "blink/macros.h"
#include "blink/memory.h"
#include "blink/modrm.h"
#include "blink/random.h"
#include "blink/real.h"
#include "blink/sse.h"
#include "blink/ssefloat.h"
#include "blink/ssemov.h"
#include "blink/string.h"
#include "blink/swap.h"
#include "blink/time.h"
#include "blink/util.h"

#define OpLfence    OpNoop
#define OpMfence    OpNoop
#define OpSfence    OpNoop
#define OpClflush   OpNoop
#define OpHintNopEv OpNoop

// GetSegment

typedef void (*nexgen32e_f)(struct Machine *, u32);

static u64 ReadRegister(u32 rde, u8 p[8]) {
  if (Rexw(rde)) {
    return Read64(p);
  } else if (!Osz(rde)) {
    return Read32(p);
  } else {
    return Read16(p);
  }
}

static i64 ReadRegisterSigned(u32 rde, u8 p[8]) {
  if (Rexw(rde)) {
    return (i64)Read64(p);
  } else if (!Osz(rde)) {
    return (i32)Read32(p);
  } else {
    return (i16)Read16(p);
  }
}

static void WriteRegister(u32 rde, u8 p[8], u64 x) {
  if (Rexw(rde)) {
    Write64(p, x);
  } else if (!Osz(rde)) {
    Write64(p, x & 0xffffffff);
  } else {
    Write16(p, x);
  }
}

static u64 ReadMemory(u32 rde, u8 p[8]) {
  u64 x;
  if (Rexw(rde)) {
#if LONG_BIT == 64
    if (!((intptr_t)p & 7)) {
      x = atomic_load_explicit((atomic_ulong *)p, memory_order_acquire);
      x = SWAP64LE(x);
    } else {
      x = Read64(p);
    }
#else
    x = Read64(p);
#endif
  } else if (!Osz(rde)) {
    if (!((intptr_t)p & 3)) {
      x = atomic_load_explicit((atomic_uint *)p, memory_order_acquire);
      x = SWAP32LE(x);
    } else {
      x = Read32(p);
    }
  } else {
    x = Read16(p);
  }
  return x;
}

static u64 ReadMemorySigned(u32 rde, u8 p[8]) {
  u64 x;
  if (Rexw(rde)) {
#if LONG_BIT == 64
    if (!((intptr_t)p & 7)) {
      x = atomic_load_explicit((atomic_ulong *)p, memory_order_acquire);
      x = SWAP64LE(x);
    } else {
      x = Read64(p);
    }
#else
    x = Read64(p);
#endif
  } else if (!Osz(rde)) {
    if (!((intptr_t)p & 3)) {
      x = atomic_load_explicit((atomic_uint *)p, memory_order_acquire);
      x = SWAP32LE(x);
    } else {
      x = Read32(p);
    }
    x = (i32)x;
  } else {
    x = (i16)Read16(p);
  }
  return x;
}

static void WriteMemory(u32 rde, u8 p[8], u64 x) {
  if (Rexw(rde)) {
#if LONG_BIT == 64
    if (!((intptr_t)p & 7)) {
      atomic_store_explicit((atomic_ulong *)p, SWAP64LE(x),
                            memory_order_release);
    } else {
      Write64(p, x);
    }
#else
    Write64(p, x);
#endif
  } else if (!Osz(rde)) {
    if (!((intptr_t)p & 3)) {
      atomic_store_explicit((atomic_uint *)p, SWAP32LE(x),
                            memory_order_release);
    } else {
      Write32(p, x);
    }
  } else {
    Write16(p, x);
  }
}

static void WriteRegisterOrMemory(u32 rde, u8 p[8], u64 x) {
  if (IsModrmRegister(rde)) {
    WriteRegister(rde, p, x);
  } else {
    WriteMemory(rde, p, x);
  }
}

static bool IsParity(struct Machine *m) {
  return GetFlag(m->flags, FLAGS_PF);
}

static bool IsBelowOrEqual(struct Machine *m) {
  return GetFlag(m->flags, FLAGS_CF) || GetFlag(m->flags, FLAGS_ZF);
}

static bool IsAbove(struct Machine *m) {
  return !GetFlag(m->flags, FLAGS_CF) && !GetFlag(m->flags, FLAGS_ZF);
}

static bool IsLess(struct Machine *m) {
  return GetFlag(m->flags, FLAGS_SF) != GetFlag(m->flags, FLAGS_OF);
}

static bool IsGreaterOrEqual(struct Machine *m) {
  return GetFlag(m->flags, FLAGS_SF) == GetFlag(m->flags, FLAGS_OF);
}

static bool IsLessOrEqual(struct Machine *m) {
  return GetFlag(m->flags, FLAGS_ZF) ||
         (GetFlag(m->flags, FLAGS_SF) != GetFlag(m->flags, FLAGS_OF));
}

static bool IsGreater(struct Machine *m) {
  return !GetFlag(m->flags, FLAGS_ZF) &&
         (GetFlag(m->flags, FLAGS_SF) == GetFlag(m->flags, FLAGS_OF));
}

static void OpNoop(struct Machine *m, u32 rde) {
}

static void OpCmc(struct Machine *m, u32 rde) {
  m->flags = SetFlag(m->flags, FLAGS_CF, !GetFlag(m->flags, FLAGS_CF));
}

static void OpClc(struct Machine *m, u32 rde) {
  m->flags = SetFlag(m->flags, FLAGS_CF, false);
}

static void OpStc(struct Machine *m, u32 rde) {
  m->flags = SetFlag(m->flags, FLAGS_CF, true);
}

static void OpCli(struct Machine *m, u32 rde) {
  m->flags = SetFlag(m->flags, FLAGS_IF, false);
}

static void OpSti(struct Machine *m, u32 rde) {
  m->flags = SetFlag(m->flags, FLAGS_IF, true);
}

static void OpCld(struct Machine *m, u32 rde) {
  m->flags = SetFlag(m->flags, FLAGS_DF, false);
}

static void OpStd(struct Machine *m, u32 rde) {
  m->flags = SetFlag(m->flags, FLAGS_DF, true);
}

static void OpPushf(struct Machine *m, u32 rde) {
  Push(m, rde, ExportFlags(m->flags) & 0xFCFFFF);
}

static void OpPopf(struct Machine *m, u32 rde) {
  if (!Osz(rde)) {
    ImportFlags(m, Pop(m, rde, 0));
  } else {
    ImportFlags(m, (m->flags & ~0xffff) | Pop(m, rde, 0));
  }
}

static void OpLahf(struct Machine *m, u32 rde) {
  Write8(m->ax + 1, ExportFlags(m->flags));
}

static void OpSahf(struct Machine *m, u32 rde) {
  ImportFlags(m, (m->flags & ~0xff) | m->ax[1]);
}

static void OpLeaGvqpM(struct Machine *m, u32 rde) {
  WriteRegister(rde, RegRexrReg(m, rde), LoadEffectiveAddress(m, rde).addr);
}

static void OpPushSeg(struct Machine *m, u32 rde) {
  u8 seg = (m->xedd->op.opcode & 070) >> 3;
  Push(m, rde, Read64(GetSegment(m, rde, seg)) >> 4);
}

static void OpPopSeg(struct Machine *m, u32 rde) {
  u8 seg = (m->xedd->op.opcode & 070) >> 3;
  Write64(GetSegment(m, rde, seg), Pop(m, rde, 0) << 4);
}

static void OpMovEvqpSw(struct Machine *m, u32 rde) {
  WriteRegisterOrMemory(rde, GetModrmRegisterWordPointerWriteOszRexw(m, rde),
                        Read64(GetSegment(m, rde, ModrmReg(rde))) >> 4);
}

static int GetDescriptor(struct Machine *m, int selector,
                         u64 *out_descriptor) {
  unassert(m->system->gdt_base + m->system->gdt_limit <=
           GetRealMemorySize(m->system));
  selector &= -8;
  if (8 <= selector && selector + 8 <= m->system->gdt_limit) {
    SetReadAddr(m, m->system->gdt_base + selector, 8);
    *out_descriptor =
        Read64(m->system->real.p + m->system->gdt_base + selector);
    return 0;
  } else {
    return -1;
  }
}

static u64 GetDescriptorBase(u64 d) {
  return (d & 0xff00000000000000) >> 32 | (d & 0x000000ffffff0000) >> 16;
}

static u64 GetDescriptorLimit(u64 d) {
  return (d & 0x000f000000000000) >> 32 | (d & 0xffff);
}

static int GetDescriptorMode(u64 d) {
  u8 kMode[] = {
      XED_MACHINE_MODE_REAL,
      XED_MACHINE_MODE_LONG_64,
      XED_MACHINE_MODE_LEGACY_32,
      XED_MACHINE_MODE_LONG_64,
  };
  return kMode[(d & 0x0060000000000000) >> 53];
}

static bool IsProtectedMode(struct Machine *m) {
  return m->system->cr0 & 1;
}

static void OpMovSwEvqp(struct Machine *m, u32 rde) {
  u64 x, d;
  x = ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(m, rde));
  if (!IsProtectedMode(m)) {
    x <<= 4;
  } else if (GetDescriptor(m, x, &d) != -1) {
    x = GetDescriptorBase(d);
  } else {
    ThrowProtectionFault(m);
  }
  Write64(GetSegment(m, rde, ModrmReg(rde)), x);
}

static void OpLsl(struct Machine *m, u32 rde) {
  u64 descriptor;
  if (GetDescriptor(m, Read16(GetModrmRegisterWordPointerRead2(m, rde)),
                    &descriptor) != -1) {
    WriteRegister(rde, RegRexrReg(m, rde), GetDescriptorLimit(descriptor));
    m->flags = SetFlag(m->flags, FLAGS_ZF, true);
  } else {
    m->flags = SetFlag(m->flags, FLAGS_ZF, false);
  }
}

static void ChangeMachineMode(struct Machine *m, int mode) {
  if (mode == m->mode) return;
  ResetInstructionCache(m);
  m->mode = mode;
}

static void OpJmpf(struct Machine *m, u32 rde) {
  u64 descriptor;
  if (!IsProtectedMode(m)) {
    Write64(m->cs, m->xedd->op.uimm0 << 4);
    m->ip = m->xedd->op.disp;
  } else if (GetDescriptor(m, m->xedd->op.uimm0, &descriptor) != -1) {
    Write64(m->cs, GetDescriptorBase(descriptor));
    m->ip = m->xedd->op.disp;
    ChangeMachineMode(m, GetDescriptorMode(descriptor));
  } else {
    ThrowProtectionFault(m);
  }
  if (m->system->onlongbranch) {
    m->system->onlongbranch(m);
  }
}

static void OpXlatAlBbb(struct Machine *m, u32 rde) {
  u64 v;
  v = MaskAddress(Eamode(rde), Read64(m->bx) + Read8(m->ax));
  v = DataSegment(m, rde, v);
  SetReadAddr(m, v, 1);
  Write8(m->ax, Read8(ResolveAddress(m, v)));
}

static void WriteEaxAx(struct Machine *m, u32 rde, u32 x) {
  if (!Osz(rde)) {
    Write64(m->ax, x);
  } else {
    Write16(m->ax, x);
  }
}

static u32 ReadEaxAx(struct Machine *m, u32 rde) {
  if (!Osz(rde)) {
    return Read32(m->ax);
  } else {
    return Read16(m->ax);
  }
}

static void OpInAlImm(struct Machine *m, u32 rde) {
  Write8(m->ax, OpIn(m, m->xedd->op.uimm0));
}

static void OpInAxImm(struct Machine *m, u32 rde) {
  WriteEaxAx(m, rde, OpIn(m, m->xedd->op.uimm0));
}

static void OpInAlDx(struct Machine *m, u32 rde) {
  Write8(m->ax, OpIn(m, Read16(m->dx)));
}

static void OpInAxDx(struct Machine *m, u32 rde) {
  WriteEaxAx(m, rde, OpIn(m, Read16(m->dx)));
}

static void OpOutImmAl(struct Machine *m, u32 rde) {
  OpOut(m, m->xedd->op.uimm0, Read8(m->ax));
}

static void OpOutImmAx(struct Machine *m, u32 rde) {
  OpOut(m, m->xedd->op.uimm0, ReadEaxAx(m, rde));
}

static void OpOutDxAl(struct Machine *m, u32 rde) {
  OpOut(m, Read16(m->dx), Read8(m->ax));
}

static void OpOutDxAx(struct Machine *m, u32 rde) {
  OpOut(m, Read16(m->dx), ReadEaxAx(m, rde));
}

static void OpXchgZvqp(struct Machine *m, u32 rde) {
  u64 x, y;
  x = Read64(m->ax);
  y = Read64(RegRexbSrm(m, rde));
  WriteRegister(rde, m->ax, y);
  WriteRegister(rde, RegRexbSrm(m, rde), x);
}

static void Op1c7(struct Machine *m, u32 rde) {
  bool ismem;
  ismem = !IsModrmRegister(rde);
  switch (ModrmReg(rde)) {
    case 6:
      if (!ismem) {
        OpRdrand(m, rde);
      } else {
        OpUd(m, rde);
      }
      break;
    case 7:
      if (!ismem) {
        if (m->xedd->op.rep == 3) {
          OpRdpid(m, rde);
        } else {
          OpRdseed(m, rde);
        }
      } else {
        OpUd(m, rde);
      }
      break;
    default:
      OpUd(m, rde);
  }
}

static u64 Bts(u64 x, u64 y) {
  return x | y;
}

static u64 Btr(u64 x, u64 y) {
  return x & ~y;
}

static u64 Btc(u64 x, u64 y) {
  return (x & ~y) | (~x & y);
}

static void OpBit(struct Machine *m, u32 rde) {
  int op;
  u8 *p;
  unsigned bit;
  i64 disp;
  u64 v, x, y, z;
  u8 w, W[2][2] = {{2, 3}, {1, 3}};
  unassert(!Lock(rde));
  w = W[Osz(rde)][Rexw(rde)];
  if (m->xedd->op.opcode == 0xBA) {
    op = ModrmReg(rde);
    bit = m->xedd->op.uimm0 & ((8 << w) - 1);
    disp = 0;
  } else {
    op = (m->xedd->op.opcode & 070) >> 3;
    disp = ReadRegisterSigned(rde, RegRexrReg(m, rde));
    bit = disp & ((8 << w) - 1);
    disp &= -(8 << w);
    disp >>= 3;
  }
  if (IsModrmRegister(rde)) {
    p = RegRexbRm(m, rde);
  } else {
    v = MaskAddress(Eamode(rde), ComputeAddress(m, rde) + disp);
    p = (u8 *)ReserveAddress(m, v, 1 << w);
    if (op == 4) {
      SetReadAddr(m, v, 1 << w);
    } else {
      SetWriteAddr(m, v, 1 << w);
    }
  }
  y = 1;
  y <<= bit;
  x = ReadMemory(rde, p);
  m->flags = SetFlag(m->flags, FLAGS_CF, !!(y & x));
  switch (op) {
    case 4:
      return;
    case 5:
      z = Bts(x, y);
      break;
    case 6:
      z = Btr(x, y);
      break;
    case 7:
      z = Btc(x, y);
      break;
    default:
      OpUd(m, rde);
  }
  WriteRegisterOrMemory(rde, p, z);
}

static void OpSax(struct Machine *m, u32 rde) {
  if (Rexw(rde)) {
    Write64(m->ax, (i32)Read32(m->ax));
  } else if (!Osz(rde)) {
    Write64(m->ax, (u32)(i16)Read16(m->ax));
  } else {
    Write16(m->ax, (i8)Read8(m->ax));
  }
}

static void OpConvert(struct Machine *m, u32 rde) {
  if (Rexw(rde)) {
    Write64(m->dx, Read64(m->ax) & 0x8000000000000000 ? 0xffffffffffffffff : 0);
  } else if (!Osz(rde)) {
    Write64(m->dx, Read32(m->ax) & 0x80000000 ? 0xffffffff : 0);
  } else {
    Write16(m->dx, Read16(m->ax) & 0x8000 ? 0xffff : 0);
  }
}

static void OpBswapZvqp(struct Machine *m, u32 rde) {
  u64 x;
  x = Read64(RegRexbSrm(m, rde));
  if (Rexw(rde)) {
    Write64(
        RegRexbSrm(m, rde),
        ((x & 0xff00000000000000) >> 070 | (x & 0x00000000000000ff) << 070 |
         (x & 0x00ff000000000000) >> 050 | (x & 0x000000000000ff00) << 050 |
         (x & 0x0000ff0000000000) >> 030 | (x & 0x0000000000ff0000) << 030 |
         (x & 0x000000ff00000000) >> 010 | (x & 0x00000000ff000000) << 010));
  } else if (!Osz(rde)) {
    Write64(RegRexbSrm(m, rde),
            ((x & 0xff000000) >> 030 | (x & 0x000000ff) << 030 |
             (x & 0x00ff0000) >> 010 | (x & 0x0000ff00) << 010));
  } else {
    Write16(RegRexbSrm(m, rde), (x & 0x00ff) << 010 | (x & 0xff00) << 010);
  }
}

static void OpMovEbIb(struct Machine *m, u32 rde) {
  Write8(GetModrmRegisterBytePointerWrite(m, rde), m->xedd->op.uimm0);
}

static void OpMovAlOb(struct Machine *m, u32 rde) {
  i64 addr;
  addr = AddressOb(m, rde);
  SetWriteAddr(m, addr, 1);
  Write8(m->ax, Read8(ResolveAddress(m, addr)));
}

static void OpMovObAl(struct Machine *m, u32 rde) {
  i64 addr;
  addr = AddressOb(m, rde);
  SetReadAddr(m, addr, 1);
  Write8((u8 *)ResolveAddress(m, addr), Read8(m->ax));
}

static void OpMovRaxOvqp(struct Machine *m, u32 rde) {
  u64 v;
  v = DataSegment(m, rde, m->xedd->op.disp);
  SetReadAddr(m, v, 1 << RegLog2(rde));
  WriteRegister(rde, m->ax, ReadMemory(rde, (u8 *)ResolveAddress(m, v)));
}

static void OpMovOvqpRax(struct Machine *m, u32 rde) {
  u64 v;
  v = DataSegment(m, rde, m->xedd->op.disp);
  SetWriteAddr(m, v, 1 << RegLog2(rde));
  WriteMemory(rde, (u8 *)ResolveAddress(m, v), Read64(m->ax));
}

static void OpMovEbGb(struct Machine *m, u32 rde) {
  memcpy(GetModrmRegisterBytePointerWrite(m, rde), ByteRexrReg(m, rde), 1);
}

static void OpMovGbEb(struct Machine *m, u32 rde) {
  memcpy(ByteRexrReg(m, rde), GetModrmRegisterBytePointerRead(m, rde), 1);
}

static void OpMovZbIb(struct Machine *m, u32 rde) {
  Write8(ByteRexbSrm(m, rde), m->xedd->op.uimm0);
}

static void OpMovZvqpIvqp(struct Machine *m, u32 rde) {
  WriteRegister(rde, RegRexbSrm(m, rde), m->xedd->op.uimm0);
}

static void OpIncZv(struct Machine *m, u32 rde) {
  if (!Osz(rde)) {
    Write32(RegSrm(m, rde), Inc32(Read32(RegSrm(m, rde)), 0, &m->flags));
  } else {
    Write16(RegSrm(m, rde), Inc16(Read16(RegSrm(m, rde)), 0, &m->flags));
  }
}

static void OpDecZv(struct Machine *m, u32 rde) {
  if (!Osz(rde)) {
    Write32(RegSrm(m, rde), Dec32(Read32(RegSrm(m, rde)), 0, &m->flags));
  } else {
    Write16(RegSrm(m, rde), Dec16(Read16(RegSrm(m, rde)), 0, &m->flags));
  }
}

static void OpMovEvqpIvds(struct Machine *m, u32 rde) {
  WriteRegisterOrMemory(rde, GetModrmRegisterWordPointerWriteOszRexw(m, rde),
                        m->xedd->op.uimm0);
}

static void OpMovEvqpGvqp(struct Machine *m, u32 rde) {
  WriteRegisterOrMemory(rde, GetModrmRegisterWordPointerWriteOszRexw(m, rde),
                        ReadMemory(rde, RegRexrReg(m, rde)));
}

static void OpMovzbGvqpEb(struct Machine *m, u32 rde) {
  WriteRegister(rde, RegRexrReg(m, rde),
                Read8(GetModrmRegisterBytePointerRead(m, rde)));
}

static void OpMovzwGvqpEw(struct Machine *m, u32 rde) {
  WriteRegister(rde, RegRexrReg(m, rde),
                Read16(GetModrmRegisterWordPointerRead2(m, rde)));
}

static void OpMovsbGvqpEb(struct Machine *m, u32 rde) {
  WriteRegister(rde, RegRexrReg(m, rde),
                (i8)Read8(GetModrmRegisterBytePointerRead(m, rde)));
}

static void OpMovswGvqpEw(struct Machine *m, u32 rde) {
  WriteRegister(rde, RegRexrReg(m, rde),
                (i16)Read16(GetModrmRegisterWordPointerRead2(m, rde)));
}

static void OpMovsxdGdqpEd(struct Machine *m, u32 rde) {
  Write64(RegRexrReg(m, rde),
          (i32)Read32(GetModrmRegisterWordPointerRead4(m, rde)));
}

static void AlubRo(struct Machine *m, u32 rde, aluop_f op) {
  op(Read8(GetModrmRegisterBytePointerRead(m, rde)), Read8(ByteRexrReg(m, rde)),
     &m->flags);
}

static void OpAlubCmp(struct Machine *m, u32 rde) {
  AlubRo(m, rde, Sub8);
}

static void OpAlubTest(struct Machine *m, u32 rde) {
  AlubRo(m, rde, And8);
}

static void AlubFlip(struct Machine *m, u32 rde, aluop_f op) {
  Write8(ByteRexrReg(m, rde),
         op(Read8(ByteRexrReg(m, rde)),
            Read8(GetModrmRegisterBytePointerRead(m, rde)), &m->flags));
}

static void OpAlubFlipAdd(struct Machine *m, u32 rde) {
  AlubFlip(m, rde, Add8);
}

static void OpAlubFlipOr(struct Machine *m, u32 rde) {
  AlubFlip(m, rde, Or8);
}

static void OpAlubFlipAdc(struct Machine *m, u32 rde) {
  AlubFlip(m, rde, Adc8);
}

static void OpAlubFlipSbb(struct Machine *m, u32 rde) {
  AlubFlip(m, rde, Sbb8);
}

static void OpAlubFlipAnd(struct Machine *m, u32 rde) {
  AlubFlip(m, rde, And8);
}

static void OpAlubFlipSub(struct Machine *m, u32 rde) {
  AlubFlip(m, rde, Sub8);
}

static void OpAlubFlipXor(struct Machine *m, u32 rde) {
  AlubFlip(m, rde, Xor8);
}

static void AlubFlipRo(struct Machine *m, u32 rde, aluop_f op) {
  op(Read8(ByteRexrReg(m, rde)), Read8(GetModrmRegisterBytePointerRead(m, rde)),
     &m->flags);
}

static void OpAlubFlipCmp(struct Machine *m, u32 rde) {
  AlubFlipRo(m, rde, Sub8);
}

static void Alubi(struct Machine *m, u32 rde, aluop_f op) {
  u8 *a = GetModrmRegisterBytePointerWrite(m, rde);
  Write8(a, op(Read8(a), m->xedd->op.uimm0, &m->flags));
}

static void AlubiRo(struct Machine *m, u32 rde, aluop_f op) {
  op(atomic_load_explicit(
         (atomic_uchar *)GetModrmRegisterBytePointerRead(m, rde),
         memory_order_acquire),
     m->xedd->op.uimm0, &m->flags);
}

static void OpAlubiTest(struct Machine *m, u32 rde) {
  AlubiRo(m, rde, And8);
}

static void OpAlubiReg(struct Machine *m, u32 rde) {
  if (ModrmReg(rde) == ALU_CMP) {
    AlubiRo(m, rde, kAlu[ModrmReg(rde)][0]);
  } else {
    Alubi(m, rde, kAlu[ModrmReg(rde)][0]);
  }
}

static void AluwRo(struct Machine *m, u32 rde, const aluop_f ops[4]) {
  ops[RegLog2(rde)](
      ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(m, rde)),
      Read64(RegRexrReg(m, rde)), &m->flags);
}

static void OpAluwCmp(struct Machine *m, u32 rde) {
  AluwRo(m, rde, kAlu[ALU_SUB]);
}

static void OpAluwTest(struct Machine *m, u32 rde) {
  AluwRo(m, rde, kAlu[ALU_AND]);
}

static void OpAluwFlip(struct Machine *m, u32 rde) {
  WriteRegister(
      rde, RegRexrReg(m, rde),
      kAlu[(m->xedd->op.opcode & 070) >> 3][RegLog2(rde)](
          Read64(RegRexrReg(m, rde)),
          ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(m, rde)),
          &m->flags));
}

static void AluwFlipRo(struct Machine *m, u32 rde, const aluop_f ops[4]) {
  ops[RegLog2(rde)](
      Read64(RegRexrReg(m, rde)),
      ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(m, rde)),
      &m->flags);
}

static void OpAluwFlipCmp(struct Machine *m, u32 rde) {
  AluwFlipRo(m, rde, kAlu[ALU_SUB]);
}

static void Aluwi(struct Machine *m, u32 rde, const aluop_f ops[4]) {
  u8 *a;
  a = GetModrmRegisterWordPointerWriteOszRexw(m, rde);
  WriteRegisterOrMemory(
      rde, a,
      ops[RegLog2(rde)](ReadMemory(rde, a), m->xedd->op.uimm0, &m->flags));
}

static void AluwiRo(struct Machine *m, u32 rde, const aluop_f ops[4]) {
  ops[RegLog2(rde)](
      ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(m, rde)),
      m->xedd->op.uimm0, &m->flags);
}

static void OpAluwiReg(struct Machine *m, u32 rde) {
  if (ModrmReg(rde) == ALU_CMP) {
    AluwiRo(m, rde, kAlu[ModrmReg(rde)]);
  } else {
    Aluwi(m, rde, kAlu[ModrmReg(rde)]);
  }
}

static void AluAlIb(struct Machine *m, aluop_f op) {
  Write8(m->ax, op(Read8(m->ax), m->xedd->op.uimm0, &m->flags));
}

static void OpAluAlIbAdd(struct Machine *m, u32 rde) {
  AluAlIb(m, Add8);
}

static void OpAluAlIbOr(struct Machine *m, u32 rde) {
  AluAlIb(m, Or8);
}

static void OpAluAlIbAdc(struct Machine *m, u32 rde) {
  AluAlIb(m, Adc8);
}

static void OpAluAlIbSbb(struct Machine *m, u32 rde) {
  AluAlIb(m, Sbb8);
}

static void OpAluAlIbAnd(struct Machine *m, u32 rde) {
  AluAlIb(m, And8);
}

static void OpAluAlIbSub(struct Machine *m, u32 rde) {
  AluAlIb(m, Sub8);
}

static void OpAluAlIbXor(struct Machine *m, u32 rde) {
  AluAlIb(m, Xor8);
}

static void OpAluRaxIvds(struct Machine *m, u32 rde) {
  WriteRegister(rde, m->ax,
                kAlu[(m->xedd->op.opcode & 070) >> 3][RegLog2(rde)](
                    ReadRegister(rde, m->ax), m->xedd->op.uimm0, &m->flags));
}

static void OpCmpAlIb(struct Machine *m, u32 rde) {
  Sub8(Read8(m->ax), m->xedd->op.uimm0, &m->flags);
}

static void OpCmpRaxIvds(struct Machine *m, u32 rde) {
  kAlu[ALU_SUB][RegLog2(rde)](ReadRegister(rde, m->ax), m->xedd->op.uimm0,
                              &m->flags);
}

static void OpTestAlIb(struct Machine *m, u32 rde) {
  And8(Read8(m->ax), m->xedd->op.uimm0, &m->flags);
}

static void OpTestRaxIvds(struct Machine *m, u32 rde) {
  kAlu[ALU_AND][RegLog2(rde)](ReadRegister(rde, m->ax), m->xedd->op.uimm0,
                              &m->flags);
}

static void Bsuwi(struct Machine *m, u32 rde, u64 y) {
  u8 *p;
  p = GetModrmRegisterWordPointerWriteOszRexw(m, rde);
  WriteRegisterOrMemory(
      rde, p,
      kBsu[ModrmReg(rde)][RegLog2(rde)](ReadMemory(rde, p), y, &m->flags));
}

static void OpBsuwi1(struct Machine *m, u32 rde) {
  Bsuwi(m, rde, 1);
}

static void OpBsuwiCl(struct Machine *m, u32 rde) {
  Bsuwi(m, rde, Read8(m->cx));
}

static void OpBsuwiImm(struct Machine *m, u32 rde) {
  Bsuwi(m, rde, m->xedd->op.uimm0);
}

static void Bsubi(struct Machine *m, u32 rde, u64 y) {
  u8 *a = GetModrmRegisterBytePointerWrite(m, rde);
  Write8(a, kBsu[ModrmReg(rde)][RegLog2(rde)](Read8(a), y, &m->flags));
}

static void OpBsubi1(struct Machine *m, u32 rde) {
  Bsubi(m, rde, 1);
}

static void OpBsubiCl(struct Machine *m, u32 rde) {
  Bsubi(m, rde, Read8(m->cx));
}

static void OpBsubiImm(struct Machine *m, u32 rde) {
  Bsubi(m, rde, m->xedd->op.uimm0);
}

static void OpPushImm(struct Machine *m, u32 rde) {
  Push(m, rde, m->xedd->op.uimm0);
}

static void Interrupt(struct Machine *m, u32 rde, int i) {
  HaltMachine(m, i);
}

static void OpInterruptImm(struct Machine *m, u32 rde) {
  Interrupt(m, rde, m->xedd->op.uimm0);
}

static void OpInterrupt1(struct Machine *m, u32 rde) {
  Interrupt(m, rde, 1);
}

static void OpInterrupt3(struct Machine *m, u32 rde) {
  Interrupt(m, rde, 3);
}

static void OpJmp(struct Machine *m, u32 rde) {
  m->ip += m->xedd->op.disp;
}

static void OpJe(struct Machine *m, u32 rde) {
  if (GetFlag(m->flags, FLAGS_ZF)) {
    OpJmp(m, rde);
  }
}

static void OpJne(struct Machine *m, u32 rde) {
  if (!GetFlag(m->flags, FLAGS_ZF)) {
    OpJmp(m, rde);
  }
}

static void OpJb(struct Machine *m, u32 rde) {
  if (GetFlag(m->flags, FLAGS_CF)) {
    OpJmp(m, rde);
  }
}

static void OpJbe(struct Machine *m, u32 rde) {
  if (IsBelowOrEqual(m)) {
    OpJmp(m, rde);
  }
}

static void OpJo(struct Machine *m, u32 rde) {
  if (GetFlag(m->flags, FLAGS_OF)) {
    OpJmp(m, rde);
  }
}

static void OpJno(struct Machine *m, u32 rde) {
  if (!GetFlag(m->flags, FLAGS_OF)) {
    OpJmp(m, rde);
  }
}

static void OpJae(struct Machine *m, u32 rde) {
  if (!GetFlag(m->flags, FLAGS_CF)) {
    OpJmp(m, rde);
  }
}

static void OpJa(struct Machine *m, u32 rde) {
  if (IsAbove(m)) {
    OpJmp(m, rde);
  }
}

static void OpJs(struct Machine *m, u32 rde) {
  if (GetFlag(m->flags, FLAGS_SF)) {
    OpJmp(m, rde);
  }
}

static void OpJns(struct Machine *m, u32 rde) {
  if (!GetFlag(m->flags, FLAGS_SF)) {
    OpJmp(m, rde);
  }
}

static void OpJp(struct Machine *m, u32 rde) {
  if (IsParity(m)) {
    OpJmp(m, rde);
  }
}

static void OpJnp(struct Machine *m, u32 rde) {
  if (!IsParity(m)) {
    OpJmp(m, rde);
  }
}

static void OpJl(struct Machine *m, u32 rde) {
  if (IsLess(m)) {
    OpJmp(m, rde);
  }
}

static void OpJge(struct Machine *m, u32 rde) {
  if (IsGreaterOrEqual(m)) {
    OpJmp(m, rde);
  }
}

static void OpJle(struct Machine *m, u32 rde) {
  if (IsLessOrEqual(m)) {
    OpJmp(m, rde);
  }
}

static void OpJg(struct Machine *m, u32 rde) {
  if (IsGreater(m)) {
    OpJmp(m, rde);
  }
}

static void OpMovGvqpEvqp(struct Machine *m, u32 rde) {
  WriteRegister(
      rde, RegRexrReg(m, rde),
      ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(m, rde)));
}

static void OpCmovo(struct Machine *m, u32 rde) {
  if (GetFlag(m->flags, FLAGS_OF)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovno(struct Machine *m, u32 rde) {
  if (!GetFlag(m->flags, FLAGS_OF)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovb(struct Machine *m, u32 rde) {
  if (GetFlag(m->flags, FLAGS_CF)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovae(struct Machine *m, u32 rde) {
  if (!GetFlag(m->flags, FLAGS_CF)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmove(struct Machine *m, u32 rde) {
  if (GetFlag(m->flags, FLAGS_ZF)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovne(struct Machine *m, u32 rde) {
  if (!GetFlag(m->flags, FLAGS_ZF)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovbe(struct Machine *m, u32 rde) {
  if (IsBelowOrEqual(m)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmova(struct Machine *m, u32 rde) {
  if (IsAbove(m)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovs(struct Machine *m, u32 rde) {
  if (GetFlag(m->flags, FLAGS_SF)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovns(struct Machine *m, u32 rde) {
  if (!GetFlag(m->flags, FLAGS_SF)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovp(struct Machine *m, u32 rde) {
  if (IsParity(m)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovnp(struct Machine *m, u32 rde) {
  if (!IsParity(m)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovl(struct Machine *m, u32 rde) {
  if (IsLess(m)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovge(struct Machine *m, u32 rde) {
  if (IsGreaterOrEqual(m)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovle(struct Machine *m, u32 rde) {
  if (IsLessOrEqual(m)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void OpCmovg(struct Machine *m, u32 rde) {
  if (IsGreater(m)) {
    OpMovGvqpEvqp(m, rde);
  }
}

static void SetEb(struct Machine *m, u32 rde, bool x) {
  Write8(GetModrmRegisterBytePointerWrite(m, rde), x);
}

static void OpSeto(struct Machine *m, u32 rde) {
  SetEb(m, rde, GetFlag(m->flags, FLAGS_OF));
}

static void OpSetno(struct Machine *m, u32 rde) {
  SetEb(m, rde, !GetFlag(m->flags, FLAGS_OF));
}

static void OpSetb(struct Machine *m, u32 rde) {
  SetEb(m, rde, GetFlag(m->flags, FLAGS_CF));
}

static void OpSetae(struct Machine *m, u32 rde) {
  SetEb(m, rde, !GetFlag(m->flags, FLAGS_CF));
}

static void OpSete(struct Machine *m, u32 rde) {
  SetEb(m, rde, GetFlag(m->flags, FLAGS_ZF));
}

static void OpSetne(struct Machine *m, u32 rde) {
  SetEb(m, rde, !GetFlag(m->flags, FLAGS_ZF));
}

static void OpSetbe(struct Machine *m, u32 rde) {
  SetEb(m, rde, IsBelowOrEqual(m));
}

static void OpSeta(struct Machine *m, u32 rde) {
  SetEb(m, rde, IsAbove(m));
}

static void OpSets(struct Machine *m, u32 rde) {
  SetEb(m, rde, GetFlag(m->flags, FLAGS_SF));
}

static void OpSetns(struct Machine *m, u32 rde) {
  SetEb(m, rde, !GetFlag(m->flags, FLAGS_SF));
}

static void OpSetp(struct Machine *m, u32 rde) {
  SetEb(m, rde, IsParity(m));
}

static void OpSetnp(struct Machine *m, u32 rde) {
  SetEb(m, rde, !IsParity(m));
}

static void OpSetl(struct Machine *m, u32 rde) {
  SetEb(m, rde, IsLess(m));
}

static void OpSetge(struct Machine *m, u32 rde) {
  SetEb(m, rde, IsGreaterOrEqual(m));
}

static void OpSetle(struct Machine *m, u32 rde) {
  SetEb(m, rde, IsLessOrEqual(m));
}

static void OpSetg(struct Machine *m, u32 rde) {
  SetEb(m, rde, IsGreater(m));
}

static void OpJcxz(struct Machine *m, u32 rde) {
  if (!MaskAddress(Eamode(rde), Read64(m->cx))) {
    OpJmp(m, rde);
  }
}

static u64 AluPopcnt(struct Machine *m, u32 rde, u64 x) {
  m->flags = SetFlag(m->flags, FLAGS_ZF, !x);
  m->flags = SetFlag(m->flags, FLAGS_CF, false);
  m->flags = SetFlag(m->flags, FLAGS_SF, false);
  m->flags = SetFlag(m->flags, FLAGS_OF, false);
  m->flags = SetFlag(m->flags, FLAGS_PF, false);
  return popcount(x);
}

static u64 AluBsr(struct Machine *m, u32 rde, u64 x) {
  unsigned n;
  if (Rexw(rde)) {
    x &= 0xffffffffffffffff;
    n = 64;
  } else if (!Osz(rde)) {
    x &= 0xffffffff;
    n = 32;
  } else {
    x &= 0xffff;
    n = 16;
  }
  if (m->xedd->op.rep == 3) {
    if (!x) {
      m->flags = SetFlag(m->flags, FLAGS_CF, true);
      m->flags = SetFlag(m->flags, FLAGS_ZF, false);
      return n;
    } else {
      m->flags = SetFlag(m->flags, FLAGS_CF, false);
      m->flags = SetFlag(m->flags, FLAGS_ZF, x == 1);
    }
  } else {
    m->flags = SetFlag(m->flags, FLAGS_ZF, !x);
    if (!x) return 0;
  }
  return bsr(x);
}

static u64 AluBsf(struct Machine *m, u32 rde, u64 x) {
  unsigned n;
  if (Rexw(rde)) {
    x &= 0xffffffffffffffff;
    n = 64;
  } else if (!Osz(rde)) {
    x &= 0xffffffff;
    n = 32;
  } else {
    x &= 0xffff;
    n = 16;
  }
  if (m->xedd->op.rep == 3) {
    if (!x) {
      m->flags = SetFlag(m->flags, FLAGS_CF, true);
      m->flags = SetFlag(m->flags, FLAGS_ZF, false);
      return n;
    } else {
      m->flags = SetFlag(m->flags, FLAGS_CF, false);
      m->flags = SetFlag(m->flags, FLAGS_ZF, x & 1);
    }
  } else {
    m->flags = SetFlag(m->flags, FLAGS_ZF, !x);
    if (!x) return 0;
  }
  return bsf(x);
}

static void Bitscan(struct Machine *m, u32 rde,
                    u64 op(struct Machine *, u32, u64)) {
  WriteRegister(
      rde, RegRexrReg(m, rde),
      op(m, rde,
         ReadMemory(rde, GetModrmRegisterWordPointerReadOszRexw(m, rde))));
}

static void OpBsf(struct Machine *m, u32 rde) {
  Bitscan(m, rde, AluBsf);
}

static void OpBsr(struct Machine *m, u32 rde) {
  Bitscan(m, rde, AluBsr);
}

static void Op1b8(struct Machine *m, u32 rde) {
  if (m->xedd->op.rep == 3) {
    Bitscan(m, rde, AluPopcnt);
  } else {
    OpUd(m, rde);
  }
}

static void LoadFarPointer(struct Machine *m, u32 rde, u8 seg[8]) {
  u32 fp;
  fp = Read32((u8 *)ComputeReserveAddressRead4(m, rde));
  Write64(seg, (fp & 0x0000ffff) << 4);
  Write16(RegRexrReg(m, rde), fp >> 16);
}

static void OpLes(struct Machine *m, u32 rde) {
  LoadFarPointer(m, rde, m->es);
}

static void OpLds(struct Machine *m, u32 rde) {
  LoadFarPointer(m, rde, m->ds);
}

static void Loop(struct Machine *m, u32 rde, bool cond) {
  u64 cx;
  cx = Read64(m->cx) - 1;
  if (Eamode(rde) != XED_MODE_REAL) {
    if (Eamode(rde) == XED_MODE_LEGACY) {
      cx &= 0xffffffff;
    }
    Write64(m->cx, cx);
  } else {
    cx &= 0xffff;
    Write16(m->cx, cx);
  }
  if (cx && cond) {
    OpJmp(m, rde);
  }
}

static void OpLoope(struct Machine *m, u32 rde) {
  Loop(m, rde, GetFlag(m->flags, FLAGS_ZF));
}

static void OpLoopne(struct Machine *m, u32 rde) {
  Loop(m, rde, !GetFlag(m->flags, FLAGS_ZF));
}

static void OpLoop1(struct Machine *m, u32 rde) {
  Loop(m, rde, true);
}

static const nexgen32e_f kOp0f6[] = {
    OpAlubiTest,
    OpAlubiTest,
    OpNotEb,
    OpNegEb,
    OpMulAxAlEbUnsigned,
    OpMulAxAlEbSigned,
    OpDivAlAhAxEbUnsigned,
    OpDivAlAhAxEbSigned,
};

static void Op0f6(struct Machine *m, u32 rde) {
  kOp0f6[ModrmReg(rde)](m, rde);
}

static void OpTestEvqpIvds(struct Machine *m, u32 rde) {
  AluwiRo(m, rde, kAlu[ALU_AND]);
}

static const nexgen32e_f kOp0f7[] = {
    OpTestEvqpIvds,
    OpTestEvqpIvds,
    OpNotEvqp,
    OpNegEvqp,
    OpMulRdxRaxEvqpUnsigned,
    OpMulRdxRaxEvqpSigned,
    OpDivRdxRaxEvqpUnsigned,
    OpDivRdxRaxEvqpSigned,
};

static void Op0f7(struct Machine *m, u32 rde) {
  kOp0f7[ModrmReg(rde)](m, rde);
}

static const nexgen32e_f kOp0ff[] = {OpIncEvqp, OpDecEvqp, OpCallEq,  OpUd,
                                     OpJmpEq,   OpUd,      OpPushEvq, OpUd};

static void Op0ff(struct Machine *m, u32 rde) {
  kOp0ff[ModrmReg(rde)](m, rde);
}

static void OpDoubleShift(struct Machine *m, u32 rde) {
  u8 *p;
  u8 W[2][2] = {{2, 3}, {1, 3}};
  p = GetModrmRegisterWordPointerWriteOszRexw(m, rde);
  WriteRegisterOrMemory(
      rde, p,
      BsuDoubleShift(W[Osz(rde)][Rexw(rde)], ReadMemory(rde, p),
                     ReadRegister(rde, RegRexrReg(m, rde)),
                     m->xedd->op.opcode & 1 ? Read8(m->cx) : m->xedd->op.uimm0,
                     m->xedd->op.opcode & 8, &m->flags));
}

static void OpFxsave(struct Machine *m, u32 rde) {
  i64 v;
  u8 buf[32];
  memset(buf, 0, 32);
  Write16(buf + 0, m->fpu.cw);
  Write16(buf + 2, m->fpu.sw);
  Write8(buf + 4, m->fpu.tw);
  Write16(buf + 6, m->fpu.op);
  Write32(buf + 8, m->fpu.ip);
  Write32(buf + 24, m->mxcsr);
  v = ComputeAddress(m, rde);
  VirtualRecv(m, v + 0, buf, 32);
  VirtualRecv(m, v + 32, m->fpu.st, 128);
  VirtualRecv(m, v + 160, m->xmm, 256);
  SetWriteAddr(m, v, 416);
}

static void OpFxrstor(struct Machine *m, u32 rde) {
  i64 v;
  u8 buf[32];
  v = ComputeAddress(m, rde);
  SetReadAddr(m, v, 416);
  VirtualSend(m, buf, v + 0, 32);
  VirtualSend(m, m->fpu.st, v + 32, 128);
  VirtualSend(m, m->xmm, v + 160, 256);
  m->fpu.cw = Read16(buf + 0);
  m->fpu.sw = Read16(buf + 2);
  m->fpu.tw = Read8(buf + 4);
  m->fpu.op = Read16(buf + 6);
  m->fpu.ip = Read32(buf + 8);
  m->mxcsr = Read32(buf + 24);
}

static void OpXsave(struct Machine *m, u32 rde) {
}

static void OpLdmxcsr(struct Machine *m, u32 rde) {
  m->mxcsr = Read32((u8 *)ComputeReserveAddressRead4(m, rde));
}

static void OpStmxcsr(struct Machine *m, u32 rde) {
  Write32((u8 *)ComputeReserveAddressWrite4(m, rde), m->mxcsr);
}

static void OpRdfsbase(struct Machine *m, u32 rde) {
  WriteRegister(rde, RegRexbRm(m, rde), Read64(m->fs));
}

static void OpRdgsbase(struct Machine *m, u32 rde) {
  WriteRegister(rde, RegRexbRm(m, rde), Read64(m->gs));
}

static void OpWrfsbase(struct Machine *m, u32 rde) {
  Write64(m->fs, ReadRegister(rde, RegRexbRm(m, rde)));
}

static void OpWrgsbase(struct Machine *m, u32 rde) {
  Write64(m->gs, ReadRegister(rde, RegRexbRm(m, rde)));
}

static void Op1ae(struct Machine *m, u32 rde) {
  bool ismem;
  ismem = !IsModrmRegister(rde);
  switch (ModrmReg(rde)) {
    case 0:
      if (ismem) {
        OpFxsave(m, rde);
      } else {
        OpRdfsbase(m, rde);
      }
      break;
    case 1:
      if (ismem) {
        OpFxrstor(m, rde);
      } else {
        OpRdgsbase(m, rde);
      }
      break;
    case 2:
      if (ismem) {
        OpLdmxcsr(m, rde);
      } else {
        OpWrfsbase(m, rde);
      }
      break;
    case 3:
      if (ismem) {
        OpStmxcsr(m, rde);
      } else {
        OpWrgsbase(m, rde);
      }
      break;
    case 4:
      if (ismem) {
        OpXsave(m, rde);
      } else {
        OpUd(m, rde);
      }
      break;
    case 5:
      OpLfence(m, rde);
      break;
    case 6:
      OpMfence(m, rde);
      break;
    case 7:
      if (ismem) {
        OpClflush(m, rde);
      } else {
        OpSfence(m, rde);
      }
      break;
    default:
      OpUd(m, rde);
  }
}

static void OpSalc(struct Machine *m, u32 rde) {
  if (GetFlag(m->flags, FLAGS_CF)) {
    m->ax[0] = 255;
  } else {
    m->ax[0] = 0;
  }
}

static void OpBofram(struct Machine *m, u32 rde) {
  if (m->xedd->op.disp) {
    m->bofram[0] = m->ip;
    m->bofram[1] = m->ip + (m->xedd->op.disp & 0xff);
  } else {
    m->bofram[0] = 0;
    m->bofram[1] = 0;
  }
}

static void OpBinbase(struct Machine *m, u32 rde) {
  if (m->system->onbinbase) {
    m->system->onbinbase(m);
  }
}

static void OpNopEv(struct Machine *m, u32 rde) {
  switch (ModrmMod(rde) << 6 | ModrmReg(rde) << 3 | ModrmRm(rde)) {
    case 0105:
      OpBofram(m, rde);
      break;
    case 0007:
    case 0107:
    case 0207:
      OpBinbase(m, rde);
      break;
    default:
      OpNoop(m, rde);
  }
}

static void OpNop(struct Machine *m, u32 rde) {
  if (Rexb(rde)) {
    OpXchgZvqp(m, rde);
  } else if (m->xedd->op.rep == 3) {
    OpPause(m, rde);
  } else {
    OpNoop(m, rde);
  }
}

static void OpMovRqCq(struct Machine *m, u32 rde) {
  switch (ModrmReg(rde)) {
    case 0:
      Write64(RegRexbRm(m, rde), m->system->cr0);
      break;
    case 2:
      Write64(RegRexbRm(m, rde), m->system->cr2);
      break;
    case 3:
      Write64(RegRexbRm(m, rde), m->system->cr3);
      break;
    case 4:
      Write64(RegRexbRm(m, rde), m->system->cr4);
      break;
    default:
      OpUd(m, rde);
  }
}

static void OpMovCqRq(struct Machine *m, u32 rde) {
  i64 cr3;
  switch (ModrmReg(rde)) {
    case 0:
      m->system->cr0 = Read64(RegRexbRm(m, rde));
      break;
    case 2:
      m->system->cr2 = Read64(RegRexbRm(m, rde));
      break;
    case 3:
      cr3 = Read64(RegRexbRm(m, rde));
      if (0 <= cr3 && cr3 + 512 * 8 <= GetRealMemorySize(m->system)) {
        m->system->cr3 = cr3;
      } else {
        ThrowProtectionFault(m);
      }
      break;
    case 4:
      m->system->cr4 = Read64(RegRexbRm(m, rde));
      break;
    default:
      OpUd(m, rde);
  }
}

static void OpWrmsr(struct Machine *m, u32 rde) {
}

static void OpRdmsr(struct Machine *m, u32 rde) {
  Write32(m->dx, 0);
  Write32(m->ax, 0);
}

static void OpEmms(struct Machine *m, u32 rde) {
  m->fpu.tw = -1;
}

const nexgen32e_f kNexgen32e[] = {
    /*000*/ OpAlubAdd,
    /*001*/ OpAluw,
    /*002*/ OpAlubFlipAdd,
    /*003*/ OpAluwFlip,
    /*004*/ OpAluAlIbAdd,
    /*005*/ OpAluRaxIvds,
    /*006*/ OpPushSeg,
    /*007*/ OpPopSeg,
    /*008*/ OpAlubOr,
    /*009*/ OpAluw,
    /*00A*/ OpAlubFlipOr,
    /*00B*/ OpAluwFlip,
    /*00C*/ OpAluAlIbOr,
    /*00D*/ OpAluRaxIvds,
    /*00E*/ OpPushSeg,
    /*00F*/ OpPopSeg,
    /*010*/ OpAlubAdc,
    /*011*/ OpAluw,
    /*012*/ OpAlubFlipAdc,
    /*013*/ OpAluwFlip,
    /*014*/ OpAluAlIbAdc,
    /*015*/ OpAluRaxIvds,
    /*016*/ OpPushSeg,
    /*017*/ OpPopSeg,
    /*018*/ OpAlubSbb,
    /*019*/ OpAluw,
    /*01A*/ OpAlubFlipSbb,
    /*01B*/ OpAluwFlip,
    /*01C*/ OpAluAlIbSbb,
    /*01D*/ OpAluRaxIvds,
    /*01E*/ OpPushSeg,
    /*01F*/ OpPopSeg,
    /*020*/ OpAlubAnd,
    /*021*/ OpAluw,
    /*022*/ OpAlubFlipAnd,
    /*023*/ OpAluwFlip,
    /*024*/ OpAluAlIbAnd,
    /*025*/ OpAluRaxIvds,
    /*026*/ OpPushSeg,
    /*027*/ OpPopSeg,
    /*028*/ OpAlubSub,
    /*029*/ OpAluw,
    /*02A*/ OpAlubFlipSub,
    /*02B*/ OpAluwFlip,
    /*02C*/ OpAluAlIbSub,
    /*02D*/ OpAluRaxIvds,
    /*02E*/ OpUd,
    /*02F*/ OpDas,
    /*030*/ OpAlubXor,
    /*031*/ OpAluw,
    /*032*/ OpAlubFlipXor,
    /*033*/ OpAluwFlip,
    /*034*/ OpAluAlIbXor,
    /*035*/ OpAluRaxIvds,
    /*036*/ OpUd,
    /*037*/ OpAaa,
    /*038*/ OpAlubCmp,
    /*039*/ OpAluwCmp,
    /*03A*/ OpAlubFlipCmp,
    /*03B*/ OpAluwFlipCmp,
    /*03C*/ OpCmpAlIb,
    /*03D*/ OpCmpRaxIvds,
    /*03E*/ OpUd,
    /*03F*/ OpAas,
    /*040*/ OpIncZv,
    /*041*/ OpIncZv,
    /*042*/ OpIncZv,
    /*043*/ OpIncZv,
    /*044*/ OpIncZv,
    /*045*/ OpIncZv,
    /*046*/ OpIncZv,
    /*047*/ OpIncZv,
    /*048*/ OpDecZv,
    /*049*/ OpDecZv,
    /*04A*/ OpDecZv,
    /*04B*/ OpDecZv,
    /*04C*/ OpDecZv,
    /*04D*/ OpDecZv,
    /*04E*/ OpDecZv,
    /*04F*/ OpDecZv,
    /*050*/ OpPushZvq,
    /*051*/ OpPushZvq,
    /*052*/ OpPushZvq,
    /*053*/ OpPushZvq,
    /*054*/ OpPushZvq,
    /*055*/ OpPushZvq,
    /*056*/ OpPushZvq,
    /*057*/ OpPushZvq,
    /*058*/ OpPopZvq,
    /*059*/ OpPopZvq,
    /*05A*/ OpPopZvq,
    /*05B*/ OpPopZvq,
    /*05C*/ OpPopZvq,
    /*05D*/ OpPopZvq,
    /*05E*/ OpPopZvq,
    /*05F*/ OpPopZvq,
    /*060*/ OpPusha,
    /*061*/ OpPopa,
    /*062*/ OpUd,
    /*063*/ OpMovsxdGdqpEd,
    /*064*/ OpUd,
    /*065*/ OpUd,
    /*066*/ OpUd,
    /*067*/ OpUd,
    /*068*/ OpPushImm,
    /*069*/ OpImulGvqpEvqpImm,
    /*06A*/ OpPushImm,
    /*06B*/ OpImulGvqpEvqpImm,
    /*06C*/ OpIns,
    /*06D*/ OpIns,
    /*06E*/ OpOuts,
    /*06F*/ OpOuts,
    /*070*/ OpJo,
    /*071*/ OpJno,
    /*072*/ OpJb,
    /*073*/ OpJae,
    /*074*/ OpJe,
    /*075*/ OpJne,
    /*076*/ OpJbe,
    /*077*/ OpJa,
    /*078*/ OpJs,
    /*079*/ OpJns,
    /*07A*/ OpJp,
    /*07B*/ OpJnp,
    /*07C*/ OpJl,
    /*07D*/ OpJge,
    /*07E*/ OpJle,
    /*07F*/ OpJg,
    /*080*/ OpAlubiReg,
    /*081*/ OpAluwiReg,
    /*082*/ OpAlubiReg,
    /*083*/ OpAluwiReg,
    /*084*/ OpAlubTest,
    /*085*/ OpAluwTest,
    /*086*/ OpXchgGbEb,
    /*087*/ OpXchgGvqpEvqp,
    /*088*/ OpMovEbGb,
    /*089*/ OpMovEvqpGvqp,
    /*08A*/ OpMovGbEb,
    /*08B*/ OpMovGvqpEvqp,
    /*08C*/ OpMovEvqpSw,
    /*08D*/ OpLeaGvqpM,
    /*08E*/ OpMovSwEvqp,
    /*08F*/ OpPopEvq,
    /*090*/ OpNop,
    /*091*/ OpXchgZvqp,
    /*092*/ OpXchgZvqp,
    /*093*/ OpXchgZvqp,
    /*094*/ OpXchgZvqp,
    /*095*/ OpXchgZvqp,
    /*096*/ OpXchgZvqp,
    /*097*/ OpXchgZvqp,
    /*098*/ OpSax,
    /*099*/ OpConvert,
    /*09A*/ OpCallf,
    /*09B*/ OpFwait,
    /*09C*/ OpPushf,
    /*09D*/ OpPopf,
    /*09E*/ OpSahf,
    /*09F*/ OpLahf,
    /*0A0*/ OpMovAlOb,
    /*0A1*/ OpMovRaxOvqp,
    /*0A2*/ OpMovObAl,
    /*0A3*/ OpMovOvqpRax,
    /*0A4*/ OpMovsb,
    /*0A5*/ OpMovs,
    /*0A6*/ OpCmps,
    /*0A7*/ OpCmps,
    /*0A8*/ OpTestAlIb,
    /*0A9*/ OpTestRaxIvds,
    /*0AA*/ OpStosb,
    /*0AB*/ OpStos,
    /*0AC*/ OpLods,
    /*0AD*/ OpLods,
    /*0AE*/ OpScas,
    /*0AF*/ OpScas,
    /*0B0*/ OpMovZbIb,
    /*0B1*/ OpMovZbIb,
    /*0B2*/ OpMovZbIb,
    /*0B3*/ OpMovZbIb,
    /*0B4*/ OpMovZbIb,
    /*0B5*/ OpMovZbIb,
    /*0B6*/ OpMovZbIb,
    /*0B7*/ OpMovZbIb,
    /*0B8*/ OpMovZvqpIvqp,
    /*0B9*/ OpMovZvqpIvqp,
    /*0BA*/ OpMovZvqpIvqp,
    /*0BB*/ OpMovZvqpIvqp,
    /*0BC*/ OpMovZvqpIvqp,
    /*0BD*/ OpMovZvqpIvqp,
    /*0BE*/ OpMovZvqpIvqp,
    /*0BF*/ OpMovZvqpIvqp,
    /*0C0*/ OpBsubiImm,
    /*0C1*/ OpBsuwiImm,
    /*0C2*/ OpRet,
    /*0C3*/ OpRet,
    /*0C4*/ OpLes,
    /*0C5*/ OpLds,
    /*0C6*/ OpMovEbIb,
    /*0C7*/ OpMovEvqpIvds,
    /*0C8*/ OpUd,
    /*0C9*/ OpLeave,
    /*0CA*/ OpRetf,
    /*0CB*/ OpRetf,
    /*0CC*/ OpInterrupt3,
    /*0CD*/ OpInterruptImm,
    /*0CE*/ OpUd,
    /*0CF*/ OpUd,
    /*0D0*/ OpBsubi1,
    /*0D1*/ OpBsuwi1,
    /*0D2*/ OpBsubiCl,
    /*0D3*/ OpBsuwiCl,
    /*0D4*/ OpAam,
    /*0D5*/ OpAad,
    /*0D6*/ OpSalc,
    /*0D7*/ OpXlatAlBbb,
    /*0D8*/ OpFpu,
    /*0D9*/ OpFpu,
    /*0DA*/ OpFpu,
    /*0DB*/ OpFpu,
    /*0DC*/ OpFpu,
    /*0DD*/ OpFpu,
    /*0DE*/ OpFpu,
    /*0DF*/ OpFpu,
    /*0E0*/ OpLoopne,
    /*0E1*/ OpLoope,
    /*0E2*/ OpLoop1,
    /*0E3*/ OpJcxz,
    /*0E4*/ OpInAlImm,
    /*0E5*/ OpInAxImm,
    /*0E6*/ OpOutImmAl,
    /*0E7*/ OpOutImmAx,
    /*0E8*/ OpCallJvds,
    /*0E9*/ OpJmp,
    /*0EA*/ OpJmpf,
    /*0EB*/ OpJmp,
    /*0EC*/ OpInAlDx,
    /*0ED*/ OpInAxDx,
    /*0EE*/ OpOutDxAl,
    /*0EF*/ OpOutDxAx,
    /*0F0*/ OpUd,
    /*0F1*/ OpInterrupt1,
    /*0F2*/ OpUd,
    /*0F3*/ OpUd,
    /*0F4*/ OpHlt,
    /*0F5*/ OpCmc,
    /*0F6*/ Op0f6,
    /*0F7*/ Op0f7,
    /*0F8*/ OpClc,
    /*0F9*/ OpStc,
    /*0FA*/ OpCli,
    /*0FB*/ OpSti,
    /*0FC*/ OpCld,
    /*0FD*/ OpStd,
    /*0FE*/ Op0fe,
    /*0FF*/ Op0ff,
    /*100*/ OpUd,
    /*101*/ Op101,
    /*102*/ OpUd,
    /*103*/ OpLsl,
    /*104*/ OpUd,
    /*105*/ OpSyscall,
    /*106*/ OpUd,
    /*107*/ OpUd,
    /*108*/ OpUd,
    /*109*/ OpUd,
    /*10A*/ OpUd,
    /*10B*/ OpUd,
    /*10C*/ OpUd,
    /*10D*/ OpHintNopEv,
    /*10E*/ OpUd,
    /*10F*/ OpUd,
    /*110*/ OpMov0f10,
    /*111*/ OpMovWpsVps,
    /*112*/ OpMov0f12,
    /*113*/ OpMov0f13,
    /*114*/ OpUnpcklpsd,
    /*115*/ OpUnpckhpsd,
    /*116*/ OpMov0f16,
    /*117*/ OpMov0f17,
    /*118*/ OpHintNopEv,
    /*119*/ OpHintNopEv,
    /*11A*/ OpHintNopEv,
    /*11B*/ OpHintNopEv,
    /*11C*/ OpHintNopEv,
    /*11D*/ OpHintNopEv,
    /*11E*/ OpHintNopEv,
    /*11F*/ OpNopEv,
    /*120*/ OpMovRqCq,
    /*121*/ OpUd,
    /*122*/ OpMovCqRq,
    /*123*/ OpUd,
    /*124*/ OpUd,
    /*125*/ OpUd,
    /*126*/ OpUd,
    /*127*/ OpUd,
    /*128*/ OpMov0f28,
    /*129*/ OpMovWpsVps,
    /*12A*/ OpCvt0f2a,
    /*12B*/ OpMov0f2b,
    /*12C*/ OpCvtt0f2c,
    /*12D*/ OpCvt0f2d,
    /*12E*/ OpComissVsWs,
    /*12F*/ OpComissVsWs,
    /*130*/ OpWrmsr,
    /*131*/ OpRdtsc,
    /*132*/ OpRdmsr,
    /*133*/ OpUd,
    /*134*/ OpUd,
    /*135*/ OpUd,
    /*136*/ OpUd,
    /*137*/ OpUd,
    /*138*/ OpUd,
    /*139*/ OpUd,
    /*13A*/ OpUd,
    /*13B*/ OpUd,
    /*13C*/ OpUd,
    /*13D*/ OpUd,
    /*13E*/ OpUd,
    /*13F*/ OpUd,
    /*140*/ OpCmovo,
    /*141*/ OpCmovno,
    /*142*/ OpCmovb,
    /*143*/ OpCmovae,
    /*144*/ OpCmove,
    /*145*/ OpCmovne,
    /*146*/ OpCmovbe,
    /*147*/ OpCmova,
    /*148*/ OpCmovs,
    /*149*/ OpCmovns,
    /*14A*/ OpCmovp,
    /*14B*/ OpCmovnp,
    /*14C*/ OpCmovl,
    /*14D*/ OpCmovge,
    /*14E*/ OpCmovle,
    /*14F*/ OpCmovg,
    /*150*/ OpUd,
    /*151*/ OpSqrtpsd,
    /*152*/ OpRsqrtps,
    /*153*/ OpRcpps,
    /*154*/ OpAndpsd,
    /*155*/ OpAndnpsd,
    /*156*/ OpOrpsd,
    /*157*/ OpXorpsd,
    /*158*/ OpAddpsd,
    /*159*/ OpMulpsd,
    /*15A*/ OpCvt0f5a,
    /*15B*/ OpCvt0f5b,
    /*15C*/ OpSubpsd,
    /*15D*/ OpMinpsd,
    /*15E*/ OpDivpsd,
    /*15F*/ OpMaxpsd,
    /*160*/ OpSsePunpcklbw,
    /*161*/ OpSsePunpcklwd,
    /*162*/ OpSsePunpckldq,
    /*163*/ OpSsePacksswb,
    /*164*/ OpSsePcmpgtb,
    /*165*/ OpSsePcmpgtw,
    /*166*/ OpSsePcmpgtd,
    /*167*/ OpSsePackuswb,
    /*168*/ OpSsePunpckhbw,
    /*169*/ OpSsePunpckhwd,
    /*16A*/ OpSsePunpckhdq,
    /*16B*/ OpSsePackssdw,
    /*16C*/ OpSsePunpcklqdq,
    /*16D*/ OpSsePunpckhqdq,
    /*16E*/ OpMov0f6e,
    /*16F*/ OpMov0f6f,
    /*170*/ OpShuffle,
    /*171*/ Op171,
    /*172*/ Op172,
    /*173*/ Op173,
    /*174*/ OpSsePcmpeqb,
    /*175*/ OpSsePcmpeqw,
    /*176*/ OpSsePcmpeqd,
    /*177*/ OpEmms,
    /*178*/ OpUd,
    /*179*/ OpUd,
    /*17A*/ OpUd,
    /*17B*/ OpUd,
    /*17C*/ OpHaddpsd,
    /*17D*/ OpHsubpsd,
    /*17E*/ OpMov0f7e,
    /*17F*/ OpMov0f7f,
    /*180*/ OpJo,
    /*181*/ OpJno,
    /*182*/ OpJb,
    /*183*/ OpJae,
    /*184*/ OpJe,
    /*185*/ OpJne,
    /*186*/ OpJbe,
    /*187*/ OpJa,
    /*188*/ OpJs,
    /*189*/ OpJns,
    /*18A*/ OpJp,
    /*18B*/ OpJnp,
    /*18C*/ OpJl,
    /*18D*/ OpJge,
    /*18E*/ OpJle,
    /*18F*/ OpJg,
    /*190*/ OpSeto,
    /*191*/ OpSetno,
    /*192*/ OpSetb,
    /*193*/ OpSetae,
    /*194*/ OpSete,
    /*195*/ OpSetne,
    /*196*/ OpSetbe,
    /*197*/ OpSeta,
    /*198*/ OpSets,
    /*199*/ OpSetns,
    /*19A*/ OpSetp,
    /*19B*/ OpSetnp,
    /*19C*/ OpSetl,
    /*19D*/ OpSetge,
    /*19E*/ OpSetle,
    /*19F*/ OpSetg,
    /*1A0*/ OpPushSeg,
    /*1A1*/ OpPopSeg,
    /*1A2*/ OpCpuid,
    /*1A3*/ OpBit,
    /*1A4*/ OpDoubleShift,
    /*1A5*/ OpDoubleShift,
    /*1A6*/ OpUd,
    /*1A7*/ OpUd,
    /*1A8*/ OpPushSeg,
    /*1A9*/ OpPopSeg,
    /*1AA*/ OpUd,
    /*1AB*/ OpBit,
    /*1AC*/ OpDoubleShift,
    /*1AD*/ OpDoubleShift,
    /*1AE*/ Op1ae,
    /*1AF*/ OpImulGvqpEvqp,
    /*1B0*/ OpCmpxchgEbAlGb,
    /*1B1*/ OpCmpxchgEvqpRaxGvqp,
    /*1B2*/ OpUd,
    /*1B3*/ OpBit,
    /*1B4*/ OpUd,
    /*1B5*/ OpUd,
    /*1B6*/ OpMovzbGvqpEb,
    /*1B7*/ OpMovzwGvqpEw,
    /*1B8*/ Op1b8,
    /*1B9*/ OpUd,
    /*1BA*/ OpBit,
    /*1BB*/ OpBit,
    /*1BC*/ OpBsf,
    /*1BD*/ OpBsr,
    /*1BE*/ OpMovsbGvqpEb,
    /*1BF*/ OpMovswGvqpEw,
    /*1C0*/ OpXaddEbGb,
    /*1C1*/ OpXaddEvqpGvqp,
    /*1C2*/ OpCmppsd,
    /*1C3*/ OpMovntiMdqpGdqp,
    /*1C4*/ OpPinsrwVdqEwIb,
    /*1C5*/ OpPextrwGdqpUdqIb,
    /*1C6*/ OpShufpsd,
    /*1C7*/ Op1c7,
    /*1C8*/ OpBswapZvqp,
    /*1C9*/ OpBswapZvqp,
    /*1CA*/ OpBswapZvqp,
    /*1CB*/ OpBswapZvqp,
    /*1CC*/ OpBswapZvqp,
    /*1CD*/ OpBswapZvqp,
    /*1CE*/ OpBswapZvqp,
    /*1CF*/ OpBswapZvqp,
    /*1D0*/ OpAddsubpsd,
    /*1D1*/ OpSsePsrlwv,
    /*1D2*/ OpSsePsrldv,
    /*1D3*/ OpSsePsrlqv,
    /*1D4*/ OpSsePaddq,
    /*1D5*/ OpSsePmullw,
    /*1D6*/ OpMov0fD6,
    /*1D7*/ OpPmovmskbGdqpNqUdq,
    /*1D8*/ OpSsePsubusb,
    /*1D9*/ OpSsePsubusw,
    /*1DA*/ OpSsePminub,
    /*1DB*/ OpSsePand,
    /*1DC*/ OpSsePaddusb,
    /*1DD*/ OpSsePaddusw,
    /*1DE*/ OpSsePmaxub,
    /*1DF*/ OpSsePandn,
    /*1E0*/ OpSsePavgb,
    /*1E1*/ OpSsePsrawv,
    /*1E2*/ OpSsePsradv,
    /*1E3*/ OpSsePavgw,
    /*1E4*/ OpSsePmulhuw,
    /*1E5*/ OpSsePmulhw,
    /*1E6*/ OpCvt0fE6,
    /*1E7*/ OpMov0fE7,
    /*1E8*/ OpSsePsubsb,
    /*1E9*/ OpSsePsubsw,
    /*1EA*/ OpSsePminsw,
    /*1EB*/ OpSsePor,
    /*1EC*/ OpSsePaddsb,
    /*1ED*/ OpSsePaddsw,
    /*1EE*/ OpSsePmaxsw,
    /*1EF*/ OpSsePxor,
    /*1F0*/ OpLddquVdqMdq,
    /*1F1*/ OpSsePsllwv,
    /*1F2*/ OpSsePslldv,
    /*1F3*/ OpSsePsllqv,
    /*1F4*/ OpSsePmuludq,
    /*1F5*/ OpSsePmaddwd,
    /*1F6*/ OpSsePsadbw,
    /*1F7*/ OpMaskMovDiXmmRegXmmRm,
    /*1F8*/ OpSsePsubb,
    /*1F9*/ OpSsePsubw,
    /*1FA*/ OpSsePsubd,
    /*1FB*/ OpSsePsubq,
    /*1FC*/ OpSsePaddb,
    /*1FD*/ OpSsePaddw,
    /*1FE*/ OpSsePaddd,
    /*1FF*/ OpUd,
    /*200*/ OpSsePshufb,
    /*201*/ OpSsePhaddw,
    /*202*/ OpSsePhaddd,
    /*203*/ OpSsePhaddsw,
    /*204*/ OpSsePmaddubsw,
    /*205*/ OpSsePhsubw,
    /*206*/ OpSsePhsubd,
    /*207*/ OpSsePhsubsw,
    /*208*/ OpSsePsignb,
    /*209*/ OpSsePsignw,
    /*20A*/ OpSsePsignd,
    /*20B*/ OpSsePmulhrsw,
};

void ExecuteSparseInstruction(struct Machine *m, u32 rde, u32 d) {
  switch (d) {
    CASE(0x21c, OpSsePabsb(m, rde));
    CASE(0x21d, OpSsePabsw(m, rde));
    CASE(0x21e, OpSsePabsd(m, rde));
    CASE(0x22a, OpMovntdqaVdqMdq(m, rde));
    CASE(0x240, OpSsePmulld(m, rde));
    CASE(0x30f, OpSsePalignr(m, rde));
    CASE(0x344, OpSsePclmulqdq(m, rde));
    default:
      OpUd(m, rde);
  }
}

void ExecuteInstruction(struct Machine *m) {
  int dispatch;
  m->ip += m->xedd->length;
  dispatch = m->xedd->op.map << 8 | m->xedd->op.opcode;
  if (dispatch < ARRAYLEN(kNexgen32e)) {
    kNexgen32e[dispatch](m, m->xedd->op.rde);
  } else {
    ExecuteSparseInstruction(m, m->xedd->op.rde, dispatch);
  }
  if (m->opcache->stashaddr) {
    VirtualRecv(m, m->opcache->stashaddr, m->opcache->stash,
                m->opcache->stashsize);
    m->opcache->stashaddr = 0;
  }
}