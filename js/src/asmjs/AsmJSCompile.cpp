/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "asmjs/AsmJSCompile.h"
#include "asmjs/AsmJSGlobals.h"

#include "jit/CodeGenerator.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::DebugOnly;
using mozilla::Maybe;

namespace js {
// ModuleCompiler encapsulates the compilation of an entire asm.js module. Over
// the course of an ModuleCompiler object's lifetime, many FunctionCompiler
// objects will be created and destroyed in sequence, one for each function in
// the module.
//
// *** asm.js FFI calls ***
//
// asm.js allows calling out to non-asm.js via "FFI calls". The asm.js type
// system does not place any constraints on the FFI call. In particular:
//  - an FFI call's target is not known or speculated at module-compile time;
//  - a single external function can be called with different signatures.
//
// If performance didn't matter, all FFI calls could simply box their arguments
// and call js::Invoke. However, we'd like to be able to specialize FFI calls
// to be more efficient in several cases:
//
//  - for calls to JS functions which have been jitted, we'd like to call
//    directly into JIT code without going through C++.
//
//  - for calls to certain builtins, we'd like to be call directly into the C++
//    code for the builtin without going through the general call path.
//
// All of this requires dynamic specialization techniques which must happen
// after module compilation. To support this, at module-compilation time, each
// FFI call generates a call signature according to the system ABI, as if the
// callee was a C++ function taking/returning the same types as the caller was
// passing/expecting. The callee is loaded from a fixed offset in the global
// data array which allows the callee to change at runtime. Initially, the
// callee is stub which boxes its arguments and calls js::Invoke.
//
// To do this, we need to generate a callee stub for each pairing of FFI callee
// and signature. We call this pairing an "exit". For example, this code has
// two external functions and three exits:
//
//  function f(global, imports) {
//    "use asm";
//    var foo = imports.foo;
//    var bar = imports.bar;
//    function g() {
//      foo(1);      // Exit #1: (int) -> void
//      foo(1.5);    // Exit #2: (double) -> void
//      bar(1)|0;    // Exit #3: (int) -> int
//      bar(2)|0;    // Exit #3: (int) -> int
//    }
//  }
//
// The ModuleCompiler maintains a hash table (ExitMap) which allows a call site
// to add a new exit or reuse an existing one. The key is an index into the
// Vector<Exit> stored in the AsmJSModule and the value is the signature of
// that exit's variant.
//
// Although ModuleCompiler isn't a MOZ_STACK_CLASS, it has the same rooting
// properties as the ModuleValidator, and a shorter lifetime: so it is marked
// as rooted in the in the rooting analysis. Don't add non-JSATom pointers, or
// this will break!
class ModuleCompiler
{
    ModuleCompileInputs                     compileInputs_;
    ScopedJSDeletePtr<ModuleCompileResults> compileResults_;

  public:
    explicit ModuleCompiler(const ModuleCompileInputs& inputs)
      : compileInputs_(inputs)
    {}

    bool init() {
        compileResults_.reset(js_new<ModuleCompileResults>());
        return !!compileResults_;
    }

    /*************************************************** Read-only interface */

    MacroAssembler& masm()          { return compileResults_->masm(); }
    Label& stackOverflowLabel()     { return compileResults_->stackOverflowLabel(); }
    Label& asyncInterruptLabel()    { return compileResults_->asyncInterruptLabel(); }
    Label& syncInterruptLabel()     { return compileResults_->syncInterruptLabel(); }
    Label& onOutOfBoundsLabel()     { return compileResults_->onOutOfBoundsLabel(); }
    Label& onConversionErrorLabel() { return compileResults_->onConversionErrorLabel(); }
    int64_t usecBefore()            { return compileResults_->usecBefore(); }

    bool usesSignalHandlersForOOB() const   { return compileInputs_.usesSignalHandlersForOOB; }
    CompileRuntime* runtime() const         { return compileInputs_.runtime; }
    CompileCompartment* compartment() const { return compileInputs_.compartment; }

    /***************************************************** Mutable interface */

    void finish(ScopedJSDeletePtr<ModuleCompileResults>* results) {
        *results = compileResults_.forget();
    }
};

} // namespace js

enum class AsmType : uint8_t {
    Int32,
    Float32,
    Float64,
    Int32x4,
    Float32x4
};

typedef Vector<size_t, 1, SystemAllocPolicy> LabelVector;
typedef Vector<MBasicBlock*, 8, SystemAllocPolicy> BlockVector;

// Encapsulates the compilation of a single function in an asm.js module. The
// function compiler handles the creation and final backend compilation of the
// MIR graph. Also see ModuleCompiler comment.
class FunctionCompiler
{
  private:
    typedef HashMap<uint32_t, BlockVector, DefaultHasher<uint32_t>, SystemAllocPolicy> LabeledBlockMap;
    typedef HashMap<size_t, BlockVector, DefaultHasher<uint32_t>, SystemAllocPolicy> UnlabeledBlockMap;
    typedef Vector<size_t, 4, SystemAllocPolicy> PositionStack;
    typedef Vector<Type, 4, SystemAllocPolicy> LocalVarTypes;

    ModuleCompiler &         m_;
    LifoAlloc &              lifo_;

    const AsmFunction &      func_;
    size_t                   pc_;

    TempAllocator *          alloc_;
    MIRGraph *               graph_;
    CompileInfo *            info_;
    MIRGenerator *           mirGen_;
    Maybe<JitContext>        jitContext_;

    MBasicBlock *            curBlock_;

    PositionStack            loopStack_;
    PositionStack            breakableStack_;
    UnlabeledBlockMap        unlabeledBreaks_;
    UnlabeledBlockMap        unlabeledContinues_;
    LabeledBlockMap          labeledBreaks_;
    LabeledBlockMap          labeledContinues_;

    LocalVarTypes            localVarTypes_;

  public:
    FunctionCompiler(ModuleCompiler& m, const AsmFunction& func, LifoAlloc& lifo)
      : m_(m),
        lifo_(lifo),
        func_(func),
        pc_(0),
        alloc_(nullptr),
        graph_(nullptr),
        info_(nullptr),
        mirGen_(nullptr),
        curBlock_(nullptr)
    {}

    ModuleCompiler & m() const            { return m_; }
    TempAllocator &  alloc() const        { return *alloc_; }
    LifoAlloc &      lifo() const         { return lifo_; }
    RetType          returnedType() const { return func_.returnedType(); }

    bool init()
    {
        return unlabeledBreaks_.init() &&
               unlabeledContinues_.init() &&
               labeledBreaks_.init() &&
               labeledContinues_.init();
    }

    void checkPostconditions()
    {
        MOZ_ASSERT(loopStack_.empty());
        MOZ_ASSERT(unlabeledBreaks_.empty());
        MOZ_ASSERT(unlabeledContinues_.empty());
        MOZ_ASSERT(labeledBreaks_.empty());
        MOZ_ASSERT(labeledContinues_.empty());
        MOZ_ASSERT(inDeadCode());
        MOZ_ASSERT(pc_ == func_.size(), "all bytecode must be consumed");
    }

    /************************* Read-only interface (after local scope setup) */

    MIRGenerator & mirGen() const     { MOZ_ASSERT(mirGen_); return *mirGen_; }
    MIRGraph &     mirGraph() const   { MOZ_ASSERT(graph_); return *graph_; }
    CompileInfo &  info() const       { MOZ_ASSERT(info_); return *info_; }

    MDefinition* getLocalDef(unsigned slot)
    {
        if (inDeadCode())
            return nullptr;
        return curBlock_->getSlot(info().localSlot(slot));
    }

    /***************************** Code generation (after local scope setup) */

    MDefinition* constant(const SimdConstant& v, MIRType type)
    {
        if (inDeadCode())
            return nullptr;
        MInstruction* constant;
        constant = MSimdConstant::New(alloc(), v, type);
        curBlock_->add(constant);
        return constant;
    }

    MDefinition* constant(Value v, MIRType type)
    {
        if (inDeadCode())
            return nullptr;
        MConstant* constant = MConstant::NewAsmJS(alloc(), v, type);
        curBlock_->add(constant);
        return constant;
    }

