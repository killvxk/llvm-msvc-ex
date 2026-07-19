#include "IndirectBranch.h"
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
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#ifdef _MSC_VER
#include <vcruntime_string.h>
#endif
#include <vector>

using namespace llvm;

namespace indirect_branch {
struct IndirectBlockInfo {
  BasicBlock *BB;
  unsigned IndexWithinTable;
  unsigned RandomKey;
};
} // namespace indirect_branch

PreservedAnalyses IndirectBranch::run(Function &F,
                                      FunctionAnalysisManager &AM) {
  if (F.hasPersonalityFn())
    return PreservedAnalyses::all();
  if (readAnnotate(&F).find("ind-br") != std::string::npos) {
    process(F);
    return PreservedAnalyses::none();
  }

  return PreservedAnalyses::all();
}

void IndirectBranch::process(Function &F) {
  DataLayout Data = F.getParent()->getDataLayout();
  int PtrSize =
      Data.getTypeAllocSize(Type::getInt8Ty(F.getContext())->getPointerTo());
  Type *PtrValueType = Type::getIntNTy(F.getContext(), PtrSize * 8);
  std::vector<BranchInst *> Brs;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (isa<BranchInst>(I)) {
        Brs.push_back((BranchInst *)&I);
      }
    }
  }

  std::map<BasicBlock *, indirect_branch::IndirectBlockInfo> Map;
  std::vector<Constant *> Values;
  for (BranchInst *Br : Brs) {
    std::vector<BasicBlock *> BBs;
    if (Br->isConditional()) {
      BasicBlock *TrueBB = Br->getSuccessor(0), *FalseBB = Br->getSuccessor(1);
      BBs.push_back(TrueBB);
      BBs.push_back(FalseBB);
    } else {
      BasicBlock *BB = Br->getSuccessor(0);
      BBs.push_back(BB);
    }
    for (BasicBlock *BB : BBs) {
      if (Map.find(BB) != Map.end()) {
        continue;
      }
      indirect_branch::IndirectBlockInfo Info;
      Info.BB = BB;
      Info.IndexWithinTable = Map.size();
      Info.RandomKey = 0;
      Map[BB] = Info;
      Values.push_back(nullptr);
    }
  }
  ArrayType *AT = ArrayType::get(
      Type::getInt8Ty(F.getContext())->getPointerTo(), Map.size());
  GlobalVariable *AddrTable = new GlobalVariable(
      *(F.getParent()), AT, false, GlobalValue::PrivateLinkage, NULL);
  for (auto Iter = Map.begin(); Iter != Map.end(); Iter++) {
    indirect_branch::IndirectBlockInfo &Info = Iter->second;
    assert(Iter->first == Info.BB);
    BlockAddress *BA = BlockAddress::get(Info.BB);
    Constant *CValue = ConstantExpr::getPtrToInt(BA, PtrValueType);
    CValue = ConstantExpr::getAdd(
        CValue, ConstantInt::get(PtrValueType, Info.RandomKey));
    CValue = ConstantExpr::getIntToPtr(
        CValue, Type::getInt8Ty(F.getContext())->getPointerTo());
    Values[Info.IndexWithinTable] = CValue;
  }
  Constant *ValueArray = ConstantArray::get(AT, ArrayRef<Constant *>(Values));
  AddrTable->setInitializer(ValueArray);

  for (BranchInst *Br : Brs) {
    IRBuilder<> IRB(Br);
    if (Br->isConditional()) {
      BasicBlock *TrueBB = Br->getSuccessor(0), *FalseBB = Br->getSuccessor(1);
      indirect_branch::IndirectBlockInfo &TI = Map[TrueBB], &FI = Map[FalseBB];
      Value *Cond = Br->getCondition();
      Value *Index = IRB.CreateSelect(Cond, IRB.getInt32(TI.IndexWithinTable),
                                      IRB.getInt32(FI.IndexWithinTable));
      Value *Item = IRB.CreateLoad(
          IRB.getInt8PtrTy(),
          IRB.CreateGEP(AT, AddrTable, {IRB.getInt32(0), Index}));

      Value *Key =
          IRB.CreateSelect(Cond, IRB.getIntN(PtrSize * 8, TI.RandomKey),
                           IRB.getIntN(PtrSize * 8, FI.RandomKey));
      Value *Addr = IRB.CreateIntToPtr(
          IRB.CreateSub(IRB.CreatePtrToInt(Item, PtrValueType), Key),
          IRB.getInt8PtrTy());

      IndirectBrInst *IBR = IRB.CreateIndirectBr(Addr);
      IBR->addDestination(TrueBB);
      IBR->addDestination(FalseBB);
      Br->eraseFromParent();
    } else {
      BasicBlock *BB = Br->getSuccessor(0);
      indirect_branch::IndirectBlockInfo &BI = Map[BB];
      Value *Item = IRB.CreateLoad(
          IRB.getInt8PtrTy(),
          IRB.CreateGEP(AT, AddrTable,
                        {IRB.getInt32(0), IRB.getInt32(BI.IndexWithinTable)}));
      Value *Key = IRB.getIntN(PtrSize * 8, BI.RandomKey);
      Value *Addr = IRB.CreateIntToPtr(
          IRB.CreateSub(IRB.CreatePtrToInt(Item, PtrValueType), Key),
          IRB.getInt8PtrTy());
      IndirectBrInst *IBR = IRB.CreateIndirectBr(Addr);
      IBR->addDestination(BB);
      Br->eraseFromParent();
    }
  }
}