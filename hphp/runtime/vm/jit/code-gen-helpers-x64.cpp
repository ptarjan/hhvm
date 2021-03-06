/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/code-gen-helpers-x64.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/ringbuffer.h"
#include "hphp/util/trace.h"

#include "hphp/runtime/base/arch.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/stats.h"
#include "hphp/runtime/base/types.h"
#include "hphp/runtime/vm/jit/back-end.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/mc-generator.h"
#include "hphp/runtime/vm/jit/mc-generator-internal.h"
#include "hphp/runtime/vm/jit/translator.h"
#include "hphp/runtime/vm/jit/ir.h"
#include "hphp/runtime/vm/jit/code-gen-x64.h"
#include "hphp/runtime/vm/jit/vasm-x64.h"

namespace HPHP { namespace JIT { namespace X64 {

//////////////////////////////////////////////////////////////////////

using namespace JIT::reg;

TRACE_SET_MOD(hhir);

//////////////////////////////////////////////////////////////////////

/*
 * It's not normally ok to directly use tracelet abi registers in
 * codegen, unless you're directly dealing with an instruction that
 * does near-end-of-tracelet glue.  (Or also we sometimes use them
 * just for some static_assertions relating to calls to helpers from
 * mcg that hardcode these registers.)
 */

/*
 * Satisfy an alignment constraint. Bridge the gap with int3's.
 */
void moveToAlign(CodeBlock& cb,
                 const size_t align /* =kJmpTargetAlign */) {
  X64Assembler a { cb };
  assert(folly::isPowTwo(align));
  size_t leftInBlock = align - ((align - 1) & uintptr_t(cb.frontier()));
  if (leftInBlock == align) return;
  if (leftInBlock > 2) {
    a.ud2();
    leftInBlock -= 2;
  }
  if (leftInBlock > 0) {
    a.emitInt3s(leftInBlock);
  }
}

void emitEagerSyncPoint(Vout& v, const Op* pc) {
  v << storeq{rVmFp, rVmTl[RDS::kVmfpOff]};
  v << storeq{rVmSp, rVmTl[RDS::kVmspOff]};
  emitImmStoreq(v, intptr_t(pc), rVmTl[RDS::kVmpcOff]);
}

void emitEagerSyncPoint(Asm& as, const Op* pc) {
  emitEagerSyncPoint(Vauto().main(as), pc);
}

// emitEagerVMRegSave --
//   Inline. Saves regs in-place in the TC. This is an unusual need;
//   you probably want to lazily save these regs via recordCall and
//   its ilk.
void emitEagerVMRegSave(Asm& as, RegSaveFlags flags) {
  bool saveFP = bool(flags & RegSaveFlags::SaveFP);
  bool savePC = bool(flags & RegSaveFlags::SavePC);
  assert((flags & ~(RegSaveFlags::SavePC | RegSaveFlags::SaveFP)) ==
         RegSaveFlags::None);

  Reg64 pcReg = rdi;
  assert(!kSpecialCrossTraceRegs.contains(rdi));

  as.   storeq (rVmSp, rVmTl[RDS::kVmspOff]);
  if (savePC) {
    // We're going to temporarily abuse rVmSp to hold the current unit.
    Reg64 rBC = rVmSp;
    as. push   (rBC);
    // m_fp -> m_func -> m_unit -> m_bc + pcReg
    as. loadq  (rVmFp[AROFF(m_func)], rBC);
    as. loadq  (rBC[Func::unitOff()], rBC);
    as. loadq  (rBC[Unit::bcOff()], rBC);
    as. addq   (rBC, pcReg);
    as. storeq (pcReg, rVmTl[RDS::kVmpcOff]);
    as. pop    (rBC);
  }
  if (saveFP) {
    as. storeq (rVmFp, rVmTl[RDS::kVmfpOff]);
  }
}

void emitGetGContext(Vout& v, Vreg dest) {
  emitTLSLoad<ExecutionContext>(v, g_context, dest);
}

void emitGetGContext(Asm& as, PhysReg dest) {
  emitGetGContext(Vauto().main(as), dest);
}

// IfCountNotStatic --
//   Emits if (%reg->_count < 0) { ... }.
//   This depends on UncountedValue and StaticValue
//   being the only valid negative refCounts and both indicating no
//   ref count is needed.
//   May short-circuit this check if the type is known to be
//   static already.
struct IfCountNotStatic {
  typedef CondBlock<FAST_REFCOUNT_OFFSET,
                    0,
                    CC_S,
                    int32_t> NonStaticCondBlock;
  static_assert(UncountedValue < 0 && StaticValue < 0, "");
  NonStaticCondBlock *m_cb; // might be null
  IfCountNotStatic(Asm& as,
                   PhysReg reg,
                   DataType t = KindOfInvalid) {

    // Objects and variants cannot be static
    if (t != KindOfObject && t != KindOfResource && t != KindOfRef) {
      m_cb = new NonStaticCondBlock(as, reg);
    } else {
      m_cb = nullptr;
    }
  }