    template <class T>
    MDefinition* unary(MDefinition* op)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::NewAsmJS(alloc(), op);
        curBlock_->add(ins);
        return ins;
    }

    template <class T>
    MDefinition* unary(MDefinition* op, MIRType type)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::NewAsmJS(alloc(), op, type);
        curBlock_->add(ins);
        return ins;
    }

    template <class T>
    MDefinition* binary(MDefinition* lhs, MDefinition* rhs)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::New(alloc(), lhs, rhs);
        curBlock_->add(ins);
        return ins;
    }

    template <class T>
    MDefinition* binary(MDefinition* lhs, MDefinition* rhs, MIRType type)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::NewAsmJS(alloc(), lhs, rhs, type);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* unarySimd(MDefinition* input, MSimdUnaryArith::Operation op, MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(input->type()) && input->type() == type);
        MInstruction* ins = MSimdUnaryArith::NewAsmJS(alloc(), input, op, type);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* binarySimd(MDefinition* lhs, MDefinition* rhs, MSimdBinaryArith::Operation op,
                            MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(lhs->type()) && rhs->type() == lhs->type());
        MOZ_ASSERT(lhs->type() == type);
        MSimdBinaryArith* ins = MSimdBinaryArith::NewAsmJS(alloc(), lhs, rhs, op, type);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* binarySimd(MDefinition* lhs, MDefinition* rhs, MSimdBinaryBitwise::Operation op,
                            MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(lhs->type()) && rhs->type() == lhs->type());
        MOZ_ASSERT(lhs->type() == type);
        MSimdBinaryBitwise* ins = MSimdBinaryBitwise::NewAsmJS(alloc(), lhs, rhs, op, type);
        curBlock_->add(ins);
        return ins;
    }

    template<class T>
    MDefinition* binarySimd(MDefinition* lhs, MDefinition* rhs, typename T::Operation op)
    {
        if (inDeadCode())
            return nullptr;

        T* ins = T::NewAsmJS(alloc(), lhs, rhs, op);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* swizzleSimd(MDefinition* vector, int32_t X, int32_t Y, int32_t Z, int32_t W,
                             MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MSimdSwizzle* ins = MSimdSwizzle::New(alloc(), vector, type, X, Y, Z, W);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* shuffleSimd(MDefinition* lhs, MDefinition* rhs, int32_t X, int32_t Y,
                             int32_t Z, int32_t W, MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MInstruction* ins = MSimdShuffle::New(alloc(), lhs, rhs, type, X, Y, Z, W);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* insertElementSimd(MDefinition* vec, MDefinition* val, SimdLane lane, MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(vec->type()) && vec->type() == type);
        MOZ_ASSERT(!IsSimdType(val->type()));
        MSimdInsertElement* ins = MSimdInsertElement::NewAsmJS(alloc(), vec, val, type, lane);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* selectSimd(MDefinition* mask, MDefinition* lhs, MDefinition* rhs, MIRType type,
                            bool isElementWise)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(mask->type()));
        MOZ_ASSERT(mask->type() == MIRType_Int32x4);
        MOZ_ASSERT(IsSimdType(lhs->type()) && rhs->type() == lhs->type());
        MOZ_ASSERT(lhs->type() == type);
        MSimdSelect* ins = MSimdSelect::NewAsmJS(alloc(), mask, lhs, rhs, type, isElementWise);
        curBlock_->add(ins);
        return ins;
    }

    template<class T>
    MDefinition* convertSimd(MDefinition* vec, MIRType from, MIRType to)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(from) && IsSimdType(to) && from != to);
        T* ins = T::NewAsmJS(alloc(), vec, from, to);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* splatSimd(MDefinition* v, MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(type));
        MSimdSplatX4* ins = MSimdSplatX4::NewAsmJS(alloc(), v, type);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* minMax(MDefinition* lhs, MDefinition* rhs, MIRType type, bool isMax) {
        if (inDeadCode())
            return nullptr;
        MMinMax* ins = MMinMax::New(alloc(), lhs, rhs, type, isMax);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* mul(MDefinition* lhs, MDefinition* rhs, MIRType type, MMul::Mode mode)
    {
        if (inDeadCode())
            return nullptr;
        MMul* ins = MMul::New(alloc(), lhs, rhs, type, mode);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* div(MDefinition* lhs, MDefinition* rhs, MIRType type, bool unsignd)
    {
        if (inDeadCode())
            return nullptr;
        MDiv* ins = MDiv::NewAsmJS(alloc(), lhs, rhs, type, unsignd);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* mod(MDefinition* lhs, MDefinition* rhs, MIRType type, bool unsignd)
    {
        if (inDeadCode())
            return nullptr;
        MMod* ins = MMod::NewAsmJS(alloc(), lhs, rhs, type, unsignd);
        curBlock_->add(ins);
        return ins;
    }

    template <class T>
    MDefinition* bitwise(MDefinition* lhs, MDefinition* rhs)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::NewAsmJS(alloc(), lhs, rhs);
        curBlock_->add(ins);
        return ins;
    }

    template <class T>
    MDefinition* bitwise(MDefinition* op)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::NewAsmJS(alloc(), op);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* compare(MDefinition* lhs, MDefinition* rhs, JSOp op, MCompare::CompareType type)
    {
        if (inDeadCode())
            return nullptr;
        MCompare* ins = MCompare::NewAsmJS(alloc(), lhs, rhs, op, type);
        curBlock_->add(ins);
        return ins;
    }

    void assign(unsigned slot, MDefinition* def)
    {
        if (inDeadCode())
            return;
        curBlock_->setSlot(info().localSlot(slot), def);
    }

    MDefinition* loadHeap(Scalar::Type accessType, MDefinition* ptr, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return nullptr;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK;
        MOZ_ASSERT(!Scalar::isSimdType(accessType), "SIMD loads should use loadSimdHeap");
        MAsmJSLoadHeap* load = MAsmJSLoadHeap::New(alloc(), accessType, ptr, needsBoundsCheck);
        curBlock_->add(load);
        return load;
    }

    MDefinition* loadSimdHeap(Scalar::Type accessType, MDefinition* ptr, NeedsBoundsCheck chk,
                              unsigned numElems)
    {
        if (inDeadCode())
            return nullptr;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK;
        MOZ_ASSERT(Scalar::isSimdType(accessType), "loadSimdHeap can only load from a SIMD view");
        MAsmJSLoadHeap* load = MAsmJSLoadHeap::New(alloc(), accessType, ptr, needsBoundsCheck,
                                                   numElems);
        curBlock_->add(load);
        return load;
    }

    void storeHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* v, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK;
        MOZ_ASSERT(!Scalar::isSimdType(accessType), "SIMD stores should use loadSimdHeap");
        MAsmJSStoreHeap* store = MAsmJSStoreHeap::New(alloc(), accessType, ptr, v, needsBoundsCheck);
        curBlock_->add(store);
    }

    void storeSimdHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* v,
                       NeedsBoundsCheck chk, unsigned numElems)
    {
        if (inDeadCode())
            return;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK;
        MOZ_ASSERT(Scalar::isSimdType(accessType), "storeSimdHeap can only load from a SIMD view");
        MAsmJSStoreHeap* store = MAsmJSStoreHeap::New(alloc(), accessType, ptr, v, needsBoundsCheck,
                                                      numElems);
        curBlock_->add(store);
    }

    void memoryBarrier(MemoryBarrierBits type)
    {
        if (inDeadCode())
            return;
        MMemoryBarrier* ins = MMemoryBarrier::New(alloc(), type);
        curBlock_->add(ins);
    }

    MDefinition* atomicLoadHeap(Scalar::Type accessType, MDefinition* ptr, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return nullptr;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK;
        MAsmJSLoadHeap* load = MAsmJSLoadHeap::New(alloc(), accessType, ptr, needsBoundsCheck,
                                                   /* numElems */ 0,
                                                   MembarBeforeLoad, MembarAfterLoad);
        curBlock_->add(load);
        return load;
    }

    void atomicStoreHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* v, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK;
        MAsmJSStoreHeap* store = MAsmJSStoreHeap::New(alloc(), accessType, ptr, v, needsBoundsCheck,
                                                      /* numElems = */ 0,
                                                      MembarBeforeStore, MembarAfterStore);
        curBlock_->add(store);
    }

    MDefinition* atomicCompareExchangeHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* oldv,
                                           MDefinition* newv, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return nullptr;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK;
        MAsmJSCompareExchangeHeap* cas =
            MAsmJSCompareExchangeHeap::New(alloc(), accessType, ptr, oldv, newv, needsBoundsCheck);
        curBlock_->add(cas);
        return cas;
    }

    MDefinition* atomicExchangeHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* value,
                                    NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return nullptr;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK;
        MAsmJSAtomicExchangeHeap* cas =
            MAsmJSAtomicExchangeHeap::New(alloc(), accessType, ptr, value, needsBoundsCheck);
        curBlock_->add(cas);
        return cas;
    }

    MDefinition* atomicBinopHeap(js::jit::AtomicOp op, Scalar::Type accessType, MDefinition* ptr,
                                 MDefinition* v, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return nullptr;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK;
        MAsmJSAtomicBinopHeap* binop =
            MAsmJSAtomicBinopHeap::New(alloc(), op, accessType, ptr, v, needsBoundsCheck);
        curBlock_->add(binop);
        return binop;
    }

    MDefinition* loadGlobalVar(unsigned globalDataOffset, bool isConst, MIRType type)
    {
        if (inDeadCode())
            return nullptr;
        MAsmJSLoadGlobalVar* load = MAsmJSLoadGlobalVar::New(alloc(), type, globalDataOffset,
                                                             isConst);
        curBlock_->add(load);
        return load;
    }

    void storeGlobalVar(uint32_t globalDataOffset, MDefinition* v)
    {
        if (inDeadCode())
            return;
        curBlock_->add(MAsmJSStoreGlobalVar::New(alloc(), globalDataOffset, v));
    }

    void addInterruptCheck(unsigned lineno, unsigned column)
    {
        if (inDeadCode())
            return;

        CallSiteDesc callDesc(lineno, column, CallSiteDesc::Relative);
        curBlock_->add(MAsmJSInterruptCheck::New(alloc(), &m().syncInterruptLabel(), callDesc));
    }

    MDefinition* extractSimdElement(SimdLane lane, MDefinition* base, MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(base->type()));
        MOZ_ASSERT(!IsSimdType(type));
        MSimdExtractElement* ins = MSimdExtractElement::NewAsmJS(alloc(), base, type, lane);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* extractSignMask(MDefinition* base)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(base->type()));
        MSimdSignMask* ins = MSimdSignMask::NewAsmJS(alloc(), base);
        curBlock_->add(ins);
        return ins;
    }

    template<typename T>
    MDefinition* constructSimd(MDefinition* x, MDefinition* y, MDefinition* z, MDefinition* w,
                               MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(type));
        T* ins = T::NewAsmJS(alloc(), type, x, y, z, w);
        curBlock_->add(ins);
        return ins;
    }

    /***************************************************************** Calls */

    // The IonMonkey backend maintains a single stack offset (from the stack
    // pointer to the base of the frame) by adding the total amount of spill
    // space required plus the maximum stack required for argument passing.
    // Since we do not use IonMonkey's MPrepareCall/MPassArg/MCall, we must
    // manually accumulate, for the entire function, the maximum required stack
    // space for argument passing. (This is passed to the CodeGenerator via
    // MIRGenerator::maxAsmJSStackArgBytes.) Naively, this would just be the
    // maximum of the stack space required for each individual call (as
    // determined by the call ABI). However, as an optimization, arguments are
    // stored to the stack immediately after evaluation (to decrease live
    // ranges and reduce spilling). This introduces the complexity that,
    // between evaluating an argument and making the call, another argument
    // evaluation could perform a call that also needs to store to the stack.
    // When this occurs childClobbers_ = true and the parent expression's
    // arguments are stored above the maximum depth clobbered by a child
    // expression.

    class Call
    {
        uint32_t lineno_;
        uint32_t column_;
        ABIArgGenerator abi_;
        uint32_t prevMaxStackBytes_;
        uint32_t maxChildStackBytes_;
        uint32_t spIncrement_;
        MAsmJSCall::Args regArgs_;
        Vector<MAsmJSPassStackArg*, 0, SystemAllocPolicy> stackArgs_;
        bool childClobbers_;

        friend class FunctionCompiler;

      public:
        Call(FunctionCompiler& f, uint32_t lineno, uint32_t column)
          : lineno_(lineno),
            column_(column),
            prevMaxStackBytes_(0),
            maxChildStackBytes_(0),
            spIncrement_(0),
            childClobbers_(false)
        { }
    };

    void startCallArgs(Call* call)
    {
        if (inDeadCode())
            return;
        call->prevMaxStackBytes_ = mirGen().resetAsmJSMaxStackArgBytes();
    }

    bool passArg(MDefinition* argDef, MIRType mirType, Call* call)
    {
        if (inDeadCode())
            return true;

        uint32_t childStackBytes = mirGen().resetAsmJSMaxStackArgBytes();
        call->maxChildStackBytes_ = Max(call->maxChildStackBytes_, childStackBytes);
        if (childStackBytes > 0 && !call->stackArgs_.empty())
            call->childClobbers_ = true;

        ABIArg arg = call->abi_.next(mirType);
        if (arg.kind() == ABIArg::Stack) {
            MAsmJSPassStackArg* mir = MAsmJSPassStackArg::New(alloc(), arg.offsetFromArgBase(),
                                                              argDef);
            curBlock_->add(mir);
            if (!call->stackArgs_.append(mir))
                return false;
        } else {
            if (!call->regArgs_.append(MAsmJSCall::Arg(arg.reg(), argDef)))
                return false;
        }
        return true;
    }

    void finishCallArgs(Call* call)
    {
        if (inDeadCode())
            return;
        uint32_t parentStackBytes = call->abi_.stackBytesConsumedSoFar();
        uint32_t newStackBytes;
        if (call->childClobbers_) {
            call->spIncrement_ = AlignBytes(call->maxChildStackBytes_, AsmJSStackAlignment);
            for (unsigned i = 0; i < call->stackArgs_.length(); i++)
                call->stackArgs_[i]->incrementOffset(call->spIncrement_);
            newStackBytes = Max(call->prevMaxStackBytes_,
                                call->spIncrement_ + parentStackBytes);
        } else {
            call->spIncrement_ = 0;
            newStackBytes = Max(call->prevMaxStackBytes_,
                                Max(call->maxChildStackBytes_, parentStackBytes));
        }
        mirGen_->setAsmJSMaxStackArgBytes(newStackBytes);
    }

  private:
    bool callPrivate(MAsmJSCall::Callee callee, const Call& call, MIRType returnType, MDefinition** def)
    {
        if (inDeadCode()) {
            *def = nullptr;
            return true;
        }

        CallSiteDesc::Kind kind = CallSiteDesc::Kind(-1);
        switch (callee.which()) {
          case MAsmJSCall::Callee::Internal: kind = CallSiteDesc::Relative; break;
          case MAsmJSCall::Callee::Dynamic:  kind = CallSiteDesc::Register; break;
          case MAsmJSCall::Callee::Builtin:  kind = CallSiteDesc::Register; break;
        }

        MAsmJSCall* ins = MAsmJSCall::New(alloc(), CallSiteDesc(call.lineno_, call.column_, kind),
                                          callee, call.regArgs_, returnType, call.spIncrement_);
        if (!ins)
            return false;

        curBlock_->add(ins);
        *def = ins;
        return true;
    }

  public:
    bool internalCall(const Signature& sig, uint32_t funcIndex, const Call& call, MDefinition** def)
    {
        MIRType returnType = sig.retType().toMIRType();
        return callPrivate(MAsmJSCall::Callee(AsmJSInternalCallee(funcIndex)), call, returnType, def);
    }

    bool funcPtrCall(const Signature& sig, uint32_t maskLit, uint32_t globalDataOffset, MDefinition* index,
                     const Call& call, MDefinition** def)
    {
        if (inDeadCode()) {
            *def = nullptr;
            return true;
        }

        MConstant* mask = MConstant::New(alloc(), Int32Value(maskLit));
        curBlock_->add(mask);
        MBitAnd* maskedIndex = MBitAnd::NewAsmJS(alloc(), index, mask);
        curBlock_->add(maskedIndex);
        MAsmJSLoadFuncPtr* ptrFun = MAsmJSLoadFuncPtr::New(alloc(), globalDataOffset, maskedIndex);
        curBlock_->add(ptrFun);

        MIRType returnType = sig.retType().toMIRType();
        return callPrivate(MAsmJSCall::Callee(ptrFun), call, returnType, def);
    }

    bool ffiCall(unsigned globalDataOffset, const Call& call, MIRType returnType, MDefinition** def)
    {
        if (inDeadCode()) {
            *def = nullptr;
            return true;
        }

        MAsmJSLoadFFIFunc* ptrFun = MAsmJSLoadFFIFunc::New(alloc(), globalDataOffset);
        curBlock_->add(ptrFun);

        return callPrivate(MAsmJSCall::Callee(ptrFun), call, returnType, def);
    }

    bool builtinCall(AsmJSImmKind builtin, const Call& call, MIRType returnType, MDefinition** def)
    {
        return callPrivate(MAsmJSCall::Callee(builtin), call, returnType, def);
    }

    /*********************************************** Control flow generation */

    inline bool inDeadCode() const {
        return curBlock_ == nullptr;
    }

    void returnExpr(MDefinition* expr)
    {
        if (inDeadCode())
            return;
        MAsmJSReturn* ins = MAsmJSReturn::New(alloc(), expr);
        curBlock_->end(ins);
        curBlock_ = nullptr;
    }

    void returnVoid()
    {
        if (inDeadCode())
            return;
        MAsmJSVoidReturn* ins = MAsmJSVoidReturn::New(alloc());
        curBlock_->end(ins);
        curBlock_ = nullptr;
    }

    bool branchAndStartThen(MDefinition* cond, MBasicBlock** thenBlock, MBasicBlock** elseBlock)
    {
        if (inDeadCode())
            return true;

        bool hasThenBlock = *thenBlock != nullptr;
        bool hasElseBlock = *elseBlock != nullptr;

        if (!hasThenBlock && !newBlock(curBlock_, thenBlock))
            return false;
        if (!hasElseBlock && !newBlock(curBlock_, elseBlock))
            return false;

        curBlock_->end(MTest::New(alloc(), cond, *thenBlock, *elseBlock));

        // Only add as a predecessor if newBlock hasn't been called (as it does it for us)
        if (hasThenBlock && !(*thenBlock)->addPredecessor(alloc(), curBlock_))
            return false;
        if (hasElseBlock && !(*elseBlock)->addPredecessor(alloc(), curBlock_))
            return false;

        curBlock_ = *thenBlock;
        mirGraph().moveBlockToEnd(curBlock_);
        return true;
    }

    void assertCurrentBlockIs(MBasicBlock* block) {
        if (inDeadCode())
            return;
        MOZ_ASSERT(curBlock_ == block);
    }

    bool appendThenBlock(BlockVector* thenBlocks)
    {
        if (inDeadCode())
            return true;
        return thenBlocks->append(curBlock_);
    }

    bool joinIf(const BlockVector& thenBlocks, MBasicBlock* joinBlock)
    {
        if (!joinBlock)
            return true;
        MOZ_ASSERT_IF(curBlock_, thenBlocks.back() == curBlock_);
        for (size_t i = 0; i < thenBlocks.length(); i++) {
            thenBlocks[i]->end(MGoto::New(alloc(), joinBlock));
            if (!joinBlock->addPredecessor(alloc(), thenBlocks[i]))
                return false;
        }
        curBlock_ = joinBlock;
        mirGraph().moveBlockToEnd(curBlock_);
        return true;
    }

    void switchToElse(MBasicBlock* elseBlock)
    {
        if (!elseBlock)
            return;
        curBlock_ = elseBlock;
        mirGraph().moveBlockToEnd(curBlock_);
    }

    bool joinIfElse(const BlockVector& thenBlocks)
    {
        if (inDeadCode() && thenBlocks.empty())
            return true;
        MBasicBlock* pred = curBlock_ ? curBlock_ : thenBlocks[0];
        MBasicBlock* join;
        if (!newBlock(pred, &join))
            return false;
        if (curBlock_)
            curBlock_->end(MGoto::New(alloc(), join));
        for (size_t i = 0; i < thenBlocks.length(); i++) {
            thenBlocks[i]->end(MGoto::New(alloc(), join));
            if (pred == curBlock_ || i > 0) {
                if (!join->addPredecessor(alloc(), thenBlocks[i]))
                    return false;
            }
        }
        curBlock_ = join;
        return true;
    }

    void pushPhiInput(MDefinition* def)
    {
        if (inDeadCode())
            return;
        MOZ_ASSERT(curBlock_->stackDepth() == info().firstStackSlot());
        curBlock_->push(def);
    }

    MDefinition* popPhiOutput()
    {
        if (inDeadCode())
            return nullptr;
        MOZ_ASSERT(curBlock_->stackDepth() == info().firstStackSlot() + 1);
        return curBlock_->pop();
    }

    bool startPendingLoop(size_t pos, MBasicBlock** loopEntry)
    {
        if (!loopStack_.append(pos) || !breakableStack_.append(pos))
            return false;
        if (inDeadCode()) {
            *loopEntry = nullptr;
            return true;
        }
        MOZ_ASSERT(curBlock_->loopDepth() == loopStack_.length() - 1);
        *loopEntry = MBasicBlock::NewAsmJS(mirGraph(), info(), curBlock_,
                                           MBasicBlock::PENDING_LOOP_HEADER);
        if (!*loopEntry)
            return false;
        mirGraph().addBlock(*loopEntry);
        (*loopEntry)->setLoopDepth(loopStack_.length());
        curBlock_->end(MGoto::New(alloc(), *loopEntry));
        curBlock_ = *loopEntry;
        return true;
    }

    bool branchAndStartLoopBody(MDefinition* cond, MBasicBlock** afterLoop)
    {
        if (inDeadCode()) {
            *afterLoop = nullptr;
            return true;
        }
        MOZ_ASSERT(curBlock_->loopDepth() > 0);
        MBasicBlock* body;
        if (!newBlock(curBlock_, &body))
            return false;
        if (cond->isConstant() && cond->toConstant()->valueToBoolean()) {
            *afterLoop = nullptr;
            curBlock_->end(MGoto::New(alloc(), body));
        } else {
            if (!newBlockWithDepth(curBlock_, curBlock_->loopDepth() - 1, afterLoop))
                return false;
            curBlock_->end(MTest::New(alloc(), cond, body, *afterLoop));
        }
        curBlock_ = body;
        return true;
    }

  private:
    size_t popLoop()
    {
        size_t pos = loopStack_.popCopy();
        MOZ_ASSERT(!unlabeledContinues_.has(pos));
        breakableStack_.popBack();
        return pos;
    }

    void fixupRedundantPhis(MBasicBlock* b)
    {
        for (size_t i = 0, depth = b->stackDepth(); i < depth; i++) {
            MDefinition* def = b->getSlot(i);
            if (def->isUnused())
                b->setSlot(i, def->toPhi()->getOperand(0));
        }
    }
    template <typename T>
    void fixupRedundantPhis(MBasicBlock* loopEntry, T& map)
    {
        if (!map.initialized())
            return;
        for (typename T::Enum e(map); !e.empty(); e.popFront()) {
            BlockVector& blocks = e.front().value();
            for (size_t i = 0; i < blocks.length(); i++) {
                if (blocks[i]->loopDepth() >= loopEntry->loopDepth())
                    fixupRedundantPhis(blocks[i]);
            }
        }
    }
    bool setLoopBackedge(MBasicBlock* loopEntry, MBasicBlock* backedge, MBasicBlock* afterLoop)
    {
        if (!loopEntry->setBackedgeAsmJS(backedge))
            return false;

        // Flag all redundant phis as unused.
        for (MPhiIterator phi = loopEntry->phisBegin(); phi != loopEntry->phisEnd(); phi++) {
            MOZ_ASSERT(phi->numOperands() == 2);
            if (phi->getOperand(0) == phi->getOperand(1))
                phi->setUnused();
        }

        // Fix up phis stored in the slots Vector of pending blocks.
        if (afterLoop)
            fixupRedundantPhis(afterLoop);
        fixupRedundantPhis(loopEntry, labeledContinues_);
        fixupRedundantPhis(loopEntry, labeledBreaks_);
        fixupRedundantPhis(loopEntry, unlabeledContinues_);
        fixupRedundantPhis(loopEntry, unlabeledBreaks_);

        // Discard redundant phis and add to the free list.
        for (MPhiIterator phi = loopEntry->phisBegin(); phi != loopEntry->phisEnd(); ) {
            MPhi* entryDef = *phi++;
            if (!entryDef->isUnused())
                continue;

            entryDef->justReplaceAllUsesWith(entryDef->getOperand(0));
            loopEntry->discardPhi(entryDef);
            mirGraph().addPhiToFreeList(entryDef);
        }

        return true;
    }

  public:
    bool closeLoop(MBasicBlock* loopEntry, MBasicBlock* afterLoop)
    {
        size_t pos = popLoop();
        if (!loopEntry) {
            MOZ_ASSERT(!afterLoop);
            MOZ_ASSERT(inDeadCode());
            MOZ_ASSERT(!unlabeledBreaks_.has(pos));
            return true;
        }
        MOZ_ASSERT(loopEntry->loopDepth() == loopStack_.length() + 1);
        MOZ_ASSERT_IF(afterLoop, afterLoop->loopDepth() == loopStack_.length());
        if (curBlock_) {
            MOZ_ASSERT(curBlock_->loopDepth() == loopStack_.length() + 1);
            curBlock_->end(MGoto::New(alloc(), loopEntry));
            if (!setLoopBackedge(loopEntry, curBlock_, afterLoop))
                return false;
        }
        curBlock_ = afterLoop;
        if (curBlock_)
            mirGraph().moveBlockToEnd(curBlock_);
        return bindUnlabeledBreaks(pos);
    }

    bool branchAndCloseDoWhileLoop(MDefinition* cond, MBasicBlock* loopEntry)
    {
        size_t pos = popLoop();
        if (!loopEntry) {
            MOZ_ASSERT(inDeadCode());
            MOZ_ASSERT(!unlabeledBreaks_.has(pos));
            return true;
        }
        MOZ_ASSERT(loopEntry->loopDepth() == loopStack_.length() + 1);
        if (curBlock_) {
            MOZ_ASSERT(curBlock_->loopDepth() == loopStack_.length() + 1);
            if (cond->isConstant()) {
                if (cond->toConstant()->valueToBoolean()) {
                    curBlock_->end(MGoto::New(alloc(), loopEntry));
                    if (!setLoopBackedge(loopEntry, curBlock_, nullptr))
                        return false;
                    curBlock_ = nullptr;
                } else {
                    MBasicBlock* afterLoop;
                    if (!newBlock(curBlock_, &afterLoop))
                        return false;
                    curBlock_->end(MGoto::New(alloc(), afterLoop));
                    curBlock_ = afterLoop;
                }
            } else {
                MBasicBlock* afterLoop;
                if (!newBlock(curBlock_, &afterLoop))
                    return false;
                curBlock_->end(MTest::New(alloc(), cond, loopEntry, afterLoop));
                if (!setLoopBackedge(loopEntry, curBlock_, afterLoop))
                    return false;
                curBlock_ = afterLoop;
            }
        }
        return bindUnlabeledBreaks(pos);
    }

    bool bindContinues(size_t pos, const LabelVector* maybeLabels)
    {
        bool createdJoinBlock = false;
        if (UnlabeledBlockMap::Ptr p = unlabeledContinues_.lookup(pos)) {
            if (!bindBreaksOrContinues(&p->value(), &createdJoinBlock))
                return false;
            unlabeledContinues_.remove(p);
        }
        return bindLabeledBreaksOrContinues(maybeLabels, &labeledContinues_, &createdJoinBlock);
    }

    bool bindLabeledBreaks(const LabelVector* maybeLabels)
    {
        bool createdJoinBlock = false;
        return bindLabeledBreaksOrContinues(maybeLabels, &labeledBreaks_, &createdJoinBlock);
    }

    bool addBreak(uint32_t* maybeLabelId) {
        if (maybeLabelId)
            return addBreakOrContinue(*maybeLabelId, &labeledBreaks_);
        return addBreakOrContinue(breakableStack_.back(), &unlabeledBreaks_);
    }

    bool addContinue(uint32_t* maybeLabelId) {
        if (maybeLabelId)
            return addBreakOrContinue(*maybeLabelId, &labeledContinues_);
        return addBreakOrContinue(loopStack_.back(), &unlabeledContinues_);
    }

    bool startSwitch(size_t pos, MDefinition* expr, int32_t low, int32_t high,
                     MBasicBlock** switchBlock)
    {
        if (!breakableStack_.append(pos))
            return false;
        if (inDeadCode()) {
            *switchBlock = nullptr;
            return true;
        }
        curBlock_->end(MTableSwitch::New(alloc(), expr, low, high));
        *switchBlock = curBlock_;
        curBlock_ = nullptr;
        return true;
    }

    bool startSwitchCase(MBasicBlock* switchBlock, MBasicBlock** next)
    {
        if (!switchBlock) {
            *next = nullptr;
            return true;
        }
        if (!newBlock(switchBlock, next))
            return false;
        if (curBlock_) {
            curBlock_->end(MGoto::New(alloc(), *next));
            if (!(*next)->addPredecessor(alloc(), curBlock_))
                return false;
        }
        curBlock_ = *next;
        return true;
    }

    bool startSwitchDefault(MBasicBlock* switchBlock, BlockVector* cases, MBasicBlock** defaultBlock)
    {
        if (!startSwitchCase(switchBlock, defaultBlock))
            return false;
        if (!*defaultBlock)
            return true;
        mirGraph().moveBlockToEnd(*defaultBlock);
        return true;
    }

    bool joinSwitch(MBasicBlock* switchBlock, const BlockVector& cases, MBasicBlock* defaultBlock)
    {
        size_t pos = breakableStack_.popCopy();
        if (!switchBlock)
            return true;
        MTableSwitch* mir = switchBlock->lastIns()->toTableSwitch();
        size_t defaultIndex = mir->addDefault(defaultBlock);
        for (unsigned i = 0; i < cases.length(); i++) {
            if (!cases[i])
                mir->addCase(defaultIndex);
            else
                mir->addCase(mir->addSuccessor(cases[i]));
        }
        if (curBlock_) {
            MBasicBlock* next;
            if (!newBlock(curBlock_, &next))
                return false;
            curBlock_->end(MGoto::New(alloc(), next));
            curBlock_ = next;
        }
        return bindUnlabeledBreaks(pos);
    }

    /************************************************************ DECODING ***/

    uint8_t  readU8()              { return func_.readU8(&pc_); }
    uint32_t readU32()             { return func_.readU32(&pc_); }
    int32_t  readI32()             { return func_.readI32(&pc_); }
    float    readF32()             { return func_.readF32(&pc_); }
    double   readF64()             { return func_.readF64(&pc_); }
    LifoSignature* readSignature() { return func_.readSignature(&pc_); }
    SimdConstant readI32X4()       { return func_.readI32X4(&pc_); }
    SimdConstant readF32X4()       { return func_.readF32X4(&pc_); }

    Stmt readStmtOp()              { return Stmt(readU8()); }

    void assertDebugCheckPoint() {
#ifdef DEBUG
        MOZ_ASSERT(Stmt(readU8()) == Stmt::DebugCheckPoint);
#endif
    }

    bool done() const { return pc_ == func_.size(); }
    size_t pc() const { return pc_; }

    bool prepareEmitMIR(const VarTypeVector& argTypes)
    {
        const AsmFunction::VarInitializerVector& varInitializers = func_.varInitializers();
        size_t numLocals = func_.numLocals();

        // Prepare data structures
        alloc_  = lifo_.new_<TempAllocator>(&lifo_);
        if (!alloc_)
            return false;
        jitContext_.emplace(m().runtime(), /* CompileCompartment = */ nullptr, alloc_);
        graph_  = lifo_.new_<MIRGraph>(alloc_);
        if (!graph_)
            return false;
        MOZ_ASSERT(numLocals == argTypes.length() + varInitializers.length());
        info_   = lifo_.new_<CompileInfo>(numLocals);
        if (!info_)
            return false;
        const OptimizationInfo* optimizationInfo = js_IonOptimizations.get(Optimization_AsmJS);
        const JitCompileOptions options;
        mirGen_ = lifo_.new_<MIRGenerator>(m().compartment(),
                                           options, alloc_,
                                           graph_, info_, optimizationInfo,
                                           &m().onOutOfBoundsLabel(),
                                           &m().onConversionErrorLabel(),
                                           m().usesSignalHandlersForOOB());
        if (!mirGen_)
            return false;

        if (!newBlock(/* pred = */ nullptr, &curBlock_))
            return false;

        // Emit parameters and local variables
        for (ABIArgTypeIter i(argTypes); !i.done(); i++) {
            MAsmJSParameter* ins = MAsmJSParameter::New(alloc(), *i, i.mirType());
            curBlock_->add(ins);
            curBlock_->initSlot(info().localSlot(i.index()), ins);
            if (!mirGen_->ensureBallast())
                return false;
            localVarTypes_.append(argTypes[i.index()].toType());
        }

        unsigned firstLocalSlot = argTypes.length();
        for (unsigned i = 0; i < varInitializers.length(); i++) {
            const AsmJSNumLit& lit = varInitializers[i];
            Type type = Type::Of(lit);
            MIRType mirType = type.toMIRType();

            MInstruction* ins;
            if (lit.isSimd())
               ins = MSimdConstant::New(alloc(), lit.simdValue(), mirType);
            else
               ins = MConstant::NewAsmJS(alloc(), lit.scalarValue(), mirType);

            curBlock_->add(ins);
            curBlock_->initSlot(info().localSlot(firstLocalSlot + i), ins);
            if (!mirGen_->ensureBallast())
                return false;
            localVarTypes_.append(type);
        }

        return true;
    }

    /*************************************************************************/

    MIRGenerator* extractMIR()
    {
        MOZ_ASSERT(mirGen_ != nullptr);
        MIRGenerator* mirGen = mirGen_;
        mirGen_ = nullptr;
        return mirGen;
    }

    /*************************************************************************/
  private:
    bool newBlockWithDepth(MBasicBlock* pred, unsigned loopDepth, MBasicBlock** block)
    {
        *block = MBasicBlock::NewAsmJS(mirGraph(), info(), pred, MBasicBlock::NORMAL);
        if (!*block)
            return false;
        mirGraph().addBlock(*block);
        (*block)->setLoopDepth(loopDepth);
        return true;
    }

    bool newBlock(MBasicBlock* pred, MBasicBlock** block)
    {
        return newBlockWithDepth(pred, loopStack_.length(), block);
    }

    bool bindBreaksOrContinues(BlockVector* preds, bool* createdJoinBlock)
    {
        for (unsigned i = 0; i < preds->length(); i++) {
            MBasicBlock* pred = (*preds)[i];
            if (*createdJoinBlock) {
                pred->end(MGoto::New(alloc(), curBlock_));
                if (!curBlock_->addPredecessor(alloc(), pred))
                    return false;
            } else {
                MBasicBlock* next;
                if (!newBlock(pred, &next))
                    return false;
                pred->end(MGoto::New(alloc(), next));
                if (curBlock_) {
                    curBlock_->end(MGoto::New(alloc(), next));
                    if (!next->addPredecessor(alloc(), curBlock_))
                        return false;
                }
                curBlock_ = next;
                *createdJoinBlock = true;
            }
            MOZ_ASSERT(curBlock_->begin() == curBlock_->end());
            if (!mirGen_->ensureBallast())
                return false;
        }
        preds->clear();
        return true;
    }

    bool bindLabeledBreaksOrContinues(const LabelVector* maybeLabels, LabeledBlockMap* map,
                                      bool* createdJoinBlock)
    {
        if (!maybeLabels)
            return true;
        const LabelVector& labels = *maybeLabels;
        for (unsigned i = 0; i < labels.length(); i++) {
            if (LabeledBlockMap::Ptr p = map->lookup(labels[i])) {
                if (!bindBreaksOrContinues(&p->value(), createdJoinBlock))
                    return false;
                map->remove(p);
            }
            if (!mirGen_->ensureBallast())
                return false;
        }
        return true;
    }

    template <class Key, class Map>
    bool addBreakOrContinue(Key key, Map* map)
    {
        if (inDeadCode())
            return true;
        typename Map::AddPtr p = map->lookupForAdd(key);
        if (!p) {
            BlockVector empty;
            if (!map->add(p, key, Move(empty)))
                return false;
        }
        if (!p->value().append(curBlock_))
            return false;
        curBlock_ = nullptr;
        return true;
    }

    bool bindUnlabeledBreaks(size_t pos)
    {
        bool createdJoinBlock = false;
        if (UnlabeledBlockMap::Ptr p = unlabeledBreaks_.lookup(pos)) {
            if (!bindBreaksOrContinues(&p->value(), &createdJoinBlock))
                return false;
            unlabeledBreaks_.remove(p);
        }
        return true;
    }
};

