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
#include <string.h>

#include "blink/address.h"
#include "blink/builtin.h"
#include "blink/endian.h"
#include "blink/memory.h"
#include "blink/modrm.h"
#include "blink/ssemov.h"

static u32 pmovmskb(const u8 p[16]) {
  u32 i, m;
  for (m = i = 0; i < 16; ++i) {
    if (p[i] & 0x80) m |= 1 << i;
  }
  return m;
}

static void MovdquVdqWdq(struct Machine *m, u32 rde) {
  memcpy(XmmRexrReg(m, rde), GetModrmRegisterXmmPointerRead16(m, rde), 16);
}

static void MovdquWdqVdq(struct Machine *m, u32 rde) {
  memcpy(GetModrmRegisterXmmPointerWrite16(m, rde), XmmRexrReg(m, rde), 16);
}

static void MovupsVpsWps(struct Machine *m, u32 rde) {
  MovdquVdqWdq(m, rde);
}

static void MovupsWpsVps(struct Machine *m, u32 rde) {
  MovdquWdqVdq(m, rde);
}

static void MovupdVpsWps(struct Machine *m, u32 rde) {
  MovdquVdqWdq(m, rde);
}

static void MovupdWpsVps(struct Machine *m, u32 rde) {
  MovdquWdqVdq(m, rde);
}

void OpLddquVdqMdq(struct Machine *m, u32 rde) {
  MovdquVdqWdq(m, rde);
}

void OpMovntiMdqpGdqp(struct Machine *m, u32 rde) {
  if (Rexw(rde)) {
    memcpy(ComputeReserveAddressWrite8(m, rde), XmmRexrReg(m, rde), 8);
  } else {
    memcpy(ComputeReserveAddressWrite4(m, rde), XmmRexrReg(m, rde), 4);
  }
}

static void MovdqaVdqMdq(struct Machine *m, u32 rde) {
  i64 v;
  u8 *p;
  v = ComputeAddress(m, rde);
  SetReadAddr(m, v, 16);
  if ((v & 15) || !(p = (u8 *)FindReal(m, v))) {
    ThrowSegmentationFault(m, v);
  }
  memcpy(XmmRexrReg(m, rde), p, 16);
}

static void MovdqaMdqVdq(struct Machine *m, u32 rde) {
  i64 v;
  u8 *p;
  v = ComputeAddress(m, rde);
  SetWriteAddr(m, v, 16);
  if ((v & 15) || !(p = (u8 *)FindReal(m, v))) {
    ThrowSegmentationFault(m, v);
  }
  memcpy(p, XmmRexrReg(m, rde), 16);
}

static void MovdqaVdqWdq(struct Machine *m, u32 rde) {
  if (IsModrmRegister(rde)) {
    memcpy(XmmRexrReg(m, rde), XmmRexbRm(m, rde), 16);
  } else {
    MovdqaVdqMdq(m, rde);
  }
}

static void MovdqaWdqVdq(struct Machine *m, u32 rde) {
  if (IsModrmRegister(rde)) {
    memcpy(XmmRexbRm(m, rde), XmmRexrReg(m, rde), 16);
  } else {
    MovdqaMdqVdq(m, rde);
  }
}

static void MovntdqMdqVdq(struct Machine *m, u32 rde) {
  MovdqaMdqVdq(m, rde);
}

static void MovntpsMpsVps(struct Machine *m, u32 rde) {
  MovdqaMdqVdq(m, rde);
}

static void MovntpdMpdVpd(struct Machine *m, u32 rde) {
  MovdqaMdqVdq(m, rde);
}

void OpMovntdqaVdqMdq(struct Machine *m, u32 rde) {
  MovdqaVdqMdq(m, rde);
}

static void MovqPqQq(struct Machine *m, u32 rde) {
  memcpy(MmReg(m, rde), GetModrmRegisterMmPointerRead8(m, rde), 8);
}

static void MovqQqPq(struct Machine *m, u32 rde) {
  memcpy(GetModrmRegisterMmPointerWrite8(m, rde), MmReg(m, rde), 8);
}

static void MovqVdqEqp(struct Machine *m, u32 rde) {
  memcpy(XmmRexrReg(m, rde), GetModrmRegisterWordPointerRead8(m, rde), 8);
  memset(XmmRexrReg(m, rde) + 8, 0, 8);
}

static void MovdVdqEd(struct Machine *m, u32 rde) {
  memset(XmmRexrReg(m, rde), 0, 16);
  memcpy(XmmRexrReg(m, rde), GetModrmRegisterWordPointerRead4(m, rde), 4);
}