  ~IfCountNotStatic() {
    delete m_cb;
  }
};


void emitTransCounterInc(Asm& a) {
  if (!mcg->tx().isTransDBEnabled()) return;

  a.    movq (mcg->tx().getTransCounterAddr(), rAsm);
  a.    lock ();
  a.    incq (*rAsm);
}

void emitIncRef(Asm& as, PhysReg base) {
  if (RuntimeOption::EvalHHIRGenerateAsserts) {
    emitAssertRefCount(as, base);
  }
  // emit incref
  as.incl(base[FAST_REFCOUNT_OFFSET]);
  if (RuntimeOption::EvalHHIRGenerateAsserts) {
    // Assert that the ref count is greater than zero
    emitAssertFlagsNonNegative(as);
  }
}

void emitIncRefCheckNonStatic(Asm& as, PhysReg base, DataType dtype) {
  { // if !static then
    IfCountNotStatic ins(as, base, dtype);
    emitIncRef(as, base);
  } // endif
}

void emitIncRefGenericRegSafe(Asm& as, PhysReg base, int disp, PhysReg tmpReg) {
  { // if RC
    IfRefCounted irc(as, base, disp);
    as.   loadq  (base[disp + TVOFF(m_data)], tmpReg);
    { // if !static
      IfCountNotStatic ins(as, tmpReg);
      as. incl(tmpReg[FAST_REFCOUNT_OFFSET]);
    } // endif
  } // endif
}

void emitAssertFlagsNonNegative(Asm& as) {
  emitAssertFlagsNonNegative(Vauto().main(as));
}

void emitAssertFlagsNonNegative(Vout& v) {
  ifThen(v, CC_NGE, [&](Vout& v) { v << ud2{}; });
}

void emitAssertRefCount(Asm& as, PhysReg base) {
  as.cmpl(HPHP::StaticValue, base[FAST_REFCOUNT_OFFSET]);
  ifThen(as, CC_NLE, [&](Asm& a) {
    a.cmpl(HPHP::RefCountMaxRealistic, base[FAST_REFCOUNT_OFFSET]);
    ifThen(a, CC_NBE, [&](Asm& a) { a.ud2(); });
  });
}

// Logical register move: ensures the value in src will be in dest
// after execution, but might do so in strange ways. Do not count on
// being able to smash dest to a different register in the future, e.g.
void emitMovRegReg(Asm& as, PhysReg srcReg, PhysReg dstReg) {
  assert(srcReg != InvalidReg);
  assert(dstReg != InvalidReg);

  if (srcReg == dstReg) return;

  if (srcReg.isGP()) {
    if (dstReg.isGP()) {                 // GP => GP
      as. movq(srcReg, dstReg);
    } else {                             // GP => XMM
      // This generates a movq x86 instruction, which zero extends
      // the 64-bit value in srcReg into a 128-bit XMM register
      as. movq_rx(srcReg, dstReg);
    }
  } else {
    if (dstReg.isGP()) {                 // XMM => GP
      as. movq_xr(srcReg, dstReg);
    } else {                             // XMM => XMM
      // This copies all 128 bits in XMM,
      // thus avoiding partial register stalls
      as. movdqa(srcReg, dstReg);
    }
  }
}

void emitLea(Asm& as, MemoryRef mr, PhysReg dst) {
  if (dst == InvalidReg) return;
  if (mr.r.disp == 0) {
    emitMovRegReg(as, mr.r.base, dst);
  } else {
    as. lea(mr, dst);
  }
}

void emitLdObjClass(Asm& as, PhysReg objReg, PhysReg dstReg) {
  emitLdLowPtr(as, objReg[ObjectData::getVMClassOffset()],
               dstReg, sizeof(LowClassPtr));
}

void emitLdClsCctx(Asm& as, PhysReg srcReg, PhysReg dstReg) {
  emitMovRegReg(as, srcReg, dstReg);
  as.   decq(dstReg);
}

void emitCall(Asm& a, TCA dest) {
  Vauto().main(a) << call{dest};
}

void emitCall(Asm& a, CppCall call) {
  emitCall(Vauto().main(a), call);
}

void emitCall(Vout& v, CppCall target) {
  switch (target.kind()) {
  case CppCall::Kind::Direct:
    v << call{static_cast<TCA>(target.address())};
    return;
  case CppCall::Kind::Virtual:
    // Virtual call.
    // Load method's address from proper offset off of object in rdi,
    // using rax as scratch.
    v << loadq{*rdi, rax};
    v << callm{rax[target.vtableOffset()]};
    return;
  case CppCall::Kind::Indirect:
    v << callr{target.reg()};
    return;
  case CppCall::Kind::ArrayVirt: {
    auto const addr = reinterpret_cast<intptr_t>(target.arrayTable());
    always_assert_flog(
      deltaFits(addr, sz::dword),
      "Array data vtables are expected to be in the data "
      "segment, with addresses less than 2^31"
    );
    v << loadzbl{rdi[ArrayData::offsetofKind()], eax};
    v << callm{baseless(rax*8 + addr)};
    return;
  }
  case CppCall::Kind::Destructor:
    // this movzbl is only needed because callers aren't
    // required to zero-extend the type.
    v << movzbl{target.reg(), target.reg()};
    v << callm{lookupDestructor(v, target.reg())};
    return;
  }
  not_reached();
}

void emitImmStoreq(Vout& v, Immed64 imm, Vptr ref) {
  if (imm.fits(sz::dword)) {
    v << storeqim{imm.l(), ref};
  } else {
    v << storelim{int32_t(imm.q()), ref};
    v << storelim{int32_t(imm.q() >> 32), ref + 4};
  }
}

void emitImmStoreq(Asm& as, Immed64 imm, MemoryRef ref) {
  emitImmStoreq(Vauto().main(as), imm, ref);
}

void emitJmpOrJcc(Asm& a, ConditionCode cc, TCA dest) {
  if (cc == CC_None) {
    a.   jmp(dest);
  } else {
    a.   jcc((ConditionCode)cc, dest);
  }
}

void emitRB(X64Assembler& a,
            Trace::RingBufferType t,
            const char* msg) {
  if (!Trace::moduleEnabledRelease(Trace::ringbuffer, 1)) {
    return;
  }
  PhysRegSaver save(a, kSpecialCrossTraceRegs);
  int arg = 0;
  a.    emitImmReg((uintptr_t)msg, argNumToRegName[arg++]);
  a.    emitImmReg(strlen(msg), argNumToRegName[arg++]);
  a.    emitImmReg(t, argNumToRegName[arg++]);
  a.    call((TCA)Trace::ringbufferMsg);
}

void emitTraceCall(CodeBlock& cb, Offset pcOff) {
  Asm a { cb };
  // call to a trace function
  a.    lea    (rip[(int64_t)a.frontier()], rcx);
  a.    movq   (rVmFp, rdi);
  a.    movq   (rVmSp, rsi);
  a.    movq   (pcOff, rdx);
  // do the call; may use a trampoline
  emitCall(a, reinterpret_cast<TCA>(traceCallback));
}

void emitTestSurpriseFlags(Asm& a) {
  emitTestSurpriseFlags(Vauto().main(a));
}

void emitTestSurpriseFlags(Vout& v) {
  static_assert(RequestInjectionData::LastFlag < (1LL << 32),
                "Translator assumes RequestInjectionFlags fit in 32-bit int");
  v << testlim{-1, rVmTl[RDS::kConditionFlagsOff]};
}

void emitCheckSurpriseFlagsEnter(CodeBlock& mainCode, CodeBlock& coldCode,
                                 Fixup fixup) {
  Vauto vasm;
  auto& v = vasm.main(mainCode);
  auto& vc = vasm.cold(coldCode);
  emitCheckSurpriseFlagsEnter(v, vc, fixup);
}

void emitCheckSurpriseFlagsEnter(Vout& v, Vout& vcold, Fixup fixup) {
  auto cold = vcold.makeBlock();
  auto done = v.makeBlock();
  emitTestSurpriseFlags(v);
  v << jcc{CC_NZ, {done, cold}};

  vcold = cold;
  vcold << movq{rVmFp, argNumToRegName[0]};
  vcold << call{mcg->tx().uniqueStubs.functionEnterHelper};
  vcold << syncpoint{Fixup{fixup.m_pcOffset, fixup.m_spOffset}};
  vcold << jmp{done};
  v = done;
}

void emitLoadReg(Asm& as, MemoryRef mem, PhysReg reg) {
  assert(reg != InvalidReg);
  if (reg.isGP()) {
    as. loadq(mem, reg);
  } else {
    as. movsd(mem, reg);
  }
}

void emitStoreReg(Asm& as, PhysReg reg, MemoryRef mem) {
  assert(reg != InvalidReg);
  if (reg.isGP()) {
    as. storeq(reg, mem);
  } else {
    as. movsd(reg, mem);
  }
}

void emitLdLowPtr(Asm& as, MemoryRef mem, PhysReg reg, size_t size) {
  assert(reg != InvalidReg && reg.isGP());
  if (size == 8) {
    as.loadq(mem, reg);
  } else if (size == 4) {
    as.loadl(mem, r32(reg));
  } else {
    not_implemented();
  }
}

void emitCmpClass(Asm& as, const Class* c, MemoryRef mem) {
  auto size = sizeof(LowClassPtr);
  auto imm = Immed64(c);

  if (size == 8) {
    if (imm.fits(sz::dword)) {
      as.cmpq(imm.l(), mem);
    } else {
      // Use a scratch.  We could do this without rAsm using two immediate
      // 32-bit compares (and two branches).
      as.emitImmReg(imm, rAsm);
      as.cmpq(rAsm, mem);
    }
  } else if (size == 4) {
    as.cmpl(imm.l(), mem);
  } else {
    not_implemented();
  }
}

void emitCmpClass(Asm& as, Reg64 reg, MemoryRef mem) {
  auto size = sizeof(LowClassPtr);
  if (size == 8) {
    as.   cmpq    (reg, mem);
  } else if (size == 4) {
    as.   cmpl    (r32(reg), mem);
  } else {
    not_implemented();
  }
}

void emitCmpClass(Asm& as, Reg64 reg1, PhysReg reg2) {
  auto size = sizeof(LowClassPtr);

  if (size == 8) {
    as.   cmpq    (reg1, reg2);
  } else if (size == 4) {
    as.   cmpl    (r32(reg1), r32(reg2));
  } else {
    not_implemented();
  }
}

void shuffle2(Vout& v, PhysReg s0, PhysReg s1, PhysReg d0, PhysReg d1) {
  if (s0 == InvalidReg && s1 == InvalidReg &&
      d0 == InvalidReg && d1 == InvalidReg) return;
  assert(s0 != s1);
  assert(!s0.isSIMD() || s1 == InvalidReg); // never 2 XMMs
  assert(!d0.isSIMD() || d1 == InvalidReg); // never 2 XMMs
  if (d0 == s1 && d1 != InvalidReg) {
    assert(d0 != d1);
    v << copy2{s0, s1, d0, d1};
  } else if (d0.isSIMD() && s0.isGP() && s1.isGP()) {
    // move 2 gpr to 1 xmm
    assert(d0 != rCgXMM0); // xmm0 is reserved for scratch
    auto x = v.makeReg();
    v << copy{s0, d0};
    v << copy{s1, x};
    v << unpcklpd{x, d0, d0}; // s1 -> d0[1]
  } else {
    if (d0 != InvalidReg) v << copy{s0, d0}; // d0 != s1
    if (d1 != InvalidReg) v << copy{s1, d1};
  }
}

void shuffle2(Asm& as, PhysReg s0, PhysReg s1, PhysReg d0, PhysReg d1) {
  shuffle2(Vauto().main(as), s0, s1, d0, d1);
}

void zeroExtendIfBool(Vout& v, const SSATmp* src, Vreg reg) {
  if (src->isA(Type::Bool) && reg.isValid()) {
    // zero-extend the bool from a byte to a quad
    // note: movzbl actually extends the value to 64 bits.
    v << movzbl{reg, reg};
  }
}

void zeroExtendIfBool(Asm& as, const SSATmp* src, PhysReg reg) {
  if (src->isA(Type::Bool) && reg != InvalidReg) {
    // zero-extend the bool from a byte to a quad
    // note: movzbl actually extends the value to 64 bits.
    as.movzbl(rbyte(reg), r32(reg));
  }
}

ConditionCode opToConditionCode(Opcode opc) {
  switch (opc) {
  case JmpGt:                 return CC_G;
  case JmpGte:                return CC_GE;
  case JmpLt:                 return CC_L;
  case JmpLte:                return CC_LE;
  case JmpEq:                 return CC_E;
  case JmpNeq:                return CC_NE;
  case JmpGtInt:              return CC_G;
  case JmpGteInt:             return CC_GE;
  case JmpLtInt:              return CC_L;
  case JmpLteInt:             return CC_LE;
  case JmpEqInt:              return CC_E;
  case JmpNeqInt:             return CC_NE;
  case JmpSame:               return CC_E;
  case JmpNSame:              return CC_NE;
  case JmpInstanceOfBitmask:  return CC_NZ;
  case JmpNInstanceOfBitmask: return CC_Z;
  case JmpZero:               return CC_Z;
  case JmpNZero:              return CC_NZ;
  case ReqBindJmpGt:                 return CC_G;
  case ReqBindJmpGte:                return CC_GE;
  case ReqBindJmpLt:                 return CC_L;
  case ReqBindJmpLte:                return CC_LE;
  case ReqBindJmpEq:                 return CC_E;
  case ReqBindJmpNeq:                return CC_NE;
  case ReqBindJmpGtInt:              return CC_G;
  case ReqBindJmpGteInt:             return CC_GE;
  case ReqBindJmpLtInt:              return CC_L;
  case ReqBindJmpLteInt:             return CC_LE;
  case ReqBindJmpEqInt:              return CC_E;
  case ReqBindJmpNeqInt:             return CC_NE;
  case ReqBindJmpSame:               return CC_E;
  case ReqBindJmpNSame:              return CC_NE;
  case ReqBindJmpInstanceOfBitmask:  return CC_NZ;
  case ReqBindJmpNInstanceOfBitmask: return CC_Z;
  case ReqBindJmpZero:               return CC_Z;
  case ReqBindJmpNZero:              return CC_NZ;
  case SideExitJmpGt:                 return CC_G;
  case SideExitJmpGte:                return CC_GE;
  case SideExitJmpLt:                 return CC_L;
  case SideExitJmpLte:                return CC_LE;
  case SideExitJmpEq:                 return CC_E;
  case SideExitJmpNeq:                return CC_NE;
  case SideExitJmpGtInt:              return CC_G;
  case SideExitJmpGteInt:             return CC_GE;
  case SideExitJmpLtInt:              return CC_L;
  case SideExitJmpLteInt:             return CC_LE;
  case SideExitJmpEqInt:              return CC_E;
  case SideExitJmpNeqInt:             return CC_NE;
  case SideExitJmpSame:               return CC_E;
  case SideExitJmpNSame:              return CC_NE;
  case SideExitJmpInstanceOfBitmask:  return CC_NZ;
  case SideExitJmpNInstanceOfBitmask: return CC_Z;
  case SideExitJmpZero:               return CC_Z;
  case SideExitJmpNZero:              return CC_NZ;
  default:
    always_assert(0);
  }
}

}}}
