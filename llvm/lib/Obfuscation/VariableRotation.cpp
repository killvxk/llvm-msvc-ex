#include "VariableRotation.h"
#include "BogusControlFlow.h"
#include "ConstObfuscation.h"
#include "CryptoUtils.h"
#include "Flattening.h"
#include "IndirectGlobalVars.h"
#include "Utils.h"
#include "VMFlatten.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#ifdef _MSC_VER
#include <vcruntime_string.h>
#endif
#include <vector>

using namespace llvm;
static cl::opt<bool> RunVarRot("x-var-rot", cl::init(false),
                               cl::desc("OLLVM - variable rotation"));

namespace {
struct VariableRotation {
  Function *createRotateFunc(Module *m, PointerType *ptrType, Twine &name);
  void processFunction(Function &f, const SetVector<Function *> &shift);
  bool processVars(Function &f, SetVector<AllocaInst *> &vars,
                   Function *rotateFunc);
  bool isUsedByInst(Instruction *inst, const Value *var);
  bool runOnModule(Module &m);
};

Function *VariableRotation::createRotateFunc(Module *m, PointerType *ptrType,
                                             Twine &name) {

  IRBuilder<> builder(m->getContext());

  // Function argument types
  std::vector<Type *> funcArgs(
      {ptrType,
                                builder.getInt32Ty(), builder.getInt32Ty()});

  // Create the function type and function
  FunctionType *funcType =
      FunctionType::get(Type::getVoidTy(m->getContext()), funcArgs, false);
  Function *theFunction =
      Function::Create(funcType, Function::PrivateLinkage,
                                           name, m);

  // Define the function arguments
  Function::arg_iterator args = theFunction->arg_begin();
  Value *a1 = args++;
  a1->setName("a1");
  Value *a2 = args++;
  a2->setName("a2");
  Value *a3 = args++;
  a3->setName("a3");

  // Create the entry block and other blocks
  BasicBlock *entryBB =
      BasicBlock::Create(m->getContext(), "entry", theFunction);
  BasicBlock *outerLoopPreheaderBB =
      BasicBlock::Create(m->getContext(), "outer_loop.preheader", theFunction);
  BasicBlock *outerLoopBB =
      BasicBlock::Create(m->getContext(), "outer_loop", theFunction);
  BasicBlock *innerLoopBB =
      BasicBlock::Create(m->getContext(), "inner_loop", theFunction);
  BasicBlock *outerLoopBodyBB =
      BasicBlock::Create(m->getContext(), "outer_loop.body", theFunction);
  BasicBlock *returnBB =
      BasicBlock::Create(m->getContext(), "return", theFunction);

  // 入口
  builder.SetInsertPoint(entryBB);
  AllocaInst *jAlloca =
      builder.CreateAlloca(builder.getInt32Ty(), nullptr, "jAlloca");
  AllocaInst *iAlloca =
      builder.CreateAlloca(builder.getInt32Ty(), nullptr, "iAlloca");
  builder.CreateStore(builder.getInt32(0), iAlloca); // 初始i=0


  Value *cmp = builder.CreateICmpSLE(a2, builder.getInt32(1), "cmp");
  builder.CreateCondBr(cmp, returnBB, outerLoopPreheaderBB);

  // 外部循环前导
  builder.SetInsertPoint(outerLoopPreheaderBB);
  builder.CreateBr(outerLoopBB);

  // 外部循环
  builder.SetInsertPoint(outerLoopBB);
  builder.CreateStore(builder.getInt32(0), jAlloca); // 初始j=0
  // FIX(opaque-ptr): ptrType is a PointerType (just 'ptr' under opaque
  // pointers); loading with it as the type loads a *pointer* value from
  // the i8 buffer, then truncating a pointer to i8 is invalid IR.
  // The rotate function operates on bytes — load an i8 directly.
  Value *firstByte =
      builder.CreateLoad(builder.getInt8Ty(), a1, "firstByte");
  builder.CreateBr(innerLoopBB);

  // 内部循环
  builder.SetInsertPoint(innerLoopBB);
  Value *jVal = builder.CreateLoad(builder.getInt32Ty(), jAlloca, "j");
  Value *a1Idx = builder.CreateGEP(builder.getInt8Ty(), a1, jVal, "a1.idx");
  Value *nextJVal = builder.CreateAdd(jVal, builder.getInt32(1), "j.next");
  builder.CreateStore(nextJVal, jAlloca);
  Value *a1IdxNext =
      builder.CreateGEP(builder.getInt8Ty(), a1, nextJVal, "a1.idx.next");
  Value *nextElement =
      builder.CreateLoad(builder.getInt8Ty(), a1IdxNext, "nextElement");
  builder.CreateStore(nextElement, a1Idx);
  Value *jCmp = builder.CreateICmpSLT(nextJVal, a2, "j.cmp");
  builder.CreateCondBr(jCmp, innerLoopBB, outerLoopBodyBB);

  // 外循环体
  builder.SetInsertPoint(outerLoopBodyBB);
  Value *shiftedFirstByteI64 =
      builder.CreateZExt(firstByte, builder.getInt8Ty(), "shiftedFirstByteI64");
  Value *lastIndex = builder.CreateSub(a2, builder.getInt32(1), "lastIndex");
  Value *a1LastPtr = builder.CreateGEP(builder.getInt8Ty(), a1, lastIndex, "a1.last.ptr");
  builder.CreateStore(shiftedFirstByteI64, a1LastPtr);
  Value *iVal = builder.CreateLoad(builder.getInt32Ty(), iAlloca, "i");
  Value *nextIVal = builder.CreateAdd(iVal, builder.getInt32(1), "i.next");
  builder.CreateStore(nextIVal, iAlloca); // 更新i的值
  Value *exitCond = builder.CreateICmpEQ(nextIVal, a3, "exitcond");
  builder.CreateCondBr(exitCond, returnBB, outerLoopBB);

  // 返回
  builder.SetInsertPoint(returnBB);
  //Value *retVal = builder.CreateSelect(
  //    cmp, builder.getInt64(0), builder.CreateZExt(a3, builder.getInt64Ty()),
  //    "retVal");
  //builder.CreateRet(retVal);
  builder.CreateRetVoid();

  //theFunction->print(outs());
  return theFunction;
}

void VariableRotation::processFunction(Function &f,
                                       const SetVector<Function *> &shift) {
  SetVector<AllocaInst *> list;
  for (BasicBlock &bb : f)
    for (Instruction &instr : bb)
      if (isa<AllocaInst>(instr)) {
        auto a = (AllocaInst *)(&instr);
        // PORT-OBF: only rotate pure scalar allocas (single IntegerType,
        // FloatingPointType, or PointerType). Arrays, structs, and vectors
        // are skipped: GEP+variable-index arrays produce wrong results
        // under rotation (seed-dependent); structs may contain padding
        // that breaks byte-level rotation; vectors have SIMD alignment
        // requirements that rotation can violate.
        Type *AT = a->getAllocatedType();
        if (!isa<IntegerType>(AT))
          continue;
        // PORT-OBF: skip allocas whose address escapes to a function call.
        // The byte-rotate buffer garbles data when library functions
        // (snprintf, printf, memcpy...) read/write through the rotated
        // pointer. Only rotate allocas used exclusively by direct
        // load/store instructions.
        bool escapes = false;
        for (User *U : a->users()) {
          if (auto *CI = dyn_cast<CallInst>(U)) {
            escapes = true;
            break;
          }
          // GEP through the alloca then passed to a call also escapes
          if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
            for (User *GU : GEP->users()) {
              if (isa<CallInst>(GU)) {
                escapes = true;
                break;
              }
            }
            if (escapes)
              break;
          }
        }
        if (!escapes)
          list.insert(a);
      }
  if (list.size() >= 2 && processVars(f, list, shift[0])) {
    for (AllocaInst *a : list)
      a->eraseFromParent();
  }
}