static void MovqPqEqp(struct Machine *m, u32 rde) {
  memcpy(MmReg(m, rde), GetModrmRegisterWordPointerRead8(m, rde), 8);
}

static void MovdPqEd(struct Machine *m, u32 rde) {
  memcpy(MmReg(m, rde), GetModrmRegisterWordPointerRead4(m, rde), 4);
  memset(MmReg(m, rde) + 4, 0, 4);
}

static void MovdEdVdq(struct Machine *m, u32 rde) {
  if (IsModrmRegister(rde)) {
    Write64(RegRexbRm(m, rde), Read32(XmmRexrReg(m, rde)));
  } else {
    memcpy(ComputeReserveAddressWrite4(m, rde), XmmRexrReg(m, rde), 4);
  }
}

static void MovqEqpVdq(struct Machine *m, u32 rde) {
  memcpy(GetModrmRegisterWordPointerWrite8(m, rde), XmmRexrReg(m, rde), 8);
}

static void MovdEdPq(struct Machine *m, u32 rde) {
  if (IsModrmRegister(rde)) {
    Write64(RegRexbRm(m, rde), Read32(MmReg(m, rde)));
  } else {
    memcpy(ComputeReserveAddressWrite4(m, rde), MmReg(m, rde), 4);
  }
}

static void MovqEqpPq(struct Machine *m, u32 rde) {
  memcpy(GetModrmRegisterWordPointerWrite(m, rde, 8), MmReg(m, rde), 8);
}

static void MovntqMqPq(struct Machine *m, u32 rde) {
  memcpy(ComputeReserveAddressWrite8(m, rde), MmReg(m, rde), 8);
}

static void MovqVqWq(struct Machine *m, u32 rde) {
  memcpy(XmmRexrReg(m, rde), GetModrmRegisterXmmPointerRead8(m, rde), 8);
  memset(XmmRexrReg(m, rde) + 8, 0, 8);
}

static void MovssVpsWps(struct Machine *m, u32 rde) {
  if (IsModrmRegister(rde)) {
    memcpy(XmmRexrReg(m, rde), XmmRexbRm(m, rde), 4);
  } else {
    memcpy(XmmRexrReg(m, rde), ComputeReserveAddressRead4(m, rde), 4);
    memset(XmmRexrReg(m, rde) + 4, 0, 12);
  }
}

static void MovssWpsVps(struct Machine *m, u32 rde) {
  memcpy(GetModrmRegisterXmmPointerWrite4(m, rde), XmmRexrReg(m, rde), 4);
}

static void MovsdVpsWps(struct Machine *m, u32 rde) {
  if (IsModrmRegister(rde)) {
    memcpy(XmmRexrReg(m, rde), XmmRexbRm(m, rde), 8);
  } else {
    memcpy(XmmRexrReg(m, rde), ComputeReserveAddressRead8(m, rde), 8);
    memset(XmmRexrReg(m, rde) + 8, 0, 8);
  }
}

static void MovsdWpsVps(struct Machine *m, u32 rde) {
  memcpy(GetModrmRegisterXmmPointerWrite8(m, rde), XmmRexrReg(m, rde), 8);
}

static void MovhlpsVqUq(struct Machine *m, u32 rde) {
  memcpy(XmmRexrReg(m, rde), XmmRexbRm(m, rde) + 8, 8);
}

static void MovlpsVqMq(struct Machine *m, u32 rde) {
  memcpy(XmmRexrReg(m, rde), ComputeReserveAddressRead8(m, rde), 8);
}

static void MovlpdVqMq(struct Machine *m, u32 rde) {
  memcpy(XmmRexrReg(m, rde), ComputeReserveAddressRead8(m, rde), 8);
}

static void MovddupVqWq(struct Machine *m, u32 rde) {
  u8 *src;
  src = GetModrmRegisterXmmPointerRead8(m, rde);
  memcpy(XmmRexrReg(m, rde) + 0, src, 8);
  memcpy(XmmRexrReg(m, rde) + 8, src, 8);
}

static void MovsldupVqWq(struct Machine *m, u32 rde) {
  u8 *dst, *src;
  dst = XmmRexrReg(m, rde);
  src = GetModrmRegisterXmmPointerRead16(m, rde);
  memcpy(dst + 0 + 0, src + 0, 4);
  memcpy(dst + 0 + 4, src + 0, 4);
  memcpy(dst + 8 + 0, src + 8, 4);
  memcpy(dst + 8 + 4, src + 8, 4);
}

