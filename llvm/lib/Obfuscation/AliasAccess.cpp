//
// Ref to https://github.com/za233/Polaris-Obfuscator/
// Copied by root on 12/11/2024.
//

#include "AliasAccess.h"
#include "BogusControlFlow.h"
#include "CryptoUtils.h"
#include "Utils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
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
#include <VMFlatten.h>
#include <vector>

#define BRANCH_NUM 17

using namespace llvm;

namespace alias {
struct ElementPos {
  StructType *Type;
  unsigned Index;
};
struct ReferenceNode {
  AllocaInst *AI;
  bool IsRaw;
  unsigned Id;
  std::map<AllocaInst *, ElementPos> RawInsts;
  std::map<unsigned, ReferenceNode *> Edges;
  std::map<AllocaInst *, std::vector<unsigned>> Path;
};
};

PreservedAnalyses AliasAccess::run(Module &M, ModuleAnalysisManager &AM) {
  static_assert(BRANCH_NUM > 1);
  srand(time(NULL));
  for (Function &F : M) {
    if (F.hasPersonalityFn())
      continue;
    if (readAnnotate(&F).find("alias-access") != std::string::npos) {
      process(F);
    }
  }
  return PreservedAnalyses::none();
}
Function *AliasAccess::buildGetterFunction(Module &M, StructType *ST,
                                           unsigned Index) {
  std::vector<Type *> Params;
  Params.push_back(Type::getInt8Ty(M.getContext())->getPointerTo());
  FunctionType *FT = FunctionType::get(
      Type::getInt8Ty(M.getContext())->getPointerTo(), Params, false);
  Function *F = Function::Create(FT, GlobalValue::PrivateLinkage,
                                 Twine("__obfu_aliasaccess_getter"), M);
  BasicBlock *Entry = BasicBlock::Create(M.getContext(), "entry", F);
  Function::arg_iterator Iter = F->arg_begin();
  Value *Ptr = Iter;
  IRBuilder<> IRB(Entry);
  IRB.CreateRet(IRB.CreateGEP(ST, Ptr, {IRB.getInt32(0), IRB.getInt32(Index)}));
  return F;
}
void AliasAccess::process(Function &F) {

  std::vector<AllocaInst *> AIs;
  std::map<unsigned, Function *> Getter;
  Type *PtrType = Type::getInt8PtrTy(F.getContext());
  std::vector<alias::ReferenceNode *> Graph;
  StructType *TransST = StructType::create(F.getContext());
  std::vector<Type *> Slots;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (isa<AllocaInst>(I)) {
        AllocaInst *AI = (AllocaInst *)&I;
        if (AI->getAlign().value() <= 8) {
          AIs.push_back((AllocaInst *)&I);
        }
      }
    }
  }

  for (unsigned i = 0; i < BRANCH_NUM; i++) {
    Slots.push_back(PtrType);
  }
  TransST->setBody(Slots);
  std::vector<std::vector<AllocaInst *>> Bucket;
  for (unsigned i = 0; i < AIs.size(); i++) {
    Bucket.push_back(std::vector<AllocaInst *>());
  }

  for (AllocaInst *AI : AIs) {
    unsigned Index = getRandomNumber() % AIs.size();
    Bucket[Index].push_back(AI);
  }
  unsigned Count = 0;
  IRBuilder<> IRB(&*F.getEntryBlock().getFirstInsertionPt());
  for (auto &Items : Bucket) {
    if (Items.size() == 0) {
      continue;
    }
    alias::ReferenceNode *RN = new alias::ReferenceNode();
    RN->IsRaw = true;
    RN->Id = Count++;
    StructType *ST = StructType::create(F.getContext());
    unsigned Num = Items.size() * 2 + 1;
    Slots.clear();

    for (unsigned i = 0; i < Num; i++) {
      Slots.push_back(nullptr);
    }
    std::vector<unsigned> Random;
    // uint64_t AlignVal = 1;
    getRandomNoRepeat(Num, Items.size(), Random);
    for (unsigned i = 0; i < Items.size(); i++) {
      AllocaInst *AI = Items[i];
      // AlignVal = std::max(AI->getAlignment(), AlignVal);
      unsigned Idx = Random[i];
      Slots[Idx] = AI->getAllocatedType();
      alias::ElementPos EP;
      EP.Type = ST;
      EP.Index = Idx;
      RN->RawInsts[AI] = EP;
    }

    for (unsigned i = 0; i < Num; i++) {
      if (!Slots[i]) {
        Slots[i] = PtrType;
      }
    }
    ST->setBody(Slots);
    RN->AI = IRB.CreateAlloca(ST);
    // AlignVal = std::max(RN->AI->getAlignment(), AlignVal);
    // RN->AI->setAlignment(Align(AlignVal));
    Graph.push_back(RN);
  }
  unsigned Num = Graph.size() * 3;
  for (unsigned i = 0; i < Num; i++) {
    // std::shuffle(Graph.begin(), Graph.end(), std::default_random_engine());

    alias::ReferenceNode *Parent = new alias::ReferenceNode();
    AllocaInst *Cur = IRB.CreateAlloca(TransST);
    Parent->AI = Cur;
    Parent->IsRaw = false;
    Parent->Id = Count++;
    unsigned BN = getRandomNumber() % BRANCH_NUM;
    std::vector<unsigned> Random;
    getRandomNoRepeat(BRANCH_NUM, BN, Random);
    for (unsigned j = 0; j < BN; j++) {
      unsigned Idx = Random[j];
      alias::ReferenceNode *RN = Graph[getRandomNumber() % Graph.size()];
      Parent->Edges[Idx] = RN;

      IRB.CreateStore(
          RN->AI,
          IRB.CreateGEP(TransST, Cur, {IRB.getInt32(0), IRB.getInt32(Idx)}));
      // printf("s%d -> s%d at %d\n", Parent->Id, RN->Id, Idx);
      if (RN->IsRaw) {
        for (auto Iter = RN->RawInsts.begin(); Iter != RN->RawInsts.end();
             Iter++) {
          AllocaInst *AI = Iter->first;
          Parent->Path[AI].push_back(Idx);
        }
      } else {
        for (auto Iter = RN->Path.begin(); Iter != RN->Path.end(); Iter++) {
          Parent->Path[Iter->first].push_back(Idx);
        }
      }
    }
    Graph.push_back(Parent);
  }
  // printf("---------------------------------\n");
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      for (Use &U : I.operands()) {
        Value *Opnd = U.get();
        if (std::find(AIs.begin(), AIs.end(), Opnd) == AIs.end()) {
          continue;
        }
        AllocaInst *AI = (AllocaInst *)Opnd;
        IRB.SetInsertPoint(&I);
        std::shuffle(Graph.begin(), Graph.end(), std::default_random_engine());
        alias::ReferenceNode *Ptr = nullptr;
        for (alias::ReferenceNode *RN : Graph) {
          if (RN->Path.find(AI) != RN->Path.end() ||
              (RN->IsRaw && RN->RawInsts.find(AI) != RN->RawInsts.end())) {
            Ptr = RN;
            break;
          }
        }
        assert(Ptr != nullptr);
        Value *VP = Ptr->AI;
        while (!Ptr->IsRaw) {

          std::vector<unsigned> &Idxs = Ptr->Path[AI];
          unsigned Idx = Idxs[getRandomNumber() % Idxs.size()];
          if (Getter.find(Idx) == Getter.end()) {
            Function *G = buildGetterFunction(*F.getParent(), TransST, Idx);
            Getter[Idx] = G;
          }
          VP = IRB.CreateLoad(
              PtrType, IRB.CreateCall(FunctionCallee(Getter[Idx]), {VP}));
          // printf("(s%d, %d) -> ", Ptr->Id, Idx);
          // VP = IRB.CreateLoad(
          //   PtrType,
          //    IRB.CreateGEP(TransST, VP, {IRB.getInt32(0),
          //    IRB.getInt32(Idx)}));
          Ptr = Ptr->Edges[Idx];
        }
        // printf("s%d\n", Ptr->Id);
        assert(Ptr->RawInsts.find(AI) != Ptr->RawInsts.end());
        alias::ElementPos &EP = Ptr->RawInsts[AI];
        VP = IRB.CreateGEP(EP.Type, VP,
                           {IRB.getInt32(0), IRB.getInt32(EP.Index)});
        U.set(VP);
      }
    }
  }
  for (AllocaInst *AI : AIs) {
    AI->eraseFromParent();
  }
  for (auto Iter = Graph.begin(); Iter != Graph.end(); Iter++) {
    delete *Iter;
  }
}