bool VariableRotation::processVars(Function &f, SetVector<AllocaInst *> &vars,
                                   Function *rotateFunc) {
  if (vars.size() < 2)
    return false;

  IRBuilder<> irb(&*f.getEntryBlock().getFirstInsertionPt());
  Value *zero = ConstantInt::get(irb.getInt32Ty(), 0);
  const auto data = f.getParent()->getDataLayout();

  const int n = vars.size();

  // PORT-OBF: slot-aligned rotation. Each variable occupies one slot of
  // slot_size bytes (the max allocation size across all variables). The
  // rotation delta is always a multiple of slot_size, so multi-byte
  // variables stay contiguous within their slot during rotation. The
  // original byte-level rotation scattered multi-byte variables across
  // the array boundary (4-byte int split 2+2 at wrap-around) -> garbage.
  uint64_t slot_size = 0;
  for (const auto a : vars)
    slot_size = std::max(slot_size,
                         (uint64_t)data.getTypeAllocSize(a->getAllocatedType()));
  const uint64_t total = (uint64_t)n * slot_size;

  ArrayType *array_type = ArrayType::get(irb.getInt8Ty(), total);
  AllocaInst *array = irb.CreateAlloca(array_type);

  for (BasicBlock &bb : f) {
    int offset = 0; // in SLOTS
    auto iter = bb.getFirstInsertionPt();
    // FIX: do NOT skip an extra instruction in the entry block.
    // getFirstInsertionPt() already skips PHIs and allocas (including our
    // byte-array alloca above). The old ++iter skipped the FIRST REAL
    // instruction — typically a variable initialization store — which then
    // wrote to the original alloca (later erased) instead of the rotated
    // array, so the initial value was lost.
    while (iter != bb.end()) {
      Instruction *inst = &*iter;
      irb.SetInsertPoint(inst);
      // FIX: replace ALL rotated variables used by this instruction, not
      // just the first. The original `break` left subsequent variables
      // pointing at the original alloca (later erased → dangling pointer).
      for (int i = 0; i < n; i++)
        if (isUsedByInst(inst, vars[i])) {
          if (constexpr int prob = 50;
              cryptoutils->get_uint32_t() % 100 < prob) {
            const int times =
                cryptoutils->get_uint32_t() % (n - 1) + 1;
            irb.CreateCall(
                FunctionCallee(rotateFunc),
                {irb.CreateGEP(array->getAllocatedType(), array, {zero, zero}),
                 ConstantInt::get(irb.getInt32Ty(), total),
                 ConstantInt::get(irb.getInt32Ty(),
                                  (uint64_t)times * slot_size)});
            offset = (offset + times) % n;
          }
          const int slot_index = (n + i - offset) % n;
          const int byte_offset = (int)(slot_index * slot_size);
          Value *gep =
              irb.CreateGEP(array->getAllocatedType(), array,
                            {zero, ConstantInt::get(irb.getInt32Ty(), byte_offset)});
          Value *cast = irb.CreateBitOrPointerCast(gep, vars[i]->getType());
          int c = 0;
          for (const Value *ops : inst->operands()) {
            if (ops == vars[i])
              inst->setOperand(c, cast);
            c++;
          }
          // NO break — continue to replace other rotated vars in this inst
        }
      ++iter;
    }
    if (offset != 0) {
      irb.SetInsertPoint(bb.getTerminator());
      irb.CreateCall(
          FunctionCallee(rotateFunc),
          {irb.CreateGEP(array->getAllocatedType(), array, {zero, zero}),
           ConstantInt::get(irb.getInt32Ty(), total),
           ConstantInt::get(irb.getInt32Ty(),
                            (uint64_t)(n - offset) * slot_size)});
    }
  }
  return true;
}

bool VariableRotation::isUsedByInst(Instruction *inst, const Value *var) {
  if (std::any_of(inst->operands().begin(), inst->operands().end(),
                  [&](const Value *in) { return in == var; })) {
    return true;
  }
  return false;
}

bool VariableRotation::runOnModule(Module &m) {

  Twine fname1 = Twine("shiftFuncitonI8");

  Function *shiftFunc =
      createRotateFunc(&m, Type::getInt8PtrTy(m.getContext()), fname1);

  SetVector<Function *> shifts;

  shifts.insert(shiftFunc);

  for (Function &f : m) {

    if (&f == shiftFunc)
      continue;
    if (f.hasExactDefinition())

      processFunction(f, shifts);
  }

  return true;
}

}


PreservedAnalyses VariableRotationPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
  if (RunVarRot) {
    VariableRotation varObf;
    if (varObf.runOnModule(M))
      return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}