static void MovlpsMqVq(struct Machine *m, u32 rde) {
  memcpy(ComputeReserveAddressWrite8(m, rde), XmmRexrReg(m, rde), 8);
}

static void MovlpdMqVq(struct Machine *m, u32 rde) {
  memcpy(ComputeReserveAddressWrite8(m, rde), XmmRexrReg(m, rde), 8);
}

static void MovlhpsVqUq(struct Machine *m, u32 rde) {
  memcpy(XmmRexrReg(m, rde) + 8, XmmRexbRm(m, rde), 8);
}

static void MovhpsVqMq(struct Machine *m, u32 rde) {
  memcpy(XmmRexrReg(m, rde) + 8, ComputeReserveAddressRead8(m, rde), 8);
}

static void MovhpdVqMq(struct Machine *m, u32 rde) {
  memcpy(XmmRexrReg(m, rde) + 8, ComputeReserveAddressRead8(m, rde), 8);
}

static void MovshdupVqWq(struct Machine *m, u32 rde) {
  u8 *dst, *src;
  dst = XmmRexrReg(m, rde);
  src = GetModrmRegisterXmmPointerRead16(m, rde);
  memcpy(dst + 0 + 0, src + 04, 4);
  memcpy(dst + 0 + 4, src + 04, 4);
  memcpy(dst + 8 + 0, src + 12, 4);
  memcpy(dst + 8 + 4, src + 12, 4);
}

static void MovhpsMqVq(struct Machine *m, u32 rde) {
  memcpy(ComputeReserveAddressWrite8(m, rde), XmmRexrReg(m, rde) + 8, 8);
}

static void MovhpdMqVq(struct Machine *m, u32 rde) {
  memcpy(ComputeReserveAddressWrite8(m, rde), XmmRexrReg(m, rde) + 8, 8);
}

static void MovqWqVq(struct Machine *m, u32 rde) {
  if (IsModrmRegister(rde)) {
    memcpy(XmmRexbRm(m, rde), XmmRexrReg(m, rde), 8);
    memset(XmmRexbRm(m, rde) + 8, 0, 8);
  } else {
    memcpy(ComputeReserveAddressWrite8(m, rde), XmmRexrReg(m, rde), 8);
  }
}

static void Movq2dqVdqNq(struct Machine *m, u32 rde) {
  memcpy(XmmRexrReg(m, rde), MmRm(m, rde), 8);
  memset(XmmRexrReg(m, rde) + 8, 0, 8);
}

static void Movdq2qPqUq(struct Machine *m, u32 rde) {
  memcpy(MmReg(m, rde), XmmRexbRm(m, rde), 8);
}

static void MovapsVpsWps(struct Machine *m, u32 rde) {
  MovdqaVdqWdq(m, rde);
}

static void MovapdVpdWpd(struct Machine *m, u32 rde) {
  MovdqaVdqWdq(m, rde);
}

static void MovapsWpsVps(struct Machine *m, u32 rde) {
  MovdqaWdqVdq(m, rde);
}

static void MovapdWpdVpd(struct Machine *m, u32 rde) {
  MovdqaWdqVdq(m, rde);
}

void OpMovWpsVps(struct Machine *m, u32 rde) {
  switch (m->xedd->op.rep | Osz(rde)) {
    case 0:
      MovupsWpsVps(m, rde);
      break;
    case 1:
      MovupdWpsVps(m, rde);
      break;
    case 2:
      MovsdWpsVps(m, rde);
      break;
    case 3:
      MovssWpsVps(m, rde);
      break;
    default:
      __builtin_unreachable();
  }
}

void OpMov0f28(struct Machine *m, u32 rde) {
  if (!Osz(rde)) {
    MovapsVpsWps(m, rde);
  } else {
    MovapdVpdWpd(m, rde);
  }
}

void OpMov0f6e(struct Machine *m, u32 rde) {
  if (Osz(rde)) {
    if (Rexw(rde)) {
      MovqVdqEqp(m, rde);
    } else {
      MovdVdqEd(m, rde);
    }
  } else {
    if (Rexw(rde)) {
      MovqPqEqp(m, rde);
    } else {
      MovdPqEd(m, rde);
    }
  }
}

void OpMov0f6f(struct Machine *m, u32 rde) {
  if (Osz(rde)) {
    MovdqaVdqWdq(m, rde);
  } else if (m->xedd->op.rep == 3) {
    MovdquVdqWdq(m, rde);
  } else {
    MovqPqQq(m, rde);
  }
}