static bool
EmitLiteral(FunctionCompiler& f, AsmType type, MDefinition**def)
{
    switch (type) {
      case AsmType::Int32: {
        int32_t val = f.readI32();
        *def = f.constant(Int32Value(val), MIRType_Int32);
        return true;
      }
      case AsmType::Float32: {
        float val = f.readF32();
        *def = f.constant(Float32Value(val), MIRType_Float32);
        return true;
      }
      case AsmType::Float64: {
        double val = f.readF64();
        *def = f.constant(DoubleValue(val), MIRType_Double);
        return true;
      }
      case AsmType::Int32x4: {
        SimdConstant lit(f.readI32X4());
        *def = f.constant(lit, MIRType_Int32x4);
        return true;
      }
      case AsmType::Float32x4: {
        SimdConstant lit(f.readF32X4());
        *def = f.constant(lit, MIRType_Float32x4);
        return true;
      }
    }
    MOZ_CRASH("unexpected literal type");
}

static bool
EmitGetLoc(FunctionCompiler& f, const DebugOnly<MIRType>& type, MDefinition** def)
{
    uint32_t slot = f.readU32();
    *def = f.getLocalDef(slot);
    MOZ_ASSERT_IF(*def, (*def)->type() == type);
    return true;
}

static bool
EmitGetGlo(FunctionCompiler& f, MIRType type, MDefinition** def)
{
    uint32_t globalDataOffset = f.readU32();
    bool isConst = bool(f.readU8());
    *def = f.loadGlobalVar(globalDataOffset, isConst, type);
    return true;
}

