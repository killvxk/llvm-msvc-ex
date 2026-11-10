#include "LinearMBA.h"
#include "MBAMatrix.h"
#include "Utils.h"
#include "CryptoUtils.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Support/CommandLine.h"

#include <algorithm>
#include <map>
#include <random>
#include <vector>

using namespace llvm;

static cl::opt<bool> RunLinearMBA("x-mba", cl::init(false),
                                   cl::desc("OLLVM - Linear MBA Obfuscation"));

static cl::opt<int> LinearMBAProb(
    "x-mba-prob", cl::init(100),
    cl::desc("Choose the probability [%] each instruction will be "
             "obfuscated by Linear MBA Pass"));

namespace ollvm {

// Build expression functions
Value *buildExpr0(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateAnd(X, Y);
}

Value *buildExpr1(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateAnd(X, IRB.CreateNot(Y));
}

Value *buildExpr2(IRBuilder<> &IRB, Value *X, Value *Y) { return X; }

Value *buildExpr3(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateXor(IRB.CreateNot(X), Y);
}

Value *buildExpr4(IRBuilder<> &IRB, Value *X, Value *Y) { return Y; }

Value *buildExpr5(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateXor(X, Y);
}

Value *buildExpr6(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateOr(X, Y);
}

Value *buildExpr7(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateNot(IRB.CreateOr(X, Y));
}

Value *buildExpr8(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateNot(IRB.CreateXor(X, Y));
}

Value *buildExpr9(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateNot(Y);
}

Value *buildExpr10(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateOr(X, IRB.CreateNot(Y));
}

Value *buildExpr11(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateNot(X);
}

Value *buildExpr12(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateOr(IRB.CreateNot(X), Y);
}

Value *buildExpr13(IRBuilder<> &IRB, Value *X, Value *Y) {
  return IRB.CreateNot(IRB.CreateAnd(X, Y));
}

// Term type definitions
struct BitwiseTerm {
  int TruthTable[4];
  Value *(*Builder)(IRBuilder<> &, Value *, Value *);
};

struct LinearMBATerm {
  BitwiseTerm *TermInfo;
  int64_t Coefficient;
};

#define TERM_0(x, y) ((x) & (y))
#define TERM_1(x, y) ((x) & ~(y))
#define TERM_2(x, y) (x)
#define TERM_3(x, y) (~(x) ^ (y))
#define TERM_4(x, y) (y)
#define TERM_5(x, y) ((x) ^ (y))
#define TERM_6(x, y) ((x) | (y))
#define TERM_7(x, y) (~((x) | (y)))
#define TERM_8(x, y) (~((x) ^ (y)))
#define TERM_9(x, y) (~(y))
#define TERM_10(x, y) ((x) | ~(y))
#define TERM_11(x, y) (~(x))
#define TERM_12(x, y) (~(x) | (y))
#define TERM_13(x, y) (~((x) & (y)))

#define DEFINE_TERM_INFO(ID)                                                   \
  {{(TERM_##ID(0, 0)) & 1, (TERM_##ID(0, 1)) & 1, (TERM_##ID(1, 0)) & 1,       \
    (TERM_##ID(1, 1)) & 1},                                                    \
   buildExpr##ID}

std::vector<BitwiseTerm> TermType = {
    DEFINE_TERM_INFO(0),  DEFINE_TERM_INFO(1),  DEFINE_TERM_INFO(2),
    DEFINE_TERM_INFO(3),  DEFINE_TERM_INFO(4),  DEFINE_TERM_INFO(5),
    DEFINE_TERM_INFO(6),  DEFINE_TERM_INFO(7),  DEFINE_TERM_INFO(8),
    DEFINE_TERM_INFO(9),  DEFINE_TERM_INFO(10), DEFINE_TERM_INFO(11),
    DEFINE_TERM_INFO(12), DEFINE_TERM_INFO(13)};

void randomSelectTerms(std::vector<LinearMBATerm> &SelectedTerms) {
  std::vector<BitwiseTerm *> Available;
  for (BitwiseTerm &BT : TermType) {
    bool B = false;
    for (auto Iter = SelectedTerms.begin(); Iter != SelectedTerms.end();
         Iter++) {
      if (Iter->TermInfo == &BT) {
        B = true;
        break;
      }
    }
    if (!B) {
      Available.push_back(&BT);
    }
  }
  std::shuffle(Available.begin(), Available.end(),
               std::mt19937{std::random_device{}()});
  int Num = 5 - (int)SelectedTerms.size();
  for (int i = 0; i < Num; i++) {
    LinearMBATerm NewTerm = {Available[i], 0};
    SelectedTerms.push_back(NewTerm);
  }
  LinearMBATerm LastTerm = {nullptr, 0};
  SelectedTerms.push_back(LastTerm);
}

void calcCoefficients(std::vector<LinearMBATerm> &SelectedTerms) {
  int Size = SelectedTerms.size();
  MBAMatrix Mat(4, Size);
  for (int i = 0; i < Size; i++) {
    for (int j = 0; j < 4; j++) {
      if (SelectedTerms[i].TermInfo != nullptr) {
        Mat.setElement(j, i, SelectedTerms[i].TermInfo->TruthTable[j]);
      } else {
        Mat.setElement(j, i, 1);
      }
    }
  }
  std::vector<int64_t> Result;
  Mat.solve(Result);
  for (int i = 0; i < Size; i++) {
    SelectedTerms[i].Coefficient = Result[i];
  }
}

Value *buildLinearMBA(BinaryOperator *OriginalInsn,
                      std::vector<LinearMBATerm> &Terms) {
  IRBuilder<> IRB(OriginalInsn);
  std::shuffle(Terms.begin(), Terms.end(),
               std::mt19937{std::random_device{}()});
  // FIX: do NOT propagate the original instruction's NSW/NUW flags to the
  // MBA expression. The boolean-algebra identity holds mod 2^n, but
  // intermediate terms (coefficient × boolean-term) can wrap even when the
  // original addition did not. False no-wrap assertions let the optimizer
  // fold under non-overflow assumptions → wrong values → downstream crashes.
  (void)OriginalInsn;
  Value *X = OriginalInsn->getOperand(0), *Y = OriginalInsn->getOperand(1);
  Value *Expr = nullptr;
  for (LinearMBATerm &Term : Terms) {
    if (Term.Coefficient == 0) {
      continue;
    }
    Value *TermVal = nullptr;
    if (Term.TermInfo != nullptr) {
      TermVal = Term.TermInfo->Builder(IRB, X, Y);
    } else {
      TermVal = ConstantInt::getSigned(X->getType(), -1);
    }
    Value *Mul = ConstantInt::getSigned(X->getType(), Term.Coefficient);
    TermVal = IRB.CreateMul(Mul, TermVal);
    if (Expr != nullptr) {
      Expr = IRB.CreateAdd(Expr, TermVal);
    } else {
      Expr = TermVal;
    }
  }
  return Expr;
}

bool processAt(Instruction &Insn) {
  std::vector<LinearMBATerm> TermsSelected;
  Value *Result = nullptr;
  if (isa<BinaryOperator>(Insn)) {
    BinaryOperator &BI = cast<BinaryOperator>(Insn);
    switch (BI.getOpcode()) {
    case BinaryOperator::Add:
      TermsSelected.push_back({&TermType[2], 0});
      TermsSelected.push_back({&TermType[4], 0});
      randomSelectTerms(TermsSelected);
      calcCoefficients(TermsSelected);
      TermsSelected[0].Coefficient += 1;
      TermsSelected[1].Coefficient += 1;
      Result = buildLinearMBA(&BI, TermsSelected);
      break;
    case BinaryOperator::Sub:
      TermsSelected.push_back({&TermType[2], 0});
      TermsSelected.push_back({&TermType[4], 0});
      randomSelectTerms(TermsSelected);
      calcCoefficients(TermsSelected);
      TermsSelected[0].Coefficient += 1;
      TermsSelected[1].Coefficient -= 1;
      Result = buildLinearMBA(&BI, TermsSelected);
      break;
    case BinaryOperator::And:
      TermsSelected.push_back({&TermType[0], 0});
      randomSelectTerms(TermsSelected);
      calcCoefficients(TermsSelected);
      TermsSelected[0].Coefficient += 1;
      Result = buildLinearMBA(&BI, TermsSelected);
      break;
    case BinaryOperator::Or:
      TermsSelected.push_back({&TermType[6], 0});
      randomSelectTerms(TermsSelected);
      calcCoefficients(TermsSelected);
      TermsSelected[0].Coefficient += 1;
      Result = buildLinearMBA(&BI, TermsSelected);
      break;
    case BinaryOperator::Xor:
      TermsSelected.push_back({&TermType[5], 0});
      randomSelectTerms(TermsSelected);
      calcCoefficients(TermsSelected);
      TermsSelected[0].Coefficient += 1;
      Result = buildLinearMBA(&BI, TermsSelected);
      break;
    default:
      break;
    }
  }
  if (Result != nullptr) {
    Insn.replaceAllUsesWith(Result);
    return true;
  } else {
    return false;
  }
}

void process(Function &F) {
  std::vector<Instruction *> ToRemove;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      // Check probability
      if (static_cast<int32_t>(cryptoutils->get_range(100)) > LinearMBAProb) {
        continue;
      }
      // Only process integer types
      if (!I.getType()->isIntegerTy()) {
        continue;
      }
      if (processAt(I)) {
        ToRemove.push_back(&I);
      }
    }
  }
  for (Instruction *I : ToRemove) {
    I->eraseFromParent();
  }
}

bool runLinearMBA(Function &F) {
  process(F);
  return true;
}

} // namespace ollvm

PreservedAnalyses llvm::LinearMBAPass::run(Function &F,
                                           FunctionAnalysisManager &AM) {
  if (toObfuscate(RunLinearMBA, &F, "x-mba")) {
    if (ollvm::runLinearMBA(F))
      return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}