void OpMov0fE7(struct Machine *m, u32 rde) {
  if (!Osz(rde)) {
    MovntqMqPq(m, rde);
  } else {
    MovntdqMdqVdq(m, rde);
  }
}

void OpMov0f7e(struct Machine *m, u32 rde) {
  if (m->xedd->op.rep == 3) {
    MovqVqWq(m, rde);
  } else if (Osz(rde)) {
    if (Rexw(rde)) {
      MovqEqpVdq(m, rde);
    } else {
      MovdEdVdq(m, rde);
    }
  } else {
    if (Rexw(rde)) {
      MovqEqpPq(m, rde);
    } else {
      MovdEdPq(m, rde);
    }
  }
}

void OpMov0f7f(struct Machine *m, u32 rde) {
  if (m->xedd->op.rep == 3) {
    MovdquWdqVdq(m, rde);
  } else if (Osz(rde)) {
    MovdqaWdqVdq(m, rde);
  } else {
    MovqQqPq(m, rde);
  }
}

void OpMov0f10(struct Machine *m, u32 rde) {
  switch (m->xedd->op.rep | Osz(rde)) {
    case 0:
      MovupsVpsWps(m, rde);
      break;
    case 1:
      MovupdVpsWps(m, rde);
      break;
    case 2:
      MovsdVpsWps(m, rde);
      break;
    case 3:
      MovssVpsWps(m, rde);
      break;
    default:
      __builtin_unreachable();
  }
}

void OpMov0f29(struct Machine *m, u32 rde) {
  if (!Osz(rde)) {
    MovapsWpsVps(m, rde);
  } else {
    MovapdWpdVpd(m, rde);
  }
}

void OpMov0f2b(struct Machine *m, u32 rde) {
  if (!Osz(rde)) {
    MovntpsMpsVps(m, rde);
  } else {
    MovntpdMpdVpd(m, rde);
  }
}

void OpMov0f12(struct Machine *m, u32 rde) {
  switch (m->xedd->op.rep | Osz(rde)) {
    case 0:
      if (IsModrmRegister(rde)) {
        MovhlpsVqUq(m, rde);
      } else {
        MovlpsVqMq(m, rde);
      }
      break;
    case 1:
      MovlpdVqMq(m, rde);
      break;
    case 2:
      MovddupVqWq(m, rde);
      break;
    case 3:
      MovsldupVqWq(m, rde);
      break;
    default:
      __builtin_unreachable();
  }
}

void OpMov0f13(struct Machine *m, u32 rde) {
  if (Osz(rde)) {
    MovlpdMqVq(m, rde);
  } else {
    MovlpsMqVq(m, rde);
  }
}

void OpMov0f16(struct Machine *m, u32 rde) {
  switch (m->xedd->op.rep | Osz(rde)) {
    case 0:
      if (IsModrmRegister(rde)) {
        MovlhpsVqUq(m, rde);
      } else {
        MovhpsVqMq(m, rde);
      }
      break;
    case 1:
      MovhpdVqMq(m, rde);
      break;
    case 3:
      MovshdupVqWq(m, rde);
      break;
    default:
      OpUd(m, rde);
      break;
  }
}

void OpMov0f17(struct Machine *m, u32 rde) {
  if (Osz(rde)) {
    MovhpdMqVq(m, rde);
  } else {
    MovhpsMqVq(m, rde);
  }
}

void OpMov0fD6(struct Machine *m, u32 rde) {
  if (m->xedd->op.rep == 3) {
    Movq2dqVdqNq(m, rde);
  } else if (m->xedd->op.rep == 2) {
    Movdq2qPqUq(m, rde);
  } else if (Osz(rde)) {
    MovqWqVq(m, rde);
  } else {
    OpUd(m, rde);
  }
}

void OpPmovmskbGdqpNqUdq(struct Machine *m, u32 rde) {
  Write64(RegRexrReg(m, rde),
          pmovmskb(XmmRexbRm(m, rde)) & (Osz(rde) ? 0xffff : 0xff));
}

void OpMaskMovDiXmmRegXmmRm(struct Machine *m, u32 rde) {
  void *p[2];
  u64 v;
  unsigned i, n;
  u8 *mem, b[16];
  v = AddressDi(m, rde);
  n = Osz(rde) ? 16 : 8;
  mem = (u8 *)BeginStore(m, v, n, p, b);
  for (i = 0; i < n; ++i) {
    if (XmmRexbRm(m, rde)[i] & 0x80) {
      mem[i] = XmmRexrReg(m, rde)[i];
    }
  }
  EndStore(m, v, n, p, b);
}