static bool EmitI32Expr(FunctionCompiler& f, MDefinition** def);
static bool EmitF32Expr(FunctionCompiler& f, MDefinition** def);
static bool EmitF64Expr(FunctionCompiler& f, MDefinition** def);
static bool EmitI32X4Expr(FunctionCompiler& f, MDefinition** def);
static bool EmitF32X4Expr(FunctionCompiler& f, MDefinition** def);
static bool EmitExpr(FunctionCompiler& f, AsmType type, MDefinition** def);

static bool
EmitLoadArray(FunctionCompiler& f, Scalar::Type scalarType, MDefinition** def)
{
    NeedsBoundsCheck needsBoundsCheck = NeedsBoundsCheck(f.readU8());
    MDefinition* ptr;
    if (!EmitI32Expr(f, &ptr))
        return false;
    *def = f.loadHeap(scalarType, ptr, needsBoundsCheck);
    return true;
}

static bool
EmitSignMask(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MDefinition* in;
    if (!EmitExpr(f, type, &in))
        return false;
    *def = f.extractSignMask(in);
    return true;
}

static bool
EmitStore(FunctionCompiler& f, Scalar::Type viewType, MDefinition** def)
{
    NeedsBoundsCheck needsBoundsCheck = NeedsBoundsCheck(f.readU8());

    MDefinition* ptr;
    if (!EmitI32Expr(f, &ptr))
        return false;

    MDefinition* rhs = nullptr;
    switch (viewType) {
      case Scalar::Int8:
      case Scalar::Int16:
      case Scalar::Int32:
        if (!EmitI32Expr(f, &rhs))
            return false;
        break;
      case Scalar::Float32:
        if (!EmitF32Expr(f, &rhs))
            return false;
        break;
      case Scalar::Float64:
        if (!EmitF64Expr(f, &rhs))
            return false;
        break;
      default: MOZ_CRASH("unexpected scalar type");
    }

    f.storeHeap(viewType, ptr, rhs, needsBoundsCheck);
    *def = rhs;
    return true;
}

static bool
EmitStoreWithCoercion(FunctionCompiler& f, Scalar::Type rhsType, Scalar::Type viewType,
                      MDefinition **def)
{
    NeedsBoundsCheck needsBoundsCheck = NeedsBoundsCheck(f.readU8());
    MDefinition* ptr;
    if (!EmitI32Expr(f, &ptr))
        return false;

    MDefinition* rhs = nullptr;
    MDefinition* coerced = nullptr;
    if (rhsType == Scalar::Float32 && viewType == Scalar::Float64) {
        if (!EmitF32Expr(f, &rhs))
            return false;
        coerced = f.unary<MToDouble>(rhs);
    } else if (rhsType == Scalar::Float64 && viewType == Scalar::Float32) {
        if (!EmitF64Expr(f, &rhs))
            return false;
        coerced = f.unary<MToFloat32>(rhs);
    } else {
        MOZ_CRASH("unexpected coerced store");
    }

    f.storeHeap(viewType, ptr, coerced, needsBoundsCheck);
    *def = rhs;
    return true;
}

static bool
EmitSetLoc(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    uint32_t slot = f.readU32();
    MDefinition* expr;
    if (!EmitExpr(f, type, &expr))
        return false;
    f.assign(slot, expr);
    *def = expr;
    return true;
}

static bool
EmitSetGlo(FunctionCompiler& f, AsmType type, MDefinition**def)
{
    uint32_t globalDataOffset = f.readU32();
    MDefinition* expr;
    if (!EmitExpr(f, type, &expr))
        return false;
    f.storeGlobalVar(globalDataOffset, expr);
    *def = expr;
    return true;
}

static MIRType
MIRTypeFromAsmType(AsmType type)
{
    switch(type) {
      case AsmType::Int32:     return MIRType_Int32;
      case AsmType::Float32:   return MIRType_Float32;
      case AsmType::Float64:   return MIRType_Double;
      case AsmType::Int32x4:   return MIRType_Int32x4;
      case AsmType::Float32x4: return MIRType_Float32x4;
    }
    MOZ_CRASH("unexpected type in binary arith");
}

typedef bool IsMax;

static bool
EmitMathMinMax(FunctionCompiler& f, AsmType type, bool isMax, MDefinition** def)
{
    size_t numArgs = f.readU8();
    MOZ_ASSERT(numArgs >= 2);
    MDefinition* lastDef;
    if (!EmitExpr(f, type, &lastDef))
        return false;
    MIRType mirType = MIRTypeFromAsmType(type);
    for (size_t i = 1; i < numArgs; i++) {
        MDefinition* next;
        if (!EmitExpr(f, type, &next))
            return false;
        lastDef = f.minMax(lastDef, next, mirType, isMax);
    }
    *def = lastDef;
    return true;
}

static bool
EmitAtomicsLoad(FunctionCompiler& f, MDefinition** def)
{
    NeedsBoundsCheck needsBoundsCheck = NeedsBoundsCheck(f.readU8());
    Scalar::Type viewType = Scalar::Type(f.readU8());
    MDefinition* index;
    if (!EmitI32Expr(f, &index))
        return false;
    *def = f.atomicLoadHeap(viewType, index, needsBoundsCheck);
    return true;
}

static bool
EmitAtomicsStore(FunctionCompiler& f, MDefinition** def)
{
    NeedsBoundsCheck needsBoundsCheck = NeedsBoundsCheck(f.readU8());
    Scalar::Type viewType = Scalar::Type(f.readU8());
    MDefinition* index;
    if (!EmitI32Expr(f, &index))
        return false;
    MDefinition* value;
    if (!EmitI32Expr(f, &value))
        return false;
    f.atomicStoreHeap(viewType, index, value, needsBoundsCheck);
    *def = value;
    return true;
}

static bool
EmitAtomicsBinOp(FunctionCompiler& f, MDefinition** def)
{
    NeedsBoundsCheck needsBoundsCheck = NeedsBoundsCheck(f.readU8());
    Scalar::Type viewType = Scalar::Type(f.readU8());
    js::jit::AtomicOp op = js::jit::AtomicOp(f.readU8());
    MDefinition* index;
    if (!EmitI32Expr(f, &index))
        return false;
    MDefinition* value;
    if (!EmitI32Expr(f, &value))
        return false;
    *def = f.atomicBinopHeap(op, viewType, index, value, needsBoundsCheck);
    return true;
}

static bool
EmitAtomicsCompareExchange(FunctionCompiler& f, MDefinition** def)
{
    NeedsBoundsCheck needsBoundsCheck = NeedsBoundsCheck(f.readU8());
    Scalar::Type viewType = Scalar::Type(f.readU8());
    MDefinition* index;
    if (!EmitI32Expr(f, &index))
        return false;
    MDefinition* oldValue;
    if (!EmitI32Expr(f, &oldValue))
        return false;
    MDefinition* newValue;
    if (!EmitI32Expr(f, &newValue))
        return false;
    *def = f.atomicCompareExchangeHeap(viewType, index, oldValue, newValue, needsBoundsCheck);
    return true;
}

static bool
EmitAtomicsExchange(FunctionCompiler& f, MDefinition** def)
{
    NeedsBoundsCheck needsBoundsCheck = NeedsBoundsCheck(f.readU8());
    Scalar::Type viewType = Scalar::Type(f.readU8());
    MDefinition* index;
    if (!EmitI32Expr(f, &index))
        return false;
    MDefinition* value;
    if (!EmitI32Expr(f, &value))
        return false;
    *def = f.atomicExchangeHeap(viewType, index, value, needsBoundsCheck);
    return true;
}

static bool
EmitCallArgs(FunctionCompiler& f, const Signature& sig, FunctionCompiler::Call* call)
{
    f.startCallArgs(call);
    for (unsigned i = 0; i < sig.args().length(); i++) {
        MDefinition *arg = nullptr;
        switch (sig.arg(i).which()) {
          case VarType::Int:       if (!EmitI32Expr(f, &arg))   return false; break;
          case VarType::Float:     if (!EmitF32Expr(f, &arg))   return false; break;
          case VarType::Double:    if (!EmitF64Expr(f, &arg))   return false; break;
          case VarType::Int32x4:   if (!EmitI32X4Expr(f, &arg)) return false; break;
          case VarType::Float32x4: if (!EmitF32X4Expr(f, &arg)) return false; break;
          default: MOZ_CRASH("unexpected vartype");
        }
        if (!f.passArg(arg, sig.arg(i).toMIRType(), call))
            return false;
    }
    f.finishCallArgs(call);
    return true;
}

static void
ReadCallLineCol(FunctionCompiler& f, uint32_t* line, uint32_t* column)
{
    *line = f.readU32();
    *column = f.readU32();
}

static bool
EmitInternalCall(FunctionCompiler& f, RetType retType, MDefinition** def)
{
    uint32_t funcIndex = f.readU32();
    const Signature& sig = *f.readSignature();
    MOZ_ASSERT_IF(sig.retType() != RetType::Void, sig.retType() == retType);

    uint32_t lineno, column;
    ReadCallLineCol(f, &lineno, &column);

    FunctionCompiler::Call call(f, lineno, column);
    if (!EmitCallArgs(f, sig, &call))
        return false;

    return f.internalCall(sig, funcIndex, call, def);
}

static bool
EmitFuncPtrCall(FunctionCompiler& f, RetType retType, MDefinition** def)
{
    uint32_t mask = f.readU32();
    uint32_t globalDataOffset = f.readU32();

    const Signature& sig = *f.readSignature();
    MOZ_ASSERT_IF(sig.retType() != RetType::Void, sig.retType() == retType);

    uint32_t lineno, column;
    ReadCallLineCol(f, &lineno, &column);

    MDefinition *index;
    if (!EmitI32Expr(f, &index))
        return false;

    FunctionCompiler::Call call(f, lineno, column);
    if (!EmitCallArgs(f, sig, &call))
        return false;

    return f.funcPtrCall(sig, mask, globalDataOffset, index, call, def);
}

static bool
EmitFFICall(FunctionCompiler& f, RetType retType, MDefinition** def)
{
    unsigned globalDataOffset = f.readI32();

    const Signature& sig = *f.readSignature();
    MOZ_ASSERT_IF(sig.retType() != RetType::Void, sig.retType() == retType);

    uint32_t lineno, column;
    ReadCallLineCol(f, &lineno, &column);

    FunctionCompiler::Call call(f, lineno, column);
    if (!EmitCallArgs(f, sig, &call))
        return false;

    return f.ffiCall(globalDataOffset, call, retType.toMIRType(), def);
}

static bool
EmitMathBuiltinCall(FunctionCompiler& f, F32 f32, MDefinition** def)
{
    MOZ_ASSERT(f32 == F32::Ceil || f32 == F32::Floor);

    uint32_t lineno, column;
    ReadCallLineCol(f, &lineno, &column);

    FunctionCompiler::Call call(f, lineno, column);
    f.startCallArgs(&call);

    MDefinition* firstArg;
    if (!EmitF32Expr(f, &firstArg) || !f.passArg(firstArg, MIRType_Float32, &call))
        return false;

    f.finishCallArgs(&call);

    AsmJSImmKind callee = f32 == F32::Ceil ? AsmJSImm_CeilF : AsmJSImm_FloorF;
    return f.builtinCall(callee, call, MIRType_Float32, def);
}

static bool
EmitMathBuiltinCall(FunctionCompiler& f, F64 f64, MDefinition** def)
{
    uint32_t lineno, column;
    ReadCallLineCol(f, &lineno, &column);

    FunctionCompiler::Call call(f, lineno, column);
    f.startCallArgs(&call);

    MDefinition* firstArg;
    if (!EmitF64Expr(f, &firstArg) || !f.passArg(firstArg, MIRType_Double, &call))
        return false;

    if (f64 == F64::Pow || f64 == F64::Atan2) {
        MDefinition* secondArg;
        if (!EmitF64Expr(f, &secondArg) || !f.passArg(secondArg, MIRType_Double, &call))
            return false;
    }

    AsmJSImmKind callee;
    switch (f64) {
      case F64::Ceil:  callee = AsmJSImm_CeilD; break;
      case F64::Floor: callee = AsmJSImm_FloorD; break;
      case F64::Sin:   callee = AsmJSImm_SinD; break;
      case F64::Cos:   callee = AsmJSImm_CosD; break;
      case F64::Tan:   callee = AsmJSImm_TanD; break;
      case F64::Asin:  callee = AsmJSImm_ASinD; break;
      case F64::Acos:  callee = AsmJSImm_ACosD; break;
      case F64::Atan:  callee = AsmJSImm_ATanD; break;
      case F64::Exp:   callee = AsmJSImm_ExpD; break;
      case F64::Log:   callee = AsmJSImm_LogD; break;
      case F64::Pow:   callee = AsmJSImm_PowD; break;
      case F64::Atan2: callee = AsmJSImm_ATan2D; break;
      default: MOZ_CRASH("unexpected double math builtin callee");
    }

    f.finishCallArgs(&call);

    return f.builtinCall(callee, call, MIRType_Double, def);
}

static bool
EmitSimdUnary(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MSimdUnaryArith::Operation op = MSimdUnaryArith::Operation(f.readU8());
    MDefinition* in;
    if (!EmitExpr(f, type, &in))
        return false;
    *def = f.unarySimd(in, op, MIRTypeFromAsmType(type));
    return true;
}

template<class OpKind>
inline bool
EmitBinarySimdGuts(FunctionCompiler& f, AsmType type, OpKind op, MDefinition** def)
{
    MDefinition* lhs;
    if (!EmitExpr(f, type, &lhs))
        return false;
    MDefinition* rhs;
    if (!EmitExpr(f, type, &rhs))
        return false;
    *def = f.binarySimd(lhs, rhs, op, MIRTypeFromAsmType(type));
    return true;
}

static bool
EmitSimdBinaryArith(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MSimdBinaryArith::Operation op = MSimdBinaryArith::Operation(f.readU8());
    return EmitBinarySimdGuts(f, type, op, def);
}

static bool
EmitSimdBinaryBitwise(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MSimdBinaryBitwise::Operation op = MSimdBinaryBitwise::Operation(f.readU8());
    return EmitBinarySimdGuts(f, type, op, def);
}

static bool
EmitSimdBinaryComp(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MSimdBinaryComp::Operation op = MSimdBinaryComp::Operation(f.readU8());
    MDefinition* lhs;
    if (!EmitExpr(f, type, &lhs))
        return false;
    MDefinition* rhs;
    if (!EmitExpr(f, type, &rhs))
        return false;
    *def = f.binarySimd<MSimdBinaryComp>(lhs, rhs, op);
    return true;
}

static bool
EmitSimdBinaryShift(FunctionCompiler& f, MDefinition** def)
{
    MSimdShift::Operation op = MSimdShift::Operation(f.readU8());
    MDefinition* lhs;
    if (!EmitI32X4Expr(f, &lhs))
        return false;
    MDefinition* rhs;
    if (!EmitI32Expr(f, &rhs))
        return false;
    *def = f.binarySimd<MSimdShift>(lhs, rhs, op);
    return true;
}

static MIRType
ScalarMIRTypeFromSimdAsmType(AsmType type)
{
    switch (type) {
      case AsmType::Int32:
      case AsmType::Float32:
      case AsmType::Float64:   break;
      case AsmType::Int32x4:   return MIRType_Int32;
      case AsmType::Float32x4: return MIRType_Float32;
    }
    MOZ_CRASH("unexpected simd type");
}

static bool
EmitExtractLane(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MDefinition* vec;
    if (!EmitExpr(f, type, &vec))
        return false;

    MDefinition* laneDef;
    if (!EmitI32Expr(f, &laneDef))
        return false;

    if (!laneDef) {
        *def = nullptr;
        return true;
    }

    MOZ_ASSERT(laneDef->isConstant());
    int32_t laneLit = laneDef->toConstant()->value().toInt32();
    MOZ_ASSERT(laneLit < 4);
    SimdLane lane = SimdLane(laneLit);

    *def = f.extractSimdElement(lane, vec, ScalarMIRTypeFromSimdAsmType(type));
    return true;
}

static AsmType
AsmSimdTypeToScalarType(AsmType simd)
{
    switch (simd) {
      case AsmType::Int32x4:   return AsmType::Int32;
      case AsmType::Float32x4: return AsmType::Float32;
      case AsmType::Int32:
      case AsmType::Float32:
      case AsmType::Float64:    break;
    }
    MOZ_CRASH("unexpected simd type");
}

static bool
EmitSimdReplaceLane(FunctionCompiler& f, AsmType simdType, MDefinition** def)
{
    MDefinition* vector;
    if (!EmitExpr(f, simdType, &vector))
        return false;

    MDefinition* laneDef;
    if (!EmitI32Expr(f, &laneDef))
        return false;

    SimdLane lane;
    if (laneDef) {
        MOZ_ASSERT(laneDef->isConstant());
        int32_t laneLit = laneDef->toConstant()->value().toInt32();
        MOZ_ASSERT(laneLit < 4);
        lane = SimdLane(laneLit);
    } else {
        lane = SimdLane(-1);
    }

    MDefinition* scalar;
    if (!EmitExpr(f, AsmSimdTypeToScalarType(simdType), &scalar))
        return false;
    *def = f.insertElementSimd(vector, scalar, lane, MIRTypeFromAsmType(simdType));
    return true;
}

template<class T>
inline bool
EmitSimdCast(FunctionCompiler& f, AsmType fromType, AsmType toType, MDefinition** def)
{
    MDefinition* in;
    if (!EmitExpr(f, fromType, &in))
        return false;
    *def = f.convertSimd<T>(in, MIRTypeFromAsmType(fromType), MIRTypeFromAsmType(toType));
    return true;
}

static bool
EmitSimdSwizzle(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MDefinition* in;
    if (!EmitExpr(f, type, &in))
        return false;

    uint8_t lanes[4];
    for (unsigned i = 0; i < 4; i++)
        lanes[i] = f.readU8();

    *def = f.swizzleSimd(in, lanes[0], lanes[1], lanes[2], lanes[3], MIRTypeFromAsmType(type));
    return true;
}

static bool
EmitSimdShuffle(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MDefinition* lhs;
    if (!EmitExpr(f, type, &lhs))
        return false;

    MDefinition* rhs;
    if (!EmitExpr(f, type, &rhs))
        return false;

    uint8_t lanes[4];
    for (unsigned i = 0; i < 4; i++)
        lanes[i] = f.readU8();

    *def = f.shuffleSimd(lhs, rhs, lanes[0], lanes[1], lanes[2], lanes[3],
                         MIRTypeFromAsmType(type));
    return true;
}

static bool
EmitSimdLoad(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    Scalar::Type viewType = Scalar::Type(f.readU8());
    NeedsBoundsCheck needsBoundsCheck = NeedsBoundsCheck(f.readU8());
    uint8_t numElems = f.readU8();

    MDefinition* index;
    if (!EmitI32Expr(f, &index))
        return false;

    *def = f.loadSimdHeap(viewType, index, needsBoundsCheck, numElems);
    return true;
}

static bool
EmitSimdStore(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    Scalar::Type viewType = Scalar::Type(f.readU8());
    NeedsBoundsCheck needsBoundsCheck = NeedsBoundsCheck(f.readU8());
    uint8_t numElems = f.readU8();

    MDefinition* index;
    if (!EmitI32Expr(f, &index))
        return false;

    MDefinition* vec;
    if (!EmitExpr(f, type, &vec))
        return false;

    f.storeSimdHeap(viewType, index, vec, needsBoundsCheck, numElems);
    *def = vec;
    return true;
}

typedef bool IsElementWise;

static bool
EmitSimdSelect(FunctionCompiler& f, AsmType type, bool isElementWise, MDefinition** def)
{
    MDefinition* defs[3];
    if (!EmitI32X4Expr(f, &defs[0]) || !EmitExpr(f, type, &defs[1]) || !EmitExpr(f, type, &defs[2]))
        return false;
    *def = f.selectSimd(defs[0], defs[1], defs[2], MIRTypeFromAsmType(type), isElementWise);
    return true;
}

static bool
EmitSimdSplat(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MDefinition* in;
    if (!EmitExpr(f, AsmSimdTypeToScalarType(type), &in))
        return false;
    *def = f.splatSimd(in, MIRTypeFromAsmType(type));
    return true;
}

static bool
EmitSimdCtor(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    switch (type) {
      case AsmType::Int32x4: {
        MDefinition* args[4];
        for (unsigned i = 0; i < 4; i++) {
            if (!EmitI32Expr(f, &args[i]))
                return false;
        }
        *def = f.constructSimd<MSimdValueX4>(args[0], args[1], args[2], args[3], MIRType_Int32x4);
        return true;
      }
      case AsmType::Float32x4: {
        MDefinition* args[4];
        for (unsigned i = 0; i < 4; i++) {
            if (!EmitF32Expr(f, &args[i]))
                return false;
        }
        *def = f.constructSimd<MSimdValueX4>(args[0], args[1], args[2], args[3], MIRType_Float32x4);
        return true;
      }
      case AsmType::Int32:
      case AsmType::Float32:
      case AsmType::Float64:
        break;
    }
    MOZ_CRASH("unexpected SIMD type");
}

template<class T>
static bool
EmitUnary(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MDefinition* in;
    if (!EmitExpr(f, type, &in))
        return false;
    *def = f.unary<T>(in);
    return true;
}

template<class T>
static bool
EmitUnaryMir(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MDefinition* in;
    if (!EmitExpr(f, type, &in))
        return false;
    *def = f.unary<T>(in, MIRTypeFromAsmType(type));
    return true;
}

static bool EmitStatement(FunctionCompiler& f, LabelVector* maybeLabels = nullptr);

static bool
EmitComma(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    uint32_t numExpr = f.readU32();
    for (uint32_t i = 1; i < numExpr; i++) {
        if (!EmitStatement(f))
            return false;
    }
    return EmitExpr(f, type, def);
}

static bool
EmitConditional(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MDefinition* cond;
    if (!EmitI32Expr(f, &cond))
        return false;

    MBasicBlock* thenBlock = nullptr;
    MBasicBlock* elseBlock = nullptr;
    if (!f.branchAndStartThen(cond, &thenBlock, &elseBlock))
        return false;

    MDefinition* ifTrue;
    if (!EmitExpr(f, type, &ifTrue))
        return false;

    BlockVector thenBlocks;
    if (!f.appendThenBlock(&thenBlocks))
        return false;

    f.pushPhiInput(ifTrue);

    f.switchToElse(elseBlock);

    MDefinition* ifFalse;
    if (!EmitExpr(f, type, &ifFalse))
        return false;

    f.pushPhiInput(ifFalse);

    if (!f.joinIfElse(thenBlocks))
        return false;

    *def = f.popPhiOutput();
    return true;
}

static bool
EmitMultiply(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    MDefinition* lhs;
    if (!EmitExpr(f, type, &lhs))
        return false;
    MDefinition* rhs;
    if (!EmitExpr(f, type, &rhs))
        return false;
    MIRType mirType = MIRTypeFromAsmType(type);
    *def = f.mul(lhs, rhs, mirType, type == AsmType::Int32 ? MMul::Integer : MMul::Normal);
    return true;
}

typedef bool IsAdd;

static bool
EmitAddOrSub(FunctionCompiler& f, AsmType type, bool isAdd, MDefinition** def)
{
    MDefinition* lhs;
    if (!EmitExpr(f, type, &lhs))
        return false;
    MDefinition* rhs;
    if (!EmitExpr(f, type, &rhs))
        return false;
    MIRType mirType = MIRTypeFromAsmType(type);
    *def = isAdd ? f.binary<MAdd>(lhs, rhs, mirType) : f.binary<MSub>(lhs, rhs, mirType);
    return true;
}

typedef bool IsUnsigned;
typedef bool IsDiv;

static bool
EmitDivOrMod(FunctionCompiler& f, AsmType type, bool isDiv, bool isUnsigned, MDefinition** def)
{
    MDefinition* lhs;
    if (!EmitExpr(f, type, &lhs))
        return false;
    MDefinition* rhs;
    if (!EmitExpr(f, type, &rhs))
        return false;
    *def = isDiv
           ? f.div(lhs, rhs, MIRTypeFromAsmType(type), isUnsigned)
           : f.mod(lhs, rhs, MIRTypeFromAsmType(type), isUnsigned);
    return true;
}

static bool
EmitDivOrMod(FunctionCompiler& f, AsmType type, bool isDiv, MDefinition** def)
{
    MOZ_ASSERT(type != AsmType::Int32, "int div or mod must precise signedness");
    return EmitDivOrMod(f, type, isDiv, false, def);
}

static bool
EmitComparison(FunctionCompiler& f, I32 stmt, MDefinition** def)
{
    MDefinition *lhs, *rhs;
    MCompare::CompareType compareType;
    switch (stmt) {
      case I32::EqI32:
      case I32::NeI32:
      case I32::SLeI32:
      case I32::SLtI32:
      case I32::ULeI32:
      case I32::ULtI32:
      case I32::SGeI32:
      case I32::SGtI32:
      case I32::UGeI32:
      case I32::UGtI32:
        if (!EmitI32Expr(f, &lhs) || !EmitI32Expr(f, &rhs))
            return false;
        // The list of opcodes is sorted such that all signed comparisons
        // stand before ULtI32.
        compareType = stmt < I32::ULtI32
                      ? MCompare::Compare_Int32
                      : MCompare::Compare_UInt32;
        break;
      case I32::EqF32:
      case I32::NeF32:
      case I32::LeF32:
      case I32::LtF32:
      case I32::GeF32:
      case I32::GtF32:
        if (!EmitF32Expr(f, &lhs) || !EmitF32Expr(f, &rhs))
            return false;
        compareType = MCompare::Compare_Float32;
        break;
      case I32::EqF64:
      case I32::NeF64:
      case I32::LeF64:
      case I32::LtF64:
      case I32::GeF64:
      case I32::GtF64:
        if (!EmitF64Expr(f, &lhs) || !EmitF64Expr(f, &rhs))
            return false;
        compareType = MCompare::Compare_Double;
        break;
      default: MOZ_CRASH("unexpected comparison opcode");
    }

    JSOp compareOp;
    switch (stmt) {
      case I32::EqI32:
      case I32::EqF32:
      case I32::EqF64:
        compareOp = JSOP_EQ;
        break;
      case I32::NeI32:
      case I32::NeF32:
      case I32::NeF64:
        compareOp = JSOP_NE;
        break;
      case I32::SLeI32:
      case I32::ULeI32:
      case I32::LeF32:
      case I32::LeF64:
        compareOp = JSOP_LE;
        break;
      case I32::SLtI32:
      case I32::ULtI32:
      case I32::LtF32:
      case I32::LtF64:
        compareOp = JSOP_LT;
        break;
      case I32::SGeI32:
      case I32::UGeI32:
      case I32::GeF32:
      case I32::GeF64:
        compareOp = JSOP_GE;
        break;
      case I32::SGtI32:
      case I32::UGtI32:
      case I32::GtF32:
      case I32::GtF64:
        compareOp = JSOP_GT;
        break;
      default: MOZ_CRASH("unexpected comparison opcode");
    }

    *def = f.compare(lhs, rhs, compareOp, compareType);
    return true;
}

template<class T>
static bool
EmitBitwise(FunctionCompiler& f, MDefinition** def)
{
    MDefinition* lhs;
    if (!EmitI32Expr(f, &lhs))
        return false;
    MDefinition* rhs;
    if (!EmitI32Expr(f, &rhs))
        return false;
    *def = f.bitwise<T>(lhs, rhs);
    return true;
}

template<>
bool
EmitBitwise<MBitNot>(FunctionCompiler& f, MDefinition** def)
{
    MDefinition* in;
    if (!EmitI32Expr(f, &in))
        return false;
    *def = f.bitwise<MBitNot>(in);
    return true;
}

static bool
EmitExpr(FunctionCompiler& f, AsmType type, MDefinition** def)
{
    switch (type) {
      case AsmType::Int32:     return EmitI32Expr(f, def);
      case AsmType::Float32:   return EmitF32Expr(f, def);
      case AsmType::Float64:   return EmitF64Expr(f, def);
      case AsmType::Int32x4:   return EmitI32X4Expr(f, def);
      case AsmType::Float32x4: return EmitF32X4Expr(f, def);
    }
    MOZ_CRASH("unexpected asm type");
}

static bool
EmitInterruptCheck(FunctionCompiler& f)
{
    unsigned lineno = f.readU32();
    unsigned column = f.readU32();
    f.addInterruptCheck(lineno, column);
    return true;
}

static bool
EmitInterruptCheckLoop(FunctionCompiler& f)
{
    if (!EmitInterruptCheck(f))
        return false;
    return EmitStatement(f);
}

static bool
EmitWhile(FunctionCompiler& f, const LabelVector* maybeLabels)
{
    size_t headPc = f.pc();

    MBasicBlock* loopEntry;
    if (!f.startPendingLoop(headPc, &loopEntry))
        return false;

    MDefinition* condDef;
    if (!EmitI32Expr(f, &condDef))
        return false;

    MBasicBlock* afterLoop;
    if (!f.branchAndStartLoopBody(condDef, &afterLoop))
        return false;

    if (!EmitStatement(f))
        return false;

    if (!f.bindContinues(headPc, maybeLabels))
        return false;

    return f.closeLoop(loopEntry, afterLoop);
}

static bool
EmitFor(FunctionCompiler& f, Stmt stmt, const LabelVector* maybeLabels)
{
    MOZ_ASSERT(stmt == Stmt::ForInitInc || stmt == Stmt::ForInitNoInc ||
               stmt == Stmt::ForNoInitInc || stmt == Stmt::ForNoInitNoInc);
    size_t headPc = f.pc();

    if (stmt == Stmt::ForInitInc || stmt == Stmt::ForInitNoInc) {
        if (!EmitStatement(f))
            return false;
    }

    MBasicBlock* loopEntry;
    if (!f.startPendingLoop(headPc, &loopEntry))
        return false;

    MDefinition* condDef;
    if (!EmitI32Expr(f, &condDef))
        return false;

    MBasicBlock* afterLoop;
    if (!f.branchAndStartLoopBody(condDef, &afterLoop))
        return false;

    if (!EmitStatement(f))
        return false;

    if (!f.bindContinues(headPc, maybeLabels))
        return false;

    if (stmt == Stmt::ForInitInc || stmt == Stmt::ForNoInitInc) {
        if (!EmitStatement(f))
            return false;
    }

    f.assertDebugCheckPoint();

    return f.closeLoop(loopEntry, afterLoop);
}

static bool
EmitDoWhile(FunctionCompiler& f, const LabelVector* maybeLabels)
{
    size_t headPc = f.pc();

    MBasicBlock* loopEntry;
    if (!f.startPendingLoop(headPc, &loopEntry))
        return false;

    if (!EmitStatement(f))
        return false;

    if (!f.bindContinues(headPc, maybeLabels))
        return false;

    MDefinition* condDef;
    if (!EmitI32Expr(f, &condDef))
        return false;

    return f.branchAndCloseDoWhileLoop(condDef, loopEntry);
}

static bool
EmitLabel(FunctionCompiler& f, LabelVector* maybeLabels)
{
    uint32_t labelId = f.readU32();

    if (maybeLabels) {
        if (!maybeLabels->append(labelId))
            return false;
        return EmitStatement(f, maybeLabels);
    }

    LabelVector labels;
    if (!labels.append(labelId))
        return false;

    if (!EmitStatement(f, &labels))
        return false;

    return f.bindLabeledBreaks(&labels);
}

static bool EmitStatement(FunctionCompiler& f, Stmt stmt, LabelVector* maybeLabels = nullptr);

typedef bool HasElseBlock;

static bool
EmitIfElse(FunctionCompiler& f, bool hasElse)
{
    // Handle if/else-if chains using iteration instead of recursion. This
    // avoids blowing the C stack quota for long if/else-if chains and also
    // creates fewer MBasicBlocks at join points (by creating one join block
    // for the entire if/else-if chain).
    BlockVector thenBlocks;

  recurse:
    MDefinition* condition;
    if (!EmitI32Expr(f, &condition))
        return false;

    MBasicBlock* thenBlock = nullptr;
    MBasicBlock* elseOrJoinBlock = nullptr;
    if (!f.branchAndStartThen(condition, &thenBlock, &elseOrJoinBlock))
        return false;

    if (!EmitStatement(f))
        return false;

    if (!f.appendThenBlock(&thenBlocks))
        return false;

    if (hasElse) {
        f.switchToElse(elseOrJoinBlock);

        Stmt nextStmt(f.readStmtOp());
        if (nextStmt == Stmt::IfThen) {
            hasElse = false;
            goto recurse;
        }
        if (nextStmt == Stmt::IfElse) {
            hasElse = true;
            goto recurse;
        }

        if (!EmitStatement(f, nextStmt))
            return false;

        return f.joinIfElse(thenBlocks);
    } else {
        return f.joinIf(thenBlocks, elseOrJoinBlock);
    }
}

static bool
EmitSwitch(FunctionCompiler& f)
{
    bool hasDefault = f.readU8();
    int32_t low = f.readI32();
    int32_t high = f.readI32();
    uint32_t numCases = f.readU32();

    MDefinition* exprDef;
    if (!EmitI32Expr(f, &exprDef))
        return false;

    // Switch with no cases
    if (!hasDefault && numCases == 0)
        return true;

    BlockVector cases;
    if (!cases.resize(high - low + 1))
        return false;

    MBasicBlock* switchBlock;
    if (!f.startSwitch(f.pc(), exprDef, low, high, &switchBlock))
        return false;

    while (numCases--) {
        int32_t caseValue = f.readI32();
        MOZ_ASSERT(caseValue >= low && caseValue <= high);
        unsigned caseIndex = caseValue - low;
        if (!f.startSwitchCase(switchBlock, &cases[caseIndex]))
            return false;
        if (!EmitStatement(f))
            return false;
    }

    MBasicBlock* defaultBlock;
    if (!f.startSwitchDefault(switchBlock, &cases, &defaultBlock))
        return false;

    if (hasDefault && !EmitStatement(f))
        return false;

    return f.joinSwitch(switchBlock, cases, defaultBlock);
}

static AsmType
RetTypeToAsmType(RetType retType)
{
    switch (retType.which()) {
      case RetType::Void:      break;
      case RetType::Signed:    return AsmType::Int32;
      case RetType::Float:     return AsmType::Float32;
      case RetType::Double:    return AsmType::Float64;
      case RetType::Int32x4:   return AsmType::Int32x4;
      case RetType::Float32x4: return AsmType::Float32x4;
    }
    MOZ_CRASH("unexpected return type");
}

static bool
EmitRet(FunctionCompiler& f)
{
    RetType retType = f.returnedType();

    if (retType == RetType::Void) {
        f.returnVoid();
        return true;
    }

    AsmType type = RetTypeToAsmType(retType);
    MDefinition *def = nullptr;
    if (!EmitExpr(f, type, &def))
        return false;
    f.returnExpr(def);
    return true;
}

static bool
EmitBlock(FunctionCompiler& f)
{
    size_t numStmt = f.readU32();
    for (size_t i = 0; i < numStmt; i++) {
        if (!EmitStatement(f))
            return false;
    }
    f.assertDebugCheckPoint();
    return true;
}

typedef bool HasLabel;

static bool
EmitContinue(FunctionCompiler& f, bool hasLabel)
{
    if (!hasLabel)
        return f.addContinue(nullptr);
    uint32_t labelId = f.readU32();
    return f.addContinue(&labelId);
}

static bool
EmitBreak(FunctionCompiler& f, bool hasLabel)
{
    if (!hasLabel)
        return f.addBreak(nullptr);
    uint32_t labelId = f.readU32();
    return f.addBreak(&labelId);
}

static bool
EmitStatement(FunctionCompiler& f, Stmt stmt, LabelVector* maybeLabels /*= nullptr */)
{
    if (!f.mirGen().ensureBallast())
        return false;

    MDefinition* _;
    switch (stmt) {
      case Stmt::Block:              return EmitBlock(f);
      case Stmt::IfThen:             return EmitIfElse(f, HasElseBlock(false));
      case Stmt::IfElse:             return EmitIfElse(f, HasElseBlock(true));
      case Stmt::Switch:             return EmitSwitch(f);
      case Stmt::While:              return EmitWhile(f, maybeLabels);
      case Stmt::DoWhile:            return EmitDoWhile(f, maybeLabels);
      case Stmt::ForInitInc:
      case Stmt::ForInitNoInc:
      case Stmt::ForNoInitNoInc:
      case Stmt::ForNoInitInc:       return EmitFor(f, stmt, maybeLabels);
      case Stmt::Label:              return EmitLabel(f, maybeLabels);
      case Stmt::Continue:           return EmitContinue(f, HasLabel(false));
      case Stmt::ContinueLabel:      return EmitContinue(f, HasLabel(true));
      case Stmt::Break:              return EmitBreak(f, HasLabel(false));
      case Stmt::BreakLabel:         return EmitBreak(f, HasLabel(true));
      case Stmt::Ret:                return EmitRet(f);
      case Stmt::I32Expr:            return EmitI32Expr(f, &_);
      case Stmt::F32Expr:            return EmitF32Expr(f, &_);
      case Stmt::F64Expr:            return EmitF64Expr(f, &_);
      case Stmt::I32X4Expr:          return EmitI32X4Expr(f, &_);
      case Stmt::F32X4Expr:          return EmitF32X4Expr(f, &_);
      case Stmt::CallInternal:       return EmitInternalCall(f, RetType::Void, &_);
      case Stmt::CallIndirect:       return EmitFuncPtrCall(f, RetType::Void, &_);
      case Stmt::CallImport:         return EmitFFICall(f, RetType::Void, &_);
      case Stmt::AtomicsFence:       f.memoryBarrier(MembarFull); return true;
      case Stmt::Noop:               return true;
      case Stmt::Id:                 return EmitStatement(f);
      case Stmt::InterruptCheckHead: return EmitInterruptCheck(f);
      case Stmt::InterruptCheckLoop: return EmitInterruptCheckLoop(f);
      case Stmt::DebugCheckPoint:
      case Stmt::Bad:             break;
    }
    MOZ_CRASH("unexpected statement");
}

static bool
EmitStatement(FunctionCompiler& f, LabelVector* maybeLabels /* = nullptr */)
{
    Stmt stmt(f.readStmtOp());
    return EmitStatement(f, stmt, maybeLabels);
}

static bool
EmitI32Expr(FunctionCompiler& f, MDefinition** def)
{
    I32 op = I32(f.readU8());
    switch (op) {
      case I32::Id:
        return EmitI32Expr(f, def);
      case I32::Literal:
        return EmitLiteral(f, AsmType::Int32, def);
      case I32::GetLocal:
        return EmitGetLoc(f, DebugOnly<MIRType>(MIRType_Int32), def);
      case I32::SetLocal:
        return EmitSetLoc(f, AsmType::Int32, def);
      case I32::GetGlobal:
        return EmitGetGlo(f, MIRType_Int32, def);
      case I32::SetGlobal:
        return EmitSetGlo(f, AsmType::Int32, def);
      case I32::CallInternal:
        return EmitInternalCall(f, RetType::Signed, def);
      case I32::CallIndirect:
        return EmitFuncPtrCall(f, RetType::Signed, def);
      case I32::CallImport:
        return EmitFFICall(f, RetType::Signed, def);
      case I32::Conditional:
        return EmitConditional(f, AsmType::Int32, def);
      case I32::Comma:
        return EmitComma(f, AsmType::Int32, def);
      case I32::Add:
        return EmitAddOrSub(f, AsmType::Int32, IsAdd(true), def);
      case I32::Sub:
        return EmitAddOrSub(f, AsmType::Int32, IsAdd(false), def);
      case I32::Mul:
        return EmitMultiply(f, AsmType::Int32, def);
      case I32::UDiv:
      case I32::SDiv:
        return EmitDivOrMod(f, AsmType::Int32, IsDiv(true), IsUnsigned(op == I32::UDiv), def);
      case I32::UMod:
      case I32::SMod:
        return EmitDivOrMod(f, AsmType::Int32, IsDiv(false), IsUnsigned(op == I32::UMod), def);
      case I32::Min:
        return EmitMathMinMax(f, AsmType::Int32, IsMax(false), def);
      case I32::Max:
        return EmitMathMinMax(f, AsmType::Int32, IsMax(true), def);
      case I32::Not:
        return EmitUnary<MNot>(f, AsmType::Int32, def);
      case I32::FromF32:
        return EmitUnary<MTruncateToInt32>(f, AsmType::Float32, def);
      case I32::FromF64:
        return EmitUnary<MTruncateToInt32>(f, AsmType::Float64, def);
      case I32::Clz:
        return EmitUnary<MClz>(f, AsmType::Int32, def);
      case I32::Abs:
        return EmitUnaryMir<MAbs>(f, AsmType::Int32, def);
      case I32::Neg:
        return EmitUnaryMir<MAsmJSNeg>(f, AsmType::Int32, def);
      case I32::BitOr:
        return EmitBitwise<MBitOr>(f, def);
      case I32::BitAnd:
        return EmitBitwise<MBitAnd>(f, def);
      case I32::BitXor:
        return EmitBitwise<MBitXor>(f, def);
      case I32::Lsh:
        return EmitBitwise<MLsh>(f, def);
      case I32::ArithRsh:
        return EmitBitwise<MRsh>(f, def);
      case I32::LogicRsh:
        return EmitBitwise<MUrsh>(f, def);
      case I32::BitNot:
        return EmitBitwise<MBitNot>(f, def);
      case I32::SLoad8:
        return EmitLoadArray(f, Scalar::Int8, def);
      case I32::SLoad16:
        return EmitLoadArray(f, Scalar::Int16, def);
      case I32::SLoad32:
        return EmitLoadArray(f, Scalar::Int32, def);
      case I32::ULoad8:
        return EmitLoadArray(f, Scalar::Uint8, def);
      case I32::ULoad16:
        return EmitLoadArray(f, Scalar::Uint16, def);
      case I32::ULoad32:
        return EmitLoadArray(f, Scalar::Uint32, def);
      case I32::Store8:
        return EmitStore(f, Scalar::Int8, def);
      case I32::Store16:
        return EmitStore(f, Scalar::Int16, def);
      case I32::Store32:
        return EmitStore(f, Scalar::Int32, def);
      case I32::EqI32:
      case I32::NeI32:
      case I32::SLtI32:
      case I32::SLeI32:
      case I32::SGtI32:
      case I32::SGeI32:
      case I32::ULtI32:
      case I32::ULeI32:
      case I32::UGtI32:
      case I32::UGeI32:
      case I32::EqF32:
      case I32::NeF32:
      case I32::LtF32:
      case I32::LeF32:
      case I32::GtF32:
      case I32::GeF32:
      case I32::EqF64:
      case I32::NeF64:
      case I32::LtF64:
      case I32::LeF64:
      case I32::GtF64:
      case I32::GeF64:
        return EmitComparison(f, op, def);
      case I32::AtomicsCompareExchange:
        return EmitAtomicsCompareExchange(f, def);
      case I32::AtomicsExchange:
        return EmitAtomicsExchange(f, def);
      case I32::AtomicsLoad:
        return EmitAtomicsLoad(f, def);
      case I32::AtomicsStore:
        return EmitAtomicsStore(f, def);
      case I32::AtomicsBinOp:
        return EmitAtomicsBinOp(f, def);
      case I32::I32X4SignMask:
        return EmitSignMask(f, AsmType::Int32x4, def);
      case I32::F32X4SignMask:
        return EmitSignMask(f, AsmType::Float32x4, def);
      case I32::I32X4ExtractLane:
        return EmitExtractLane(f, AsmType::Int32x4, def);
      case I32::Bad:
        break;
    }
    MOZ_CRASH("unexpected i32 expression");
}

static bool
EmitF32Expr(FunctionCompiler& f, MDefinition** def)
{
    F32 op = F32(f.readU8());
    switch (op) {
      case F32::Id:
        return EmitF32Expr(f, def);
      case F32::Literal:
        return EmitLiteral(f, AsmType::Float32, def);
      case F32::GetLocal:
        return EmitGetLoc(f, DebugOnly<MIRType>(MIRType_Float32), def);
      case F32::SetLocal:
        return EmitSetLoc(f, AsmType::Float32, def);
      case F32::GetGlobal:
        return EmitGetGlo(f, MIRType_Float32, def);
      case F32::SetGlobal:
        return EmitSetGlo(f, AsmType::Float32, def);
      case F32::CallInternal:
        return EmitInternalCall(f, RetType::Float, def);
      case F32::CallIndirect:
        return EmitFuncPtrCall(f, RetType::Float, def);
      case F32::CallImport:
        return EmitFFICall(f, RetType::Float, def);
      case F32::Conditional:
        return EmitConditional(f, AsmType::Float32, def);
      case F32::Comma:
        return EmitComma(f, AsmType::Float32, def);
      case F32::Add:
        return EmitAddOrSub(f, AsmType::Float32, IsAdd(true), def);
      case F32::Sub:
        return EmitAddOrSub(f, AsmType::Float32, IsAdd(false), def);
      case F32::Mul:
        return EmitMultiply(f, AsmType::Float32, def);
      case F32::Div:
        return EmitDivOrMod(f, AsmType::Float32, IsDiv(true), def);
      case F32::Min:
        return EmitMathMinMax(f, AsmType::Float32, IsMax(false), def);
      case F32::Max:
        return EmitMathMinMax(f, AsmType::Float32, IsMax(true), def);
      case F32::Neg:
        return EmitUnaryMir<MAsmJSNeg>(f, AsmType::Float32, def);
      case F32::Abs:
        return EmitUnaryMir<MAbs>(f, AsmType::Float32, def);
      case F32::Sqrt:
        return EmitUnaryMir<MSqrt>(f, AsmType::Float32, def);
      case F32::Ceil:
      case F32::Floor:
        return EmitMathBuiltinCall(f, op, def);
      case F32::FromF64:
        return EmitUnary<MToFloat32>(f, AsmType::Float64, def);
      case F32::FromS32:
        return EmitUnary<MToFloat32>(f, AsmType::Int32, def);
      case F32::FromU32:
        return EmitUnary<MAsmJSUnsignedToFloat32>(f, AsmType::Int32, def);
      case F32::Load:
        return EmitLoadArray(f, Scalar::Float32, def);
      case F32::StoreF32:
        return EmitStore(f, Scalar::Float32, def);
      case F32::StoreF64:
        return EmitStoreWithCoercion(f, Scalar::Float32, Scalar::Float64, def);
      case F32::F32X4ExtractLane:
        return EmitExtractLane(f, AsmType::Float32x4, def);
      case F32::Bad:
        break;
    }
    MOZ_CRASH("unexpected f32 expression");
}

static bool
EmitF64Expr(FunctionCompiler& f, MDefinition** def)
{
    F64 op = F64(f.readU8());
    switch (op) {
      case F64::Id:
        return EmitF64Expr(f, def);
      case F64::GetLocal:
        return EmitGetLoc(f, DebugOnly<MIRType>(MIRType_Double), def);
      case F64::SetLocal:
        return EmitSetLoc(f, AsmType::Float64, def);
      case F64::GetGlobal:
        return EmitGetGlo(f, MIRType_Double, def);
      case F64::SetGlobal:
        return EmitSetGlo(f, AsmType::Float64, def);
      case F64::Literal:
        return EmitLiteral(f, AsmType::Float64, def);
      case F64::Add:
        return EmitAddOrSub(f, AsmType::Float64, IsAdd(true), def);
      case F64::Sub:
        return EmitAddOrSub(f, AsmType::Float64, IsAdd(false), def);
      case F64::Mul:
        return EmitMultiply(f, AsmType::Float64, def);
      case F64::Div:
        return EmitDivOrMod(f, AsmType::Float64, IsDiv(true), def);
      case F64::Mod:
        return EmitDivOrMod(f, AsmType::Float64, IsDiv(false), def);
      case F64::Min:
        return EmitMathMinMax(f, AsmType::Float64, IsMax(false), def);
      case F64::Max:
        return EmitMathMinMax(f, AsmType::Float64, IsMax(true), def);
      case F64::Neg:
        return EmitUnaryMir<MAsmJSNeg>(f, AsmType::Float64, def);
      case F64::Abs:
        return EmitUnaryMir<MAbs>(f, AsmType::Float64, def);
      case F64::Sqrt:
        return EmitUnaryMir<MSqrt>(f, AsmType::Float64, def);
      case F64::Ceil:
      case F64::Floor:
      case F64::Sin:
      case F64::Cos:
      case F64::Tan:
      case F64::Asin:
      case F64::Acos:
      case F64::Atan:
      case F64::Exp:
      case F64::Log:
      case F64::Pow:
      case F64::Atan2:
        return EmitMathBuiltinCall(f, op, def);
      case F64::FromF32:
        return EmitUnary<MToDouble>(f, AsmType::Float32, def);
      case F64::FromS32:
        return EmitUnary<MToDouble>(f, AsmType::Int32, def);
      case F64::FromU32:
        return EmitUnary<MAsmJSUnsignedToDouble>(f, AsmType::Int32, def);
      case F64::Load:
        return EmitLoadArray(f, Scalar::Float64, def);
      case F64::StoreF64:
        return EmitStore(f, Scalar::Float64, def);
      case F64::StoreF32:
        return EmitStoreWithCoercion(f, Scalar::Float64, Scalar::Float32, def);
      case F64::CallInternal:
        return EmitInternalCall(f, RetType::Double, def);
      case F64::CallIndirect:
        return EmitFuncPtrCall(f, RetType::Double, def);
      case F64::CallImport:
        return EmitFFICall(f, RetType::Double, def);
      case F64::Conditional:
        return EmitConditional(f, AsmType::Float64, def);
      case F64::Comma:
        return EmitComma(f, AsmType::Float64, def);
      case F64::Bad:
        break;
    }
    MOZ_CRASH("unexpected f64 expression");
}

static bool
EmitI32X4Expr(FunctionCompiler& f, MDefinition** def)
{
    I32X4 op = I32X4(f.readU8());
    switch (op) {
      case I32X4::Id:
        return EmitI32X4Expr(f, def);
      case I32X4::GetLocal:
        return EmitGetLoc(f, DebugOnly<MIRType>(MIRType_Int32x4), def);
      case I32X4::SetLocal:
        return EmitSetLoc(f, AsmType::Int32x4, def);
      case I32X4::GetGlobal:
        return EmitGetGlo(f, MIRType_Int32x4, def);
      case I32X4::SetGlobal:
        return EmitSetGlo(f, AsmType::Int32x4, def);
      case I32X4::Comma:
        return EmitComma(f, AsmType::Int32x4, def);
      case I32X4::Conditional:
        return EmitConditional(f, AsmType::Int32x4, def);
      case I32X4::CallInternal:
        return EmitInternalCall(f, RetType::Int32x4, def);
      case I32X4::CallIndirect:
        return EmitFuncPtrCall(f, RetType::Int32x4, def);
      case I32X4::CallImport:
        return EmitFFICall(f, RetType::Int32x4, def);
      case I32X4::Literal:
        return EmitLiteral(f, AsmType::Int32x4, def);
      case I32X4::Ctor:
        return EmitSimdCtor(f, AsmType::Int32x4, def);
      case I32X4::Unary:
        return EmitSimdUnary(f, AsmType::Int32x4, def);
      case I32X4::Binary:
        return EmitSimdBinaryArith(f, AsmType::Int32x4, def);
      case I32X4::BinaryBitwise:
        return EmitSimdBinaryBitwise(f, AsmType::Int32x4, def);
      case I32X4::BinaryCompI32X4:
        return EmitSimdBinaryComp(f, AsmType::Int32x4, def);
      case I32X4::BinaryCompF32X4:
        return EmitSimdBinaryComp(f, AsmType::Float32x4, def);
      case I32X4::BinaryShift:
        return EmitSimdBinaryShift(f, def);
      case I32X4::ReplaceLane:
        return EmitSimdReplaceLane(f, AsmType::Int32x4, def);
      case I32X4::FromF32X4:
        return EmitSimdCast<MSimdConvert>(f, AsmType::Float32x4, AsmType::Int32x4, def);
      case I32X4::FromF32X4Bits:
        return EmitSimdCast<MSimdReinterpretCast>(f, AsmType::Float32x4, AsmType::Int32x4, def);
      case I32X4::Swizzle:
        return EmitSimdSwizzle(f, AsmType::Int32x4, def);
      case I32X4::Shuffle:
        return EmitSimdShuffle(f, AsmType::Int32x4, def);
      case I32X4::Select:
        return EmitSimdSelect(f, AsmType::Int32x4, IsElementWise(true), def);
      case I32X4::BitSelect:
        return EmitSimdSelect(f, AsmType::Int32x4, IsElementWise(false), def);
      case I32X4::Splat:
        return EmitSimdSplat(f, AsmType::Int32x4, def);
      case I32X4::Load:
        return EmitSimdLoad(f, AsmType::Int32x4, def);
      case I32X4::Store:
        return EmitSimdStore(f, AsmType::Int32x4, def);
      case I32X4::Bad:
        break;
    }
    MOZ_CRASH("unexpected int32x4 expression");
}

static bool
EmitF32X4Expr(FunctionCompiler& f, MDefinition** def)
{
    F32X4 op = F32X4(f.readU8());
    switch (op) {
      case F32X4::Id:
        return EmitF32X4Expr(f, def);
      case F32X4::GetLocal:
        return EmitGetLoc(f, DebugOnly<MIRType>(MIRType_Float32x4), def);
      case F32X4::SetLocal:
        return EmitSetLoc(f, AsmType::Float32x4, def);
      case F32X4::GetGlobal:
        return EmitGetGlo(f, MIRType_Float32x4, def);
      case F32X4::SetGlobal:
        return EmitSetGlo(f, AsmType::Float32x4, def);
      case F32X4::Comma:
        return EmitComma(f, AsmType::Float32x4, def);
      case F32X4::Conditional:
        return EmitConditional(f, AsmType::Float32x4, def);
      case F32X4::CallInternal:
        return EmitInternalCall(f, RetType::Float32x4, def);
      case F32X4::CallIndirect:
        return EmitFuncPtrCall(f, RetType::Float32x4, def);
      case F32X4::CallImport:
        return EmitFFICall(f, RetType::Float32x4, def);
      case F32X4::Literal:
        return EmitLiteral(f, AsmType::Float32x4, def);
      case F32X4::Ctor:
        return EmitSimdCtor(f, AsmType::Float32x4, def);
      case F32X4::Unary:
        return EmitSimdUnary(f, AsmType::Float32x4, def);
      case F32X4::Binary:
        return EmitSimdBinaryArith(f, AsmType::Float32x4, def);
      case F32X4::BinaryBitwise:
        return EmitSimdBinaryBitwise(f, AsmType::Float32x4, def);
      case F32X4::ReplaceLane:
        return EmitSimdReplaceLane(f, AsmType::Float32x4, def);
      case F32X4::FromI32X4:
        return EmitSimdCast<MSimdConvert>(f, AsmType::Int32x4, AsmType::Float32x4, def);
      case F32X4::FromI32X4Bits:
        return EmitSimdCast<MSimdReinterpretCast>(f, AsmType::Int32x4, AsmType::Float32x4, def);
      case F32X4::Swizzle:
        return EmitSimdSwizzle(f, AsmType::Float32x4, def);
      case F32X4::Shuffle:
        return EmitSimdShuffle(f, AsmType::Float32x4, def);
      case F32X4::Select:
        return EmitSimdSelect(f, AsmType::Float32x4, IsElementWise(true), def);
      case F32X4::BitSelect:
        return EmitSimdSelect(f, AsmType::Float32x4, IsElementWise(false), def);
      case F32X4::Splat:
        return EmitSimdSplat(f, AsmType::Float32x4, def);
      case F32X4::Load:
        return EmitSimdLoad(f, AsmType::Float32x4, def);
      case F32X4::Store:
        return EmitSimdStore(f, AsmType::Float32x4, def);
      case F32X4::Bad:
        break;
    }
    MOZ_CRASH("unexpected float32x4 expression");
}

bool
js::GenerateAsmFunctionMIR(ModuleCompiler& m, LifoAlloc& lifo, AsmFunction& func, MIRGenerator** mir)
{
    int64_t before = PRMJ_Now();

    FunctionCompiler f(m, func, lifo);
    if (!f.init())
        return false;

    if (!f.prepareEmitMIR(func.argTypes()))
        return false;

    while (!f.done()) {
        if (!EmitStatement(f))
            return false;
    }

    *mir = f.extractMIR();
    if (!*mir)
        return false;

    jit::SpewBeginFunction(*mir, nullptr);

    f.checkPostconditions();

    func.accumulateCompileTime((PRMJ_Now() - before) / PRMJ_USEC_PER_MSEC);
    return true;
}

bool
js::GenerateAsmFunctionCode(ModuleCompiler& m, AsmFunction& func, MIRGenerator& mir, LIRGraph& lir,
                            FunctionCompileResults* results)
{
    JitContext jitContext(m.runtime(), /* CompileCompartment = */ nullptr, &mir.alloc());

    int64_t before = PRMJ_Now();

    // A single MacroAssembler is reused for all function compilations so
    // that there is a single linear code segment for each module. To avoid
    // spiking memory, a LifoAllocScope in the caller frees all MIR/LIR
    // after each function is compiled. This method is responsible for cleaning
    // out any dangling pointers that the MacroAssembler may have kept.
    m.masm().resetForNewCodeGenerator(mir.alloc());

    ScopedJSDeletePtr<CodeGenerator> codegen(js_new<CodeGenerator>(&mir, &lir, &m.masm()));
    if (!codegen)
        return false;

    Label entry;
    AsmJSFunctionLabels labels(entry, m.stackOverflowLabel());
    if (!codegen->generateAsmJS(&labels))
        return false;

    func.accumulateCompileTime((PRMJ_Now() - before) / PRMJ_USEC_PER_MSEC);

    PropertyName* funcName = func.name();
    unsigned line = func.lineno();

    // Fill in the results of the function's compilation
    AsmJSModule::FunctionCodeRange codeRange(funcName, line, labels);
    results->finishCodegen(func, codeRange, *codegen->extractScriptCounts());

    // Unlike regular IonMonkey, which links and generates a new JitCode for
    // every function, we accumulate all the functions in the module in a
    // single MacroAssembler and link at end. Linking asm.js doesn't require a
    // CodeGenerator so we can destroy it now (via ScopedJSDeletePtr).
    return true;
}

bool
js::CreateAsmModuleCompiler(ModuleCompileInputs mci, AsmModuleCompilerScope* scope)
{
    auto* mc = js_new<ModuleCompiler>(mci);
    if (!mc || !mc->init())
        return false;
    scope->setModule(mc);
    return true;
}

AsmModuleCompilerScope::~AsmModuleCompilerScope()
{
    if (m_) {
        js_delete(m_);
        m_ = nullptr;
    }
}

void
js::FinishAsmModuleCompilation(ModuleCompiler& m, ScopedJSDeletePtr<ModuleCompileResults>* results)
{
    m.finish(results);
}
