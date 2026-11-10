// SVMInterpreterGen.h - LLVM 18 adapted
// Inline VM interpreter generator using LLVM IR Builder
// Generates complete vm_exec() function at compile time

#pragma once

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

//#define SVM_INTERP_DEBUG 1

namespace svm {

// Opcodes (must match VMCodeGen.h)
enum : uint8_t {
  OP_ALLOCA   = 0x01,
  OP_LOAD     = 0x02,
  OP_STORE    = 0x03,
  OP_GEP      = 0x05,
  OP_CMP      = 0x06,
  OP_CAST     = 0x07,
  OP_BR       = 0x08,
  OP_CALL     = 0x09,
  OP_RET      = 0x0A,
  OP_ARITH    = 0x10,
  OP_PARAMMAP = 0x11,
  OP_SELECT   = 0x12,
  OP_GEP_SCALED = 0x13
};

// Value encoding
enum : uint8_t { VT_CONST = 0, VT_SLOT = 1 };
enum : uint8_t { CK_INT = 0, CK_PTR = 1, CK_F32 = 2, CK_F64 = 3 };

// Arithmetic sub-ops
enum : uint8_t {
  AR_ADD, AR_SUB, AR_MUL,
  AR_UDIV, AR_SDIV, AR_UREM, AR_SREM,
  AR_SHL, AR_LSHR, AR_ASHR,
  AR_AND, AR_OR, AR_XOR,
  AR_FADD, AR_FSUB, AR_FMUL, AR_FDIV, AR_FREM
};

// Type kinds
enum : uint8_t { TK_INT = 0, TK_FP = 1 };

// CmpInst predicates (LLVM aligned)
enum {
  FCMP_FALSE = 0, FCMP_OEQ, FCMP_OGT, FCMP_OGE, FCMP_OLT, FCMP_OLE, FCMP_ONE, FCMP_ORD,
  FCMP_UNO, FCMP_UEQ, FCMP_UGT, FCMP_UGE, FCMP_ULT, FCMP_ULE, FCMP_UNE, FCMP_TRUE,
  ICMP_EQ = 32, ICMP_NE, ICMP_UGT, ICMP_UGE, ICMP_ULT, ICMP_ULE,
  ICMP_SGT, ICMP_SGE, ICMP_SLT, ICMP_SLE
};

// Cast opcodes
enum {
  Cast_Trunc = 1, Cast_ZExt, Cast_SExt, Cast_FPToUI, Cast_FPToSI, Cast_UIToFP, Cast_SIToFP,
  Cast_FPTrunc, Cast_FPExt, Cast_PtrToInt, Cast_IntToPtr, Cast_BitCast, Cast_AddrSpaceCast
};

/// SVMInterpreterGen - Generates vm_exec interpreter function in LLVM IR
class SVMInterpreterGen {
public:
  SVMInterpreterGen(llvm::Module &M) : M(M), Ctx(M.getContext()) {
    // Cache types
    VoidTy = llvm::Type::getVoidTy(Ctx);
    I1Ty = llvm::Type::getInt1Ty(Ctx);
    I8Ty = llvm::Type::getInt8Ty(Ctx);
    I32Ty = llvm::Type::getInt32Ty(Ctx);
    I64Ty = llvm::Type::getInt64Ty(Ctx);
    F32Ty = llvm::Type::getFloatTy(Ctx);
    F64Ty = llvm::Type::getDoubleTy(Ctx);
    PtrTy = llvm::PointerType::get(Ctx, 0);
    IntPtrTy = llvm::cast<llvm::IntegerType>(
        M.getDataLayout().getIntPtrType(Ctx));
  }

  /// Generate or get vm_exec function
  /// Signature: intptr_t vm_exec(const uint8_t* code, uint32_t size, void** args, intptr_t num)
  llvm::Function* getOrCreateVMExec() {
    // Check if already exists
    if (auto *F = M.getFunction("vm_exec")) {
      if (!F->empty()) return F;  // Already has body
      // PORT-OBF: adopt the pre-inserted empty declaration (created by
      // FuncRedirector's getOrInsertFunction) instead of Create()ing a
      // colliding "vm_exec" that LLVM renames to vm_exec.N — the leftover
      // declaration then reached the object file as an unreferenced
      // undefined external symbol, which NeverC's linker rejects
      // ("undefined symbol: vm_exec"). Adopting fills the declaration in
      // place, so the name stays vm_exec and nothing vestigial survives.
      F->setLinkage(llvm::GlobalValue::InternalLinkage);
      F->setDoesNotThrow();
      auto AI = F->arg_begin();
      llvm::Argument *ArgCode = &*AI++; ArgCode->setName("code");
      llvm::Argument *ArgSize = &*AI++; ArgSize->setName("size");
      llvm::Argument *ArgArgs = &*AI++; ArgArgs->setName("args");
      llvm::Argument *ArgNum = &*AI++;  ArgNum->setName("num");
      generateBody(F, ArgCode, ArgSize, ArgArgs, ArgNum);
      return F;
    }

    // Create function with InternalLinkage
    // Each translation unit has its own vm_exec + handle_call pair
    // This avoids cross-unit linkage issues with handle_call dispatch
    auto *FT = llvm::FunctionType::get(IntPtrTy, {PtrTy, I32Ty, PtrTy, IntPtrTy}, false);
    auto *F = llvm::Function::Create(FT, llvm::GlobalValue::InternalLinkage, "vm_exec", &M);
    F->setDoesNotThrow();

    // Name arguments
    auto AI = F->arg_begin();
    llvm::Argument *ArgCode = &*AI++; ArgCode->setName("code");
    llvm::Argument *ArgSize = &*AI++; ArgSize->setName("size");
    llvm::Argument *ArgArgs = &*AI++; ArgArgs->setName("args");
    llvm::Argument *ArgNum = &*AI++;  ArgNum->setName("num");

    // Generate body
    generateBody(F, ArgCode, ArgSize, ArgArgs, ArgNum);

    return F;
  }

private:
  llvm::Module &M;
  llvm::LLVMContext &Ctx;

  // Cached types
  llvm::Type *VoidTy, *I1Ty, *I8Ty, *I32Ty, *I64Ty, *F32Ty, *F64Ty, *PtrTy;
  llvm::IntegerType *IntPtrTy;  // Pointer-sized integer type from DataLayout

  // Runtime function declarations
  llvm::FunctionCallee getMalloc() {
    return M.getOrInsertFunction("malloc", PtrTy, IntPtrTy);
  }

  llvm::FunctionCallee getRealloc() {
    return M.getOrInsertFunction("realloc", PtrTy, PtrTy, IntPtrTy);
  }

  llvm::FunctionCallee getFree() {
    return M.getOrInsertFunction("free", VoidTy, PtrTy);
  }

  llvm::FunctionCallee getMemcpy() {
    return M.getOrInsertFunction("memcpy", PtrTy, PtrTy, PtrTy, IntPtrTy);
  }

  llvm::FunctionCallee getMemset() {
    return M.getOrInsertFunction("memset", PtrTy, PtrTy, I32Ty, IntPtrTy);
  }

  llvm::FunctionCallee getHandleCall() {
    // uint64_t handle_call(uint64_t id, void** args, uint64_t num)
    auto *FT = llvm::FunctionType::get(I64Ty, {I64Ty, PtrTy, I64Ty}, false);
    auto Callee = M.getOrInsertFunction("handle_call", FT);
    if (auto *F = llvm::dyn_cast<llvm::Function>(Callee.getCallee())) {
      if (F->empty()) {
        F->setLinkage(llvm::GlobalValue::InternalLinkage);
        llvm::BasicBlock *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
        llvm::IRBuilder<> B(BB);
        B.CreateRet(llvm::ConstantInt::get(I64Ty, 0));
      }
    }
    return Callee;
  }

#ifdef SVM_INTERP_DEBUG
  llvm::FunctionCallee getPrintf() {
    auto *FT = llvm::FunctionType::get(I32Ty, {PtrTy}, true);
    return M.getOrInsertFunction("printf", FT);
  }
#endif

  void generateBody(llvm::Function *F,
                    llvm::Argument *ArgCode,
                    llvm::Argument *ArgSize,
                    llvm::Argument *ArgArgs,
                    llvm::Argument *ArgNum) {
    using namespace llvm;

    // Create entry block
    BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
    IRBuilder<> B(Entry);

    // Allocate locals
    // DataSeg: buf (ptr), sz (i32), cap (i32)
    Value *DsBuf = B.CreateAlloca(PtrTy, nullptr, "ds.buf");
    Value *DsSz = B.CreateAlloca(I32Ty, nullptr, "ds.sz");
    Value *DsCap = B.CreateAlloca(I32Ty, nullptr, "ds.cap");
    Value *RetVal = B.CreateAlloca(I64Ty, nullptr, "ret_val");

    // Pointer / key variables
    Value *CodePtr = B.CreateAlloca(PtrTy, nullptr, "p");
    Value *EndPtr = B.CreateAlloca(PtrTy, nullptr, "end");
    Value *InstrStart = B.CreateAlloca(PtrTy, nullptr, "instr.start");
    Value *KeyVar = B.CreateAlloca(I64Ty, nullptr, "key");
    Value *KeyStream = B.CreateAlloca(I64Ty, nullptr, "ks");
    Value *NextKey = B.CreateAlloca(I64Ty, nullptr, "next.key");
    Value *CurIp = B.CreateAlloca(I64Ty, nullptr, "cur.ip");
#ifdef SVM_INTERP_DEBUG
    Value *KeyMode = B.CreateAlloca(I8Ty, nullptr, "key.mode");
#endif

    // Initialize
    B.CreateStore(ConstantPointerNull::get(cast<PointerType>(PtrTy)), DsBuf);
    B.CreateStore(ConstantInt::get(I32Ty, 0), DsSz);
    B.CreateStore(ConstantInt::get(I32Ty, 0), DsCap);
    B.CreateStore(ConstantInt::get(I64Ty, 0), RetVal);

    // Header: 8-byte seed at offset 0
    Value *Seed = B.CreateLoad(I64Ty, ArgCode, "seed");
    B.CreateStore(Seed, KeyVar);

    // Code starts at offset 8
    Value *Start = B.CreateGEP(I8Ty, ArgCode, ConstantInt::get(I64Ty, 8), "code.start");
    B.CreateStore(Start, CodePtr);

    // end = code + size
    Value *End = B.CreateGEP(I8Ty, ArgCode, B.CreateZExt(ArgSize, I64Ty), "end.calc");
    B.CreateStore(End, EndPtr);

    // Main loop
    BasicBlock *LoopHeader = BasicBlock::Create(Ctx, "loop.header", F);
    BasicBlock *LoopBody = BasicBlock::Create(Ctx, "loop.body", F);
    BasicBlock *ExitBlock = BasicBlock::Create(Ctx, "exit", F);

    B.CreateBr(LoopHeader);

    // Loop header: check p < end
    B.SetInsertPoint(LoopHeader);
    Value *P = B.CreateLoad(PtrTy, CodePtr, "p.load");
    Value *E = B.CreateLoad(PtrTy, EndPtr, "end.load");
    Value *Cond = B.CreateICmpULT(P, E, "loop.cond");
    B.CreateCondBr(Cond, LoopBody, ExitBlock);

    // Loop body: decrypt instruction header and dispatch
    B.SetInsertPoint(LoopBody);

    Value *CurKey = B.CreateLoad(I64Ty, KeyVar, "key.cur");
    P = B.CreateLoad(PtrTy, CodePtr, "p.cur");
    B.CreateStore(P, InstrStart);

    Value *PInt = B.CreatePtrToInt(P, IntPtrTy);
    Value *BaseInt = B.CreatePtrToInt(ArgCode, IntPtrTy);
    Value *Ip = B.CreateZExt(B.CreateSub(PInt, BaseInt), I64Ty, "ip");
    B.CreateStore(Ip, CurIp);
#ifdef SVM_INTERP_DEBUG
    emitDebugFetch(B, Ip, CurKey);
    emitDebugPtr(B, P, E);
#endif

    // Try with current key
    Value *TmpPtr = B.CreateAlloca(PtrTy, nullptr, "tmp.p");
    Value *TmpKey = B.CreateAlloca(I64Ty, nullptr, "tmp.key");
    B.CreateStore(P, TmpPtr);
    B.CreateStore(CurKey, TmpKey);
    Value *Len1 = readEncU16(B, TmpPtr, EndPtr, TmpKey);
    Value *Crc1 = readEncU16(B, TmpPtr, EndPtr, TmpKey);
    Value *TmpPtr1 = B.CreateLoad(PtrTy, TmpPtr);
    Value *Rem1 = B.CreateZExt(B.CreateSub(B.CreatePtrToInt(EndPtr, IntPtrTy),
                              B.CreatePtrToInt(TmpPtr1, IntPtrTy)), I64Ty);
    Value *Len1_64 = B.CreateZExt(Len1, I64Ty);
    Value *LenOk1 = B.CreateICmpULE(Len1_64, Rem1);
#ifdef SVM_INTERP_DEBUG
    emitDebugHdr(B, Ip, Len1, Crc1, LenOk1);
#endif
    BasicBlock *Crc1BB = BasicBlock::Create(Ctx, "crc1.chk", F);
    BasicBlock *Crc1BadBB = BasicBlock::Create(Ctx, "crc1.badlen", F);
    BasicBlock *Crc1MergeBB = BasicBlock::Create(Ctx, "crc1.merge", F);
    B.CreateCondBr(LenOk1, Crc1BB, Crc1BadBB);

    B.SetInsertPoint(Crc1BB);
    Value *Calc1 = computeCrcOverPayload(B, TmpPtr, EndPtr, TmpKey, Len1);
    Value *CrcOk1 = B.CreateICmpEQ(Calc1, Crc1);
    B.CreateBr(Crc1MergeBB);
    BasicBlock *Crc1OkEnd = B.GetInsertBlock();

    B.SetInsertPoint(Crc1BadBB);
    B.CreateBr(Crc1MergeBB);
    BasicBlock *Crc1BadEnd = B.GetInsertBlock();

    B.SetInsertPoint(Crc1MergeBB);
    PHINode *Ok1 = B.CreatePHI(I1Ty, 2, "crc1.ok");
    Ok1->addIncoming(CrcOk1, Crc1OkEnd);
    Ok1->addIncoming(ConstantInt::getFalse(Ctx), Crc1BadEnd);

    // If CRC fails, retry with hash(seed, ip)
    Value *HashKeyVar = B.CreateAlloca(I64Ty, nullptr, "key.hash");
    BasicBlock *UseCurBB = BasicBlock::Create(Ctx, "key.use.cur", F);
    BasicBlock *TryHashBB = BasicBlock::Create(Ctx, "key.try.hash", F);
    BasicBlock *UseHashBB = BasicBlock::Create(Ctx, "key.use.hash", F);
    BasicBlock *BadKeyBB = BasicBlock::Create(Ctx, "key.bad", F);
    BasicBlock *DecodeBB = BasicBlock::Create(Ctx, "decode", F);
    B.CreateCondBr(Ok1, UseCurBB, TryHashBB);

    B.SetInsertPoint(UseCurBB);
    B.CreateStore(CurKey, KeyVar);
#ifdef SVM_INTERP_DEBUG
    B.CreateStore(ConstantInt::get(I8Ty, 0), KeyMode);
#endif
    B.CreateBr(DecodeBB);

    B.SetInsertPoint(TryHashBB);
#ifdef SVM_INTERP_DEBUG
    emitDebugCrcFail(B, Ip, Len1, Crc1, LenOk1);
#endif
    Value *HashKey = hashSeedIp(B, Seed, Ip);
    B.CreateStore(HashKey, HashKeyVar);
    B.CreateStore(P, TmpPtr);
    B.CreateStore(HashKey, TmpKey);
    Value *Len2 = readEncU16(B, TmpPtr, EndPtr, TmpKey);
    Value *Crc2 = readEncU16(B, TmpPtr, EndPtr, TmpKey);
    Value *TmpPtr2 = B.CreateLoad(PtrTy, TmpPtr);
    Value *Rem2 = B.CreateZExt(B.CreateSub(B.CreatePtrToInt(EndPtr, IntPtrTy),
                              B.CreatePtrToInt(TmpPtr2, IntPtrTy)), I64Ty);
    Value *Len2_64 = B.CreateZExt(Len2, I64Ty);
    Value *LenOk2 = B.CreateICmpULE(Len2_64, Rem2);
    BasicBlock *Crc2BB = BasicBlock::Create(Ctx, "crc2.chk", F);
    BasicBlock *Crc2BadBB = BasicBlock::Create(Ctx, "crc2.badlen", F);
    BasicBlock *Crc2MergeBB = BasicBlock::Create(Ctx, "crc2.merge", F);
    B.CreateCondBr(LenOk2, Crc2BB, Crc2BadBB);

    B.SetInsertPoint(Crc2BB);
    Value *Calc2 = computeCrcOverPayload(B, TmpPtr, EndPtr, TmpKey, Len2);
    Value *CrcOk2 = B.CreateICmpEQ(Calc2, Crc2);
    B.CreateBr(Crc2MergeBB);
    BasicBlock *Crc2OkEnd = B.GetInsertBlock();

    B.SetInsertPoint(Crc2BadBB);
    B.CreateBr(Crc2MergeBB);
    BasicBlock *Crc2BadEnd = B.GetInsertBlock();

    B.SetInsertPoint(Crc2MergeBB);
    PHINode *Ok2 = B.CreatePHI(I1Ty, 2, "crc2.ok");
    Ok2->addIncoming(CrcOk2, Crc2OkEnd);
    Ok2->addIncoming(ConstantInt::getFalse(Ctx), Crc2BadEnd);
    B.CreateCondBr(Ok2, UseHashBB, BadKeyBB);

    B.SetInsertPoint(UseHashBB);
    Value *HashKey2 = B.CreateLoad(I64Ty, HashKeyVar);
    B.CreateStore(HashKey2, KeyVar);
#ifdef SVM_INTERP_DEBUG
    B.CreateStore(ConstantInt::get(I8Ty, 1), KeyMode);
#endif
    B.CreateBr(DecodeBB);

    B.SetInsertPoint(BadKeyBB);
#ifdef SVM_INTERP_DEBUG
    emitDebugBadKey(B, Ip);
#endif
    B.CreateBr(ExitBlock);

    // Decode with chosen key
    B.SetInsertPoint(DecodeBB);
    Value *ChosenKey = B.CreateLoad(I64Ty, KeyVar);
    Value *StartP = B.CreateLoad(PtrTy, InstrStart);
    B.CreateStore(StartP, CodePtr);
    B.CreateStore(ChosenKey, KeyStream);

    Value *Len = readEncU16(B, CodePtr, EndPtr, KeyStream);
    (void)readEncU16(B, CodePtr, EndPtr, KeyStream); // crc16
    Value *Opcode = readEncU8(B, CodePtr, EndPtr, KeyStream);
    Value *Next = mixKey(B, ChosenKey, Ip, Opcode, Len);
    B.CreateStore(Next, NextKey);
#ifdef SVM_INTERP_DEBUG
    emitDebugDecode(B, Ip, Len, Opcode, ChosenKey, KeyMode);
#endif

    // Create switch for opcodes
    BasicBlock *DefaultBB = BasicBlock::Create(Ctx, "op.default", F);
    SwitchInst *Sw = B.CreateSwitch(Opcode, DefaultBB, 14);

    // Generate handlers for each opcode
    BasicBlock *LoopNext = BasicBlock::Create(Ctx, "loop.next", F);
    generateOpParamMap(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap, ArgArgs, ArgNum, LoopNext);
    generateOpAlloca(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap, CurIp, LoopNext);
    generateOpLoad(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap, CurIp, LoopNext);
    generateOpStore(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap, CurIp, LoopNext);
    generateOpGep(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap, LoopNext);
    generateOpCmp(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap, LoopNext);
    generateOpCast(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap, LoopNext);
    generateOpBr(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap,
                 ArgCode, CurIp, Seed, NextKey, LoopNext);
    generateOpArith(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap, LoopNext);
    generateOpCall(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap, LoopNext);
    generateOpRet(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap, RetVal, ExitBlock);
    generateOpSelect(F, Sw, CodePtr, EndPtr, KeyStream, DsBuf, DsSz, DsCap, LoopNext);

    // Default: go to exit
    IRBuilder<> BD(DefaultBB);
    BD.CreateBr(ExitBlock);

    B.SetInsertPoint(LoopNext);
    Value *NK = B.CreateLoad(I64Ty, NextKey);
    B.CreateStore(NK, KeyVar);
    B.CreateBr(LoopHeader);

    // Exit block: cleanup and return
    B.SetInsertPoint(ExitBlock);
    Value *BufToFree = B.CreateLoad(PtrTy, DsBuf);
    B.CreateCall(getFree(), {BufToFree});
    Value *Ret = B.CreateLoad(I64Ty, RetVal);
    // Truncate to IntPtrTy for 32-bit targets
    B.CreateRet(B.CreateTruncOrBitCast(Ret, IntPtrTy));
  }

  // xxhash-like mix / stream helpers
  llvm::Value* rotl64(llvm::IRBuilder<> &B, llvm::Value *V, unsigned r) {
    using namespace llvm;
    Value *Shl = B.CreateShl(V, r);
    Value *Shr = B.CreateLShr(V, 64 - r);
    return B.CreateOr(Shl, Shr);
  }

  llvm::Value* mixKey(llvm::IRBuilder<> &B, llvm::Value *Prev,
                      llvm::Value *Ip, llvm::Value *Opcode, llvm::Value *Len) {
    using namespace llvm;
    Value *P1 = ConstantInt::get(I64Ty, 11400714785074694791ULL);
    Value *P2 = ConstantInt::get(I64Ty, 14029467366897019727ULL);
    Value *P3 = ConstantInt::get(I64Ty, 1609587929392839161ULL);
    Value *P4 = ConstantInt::get(I64Ty, 9650029242287828579ULL);

    Value *K = B.CreateAdd(Prev, P1);
    Value *Ip64 = B.CreateZExtOrTrunc(Ip, I64Ty);
    Value *Op64 = B.CreateZExt(Opcode, I64Ty);
    Value *Len64 = B.CreateZExt(Len, I64Ty);
    K = B.CreateXor(K, B.CreateMul(Ip64, P2));
    K = B.CreateXor(K, B.CreateMul(Op64, P3));
    K = B.CreateXor(K, B.CreateMul(Len64, P4));
    K = B.CreateMul(rotl64(B, K, 31), P1);

    K = B.CreateXor(K, B.CreateLShr(K, 33));
    K = B.CreateMul(K, P2);
    K = B.CreateXor(K, B.CreateLShr(K, 29));
    K = B.CreateMul(K, P3);
    K = B.CreateXor(K, B.CreateLShr(K, 32));
    return K;
  }

  llvm::Value* hashSeedIp(llvm::IRBuilder<> &B, llvm::Value *Seed, llvm::Value *Ip) {
    using namespace llvm;
    Value *P1 = ConstantInt::get(I64Ty, 11400714785074694791ULL);
    Value *P2 = ConstantInt::get(I64Ty, 14029467366897019727ULL);
    Value *P3 = ConstantInt::get(I64Ty, 1609587929392839161ULL);

    Value *K = B.CreateAdd(Seed, P1);
    Value *Ip64 = B.CreateZExtOrTrunc(Ip, I64Ty);
    K = B.CreateXor(K, B.CreateMul(Ip64, P2));
    K = B.CreateMul(rotl64(B, K, 31), P1);
    K = B.CreateXor(K, B.CreateLShr(K, 33));
    K = B.CreateMul(K, P2);
    K = B.CreateXor(K, B.CreateLShr(K, 29));
    K = B.CreateMul(K, P3);
    K = B.CreateXor(K, B.CreateLShr(K, 32));
    return K;
  }

  llvm::Value* nextStreamByte(llvm::IRBuilder<> &B, llvm::Value *KeyState) {
    using namespace llvm;
    Value *P1 = ConstantInt::get(I64Ty, 11400714785074694791ULL);
    Value *P5 = ConstantInt::get(I64Ty, 2870177450012600261ULL);
    Value *K = B.CreateLoad(I64Ty, KeyState);
    K = B.CreateAdd(K, P5);
    K = B.CreateMul(rotl64(B, K, 17), P1);
    B.CreateStore(K, KeyState);
    return B.CreateTrunc(K, I8Ty);
  }

  // Helper: read encrypted u8 from bytecode
  llvm::Value* readEncU8(llvm::IRBuilder<> &B, llvm::Value *CodePtr,
                         llvm::Value *EndPtr, llvm::Value *KeyState) {
    using namespace llvm;
    Value *P = B.CreateLoad(PtrTy, CodePtr);
    Value *Enc = B.CreateLoad(I8Ty, P);
    Value *Ks = nextStreamByte(B, KeyState);
    Value *Dec = B.CreateXor(Enc, Ks);
    Value *PNext = B.CreateGEP(I8Ty, P, ConstantInt::get(I64Ty, 1));
    B.CreateStore(PNext, CodePtr);
    return Dec;
  }

  llvm::Value* readEncU16(llvm::IRBuilder<> &B, llvm::Value *CodePtr,
                          llvm::Value *EndPtr, llvm::Value *KeyState) {
    using namespace llvm;
    Value *B0 = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *B1 = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *V = B.CreateZExt(B0, I32Ty);
    V = B.CreateOr(V, B.CreateShl(B.CreateZExt(B1, I32Ty), 8));
    return V;
  }

  // Helper: read encrypted u32 from bytecode (little endian)
  llvm::Value* readEncU32(llvm::IRBuilder<> &B, llvm::Value *CodePtr,
                          llvm::Value *EndPtr, llvm::Value *KeyState) {
    using namespace llvm;
    Value *B0 = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *B1 = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *B2 = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *B3 = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *V = B.CreateZExt(B0, I32Ty);
    V = B.CreateOr(V, B.CreateShl(B.CreateZExt(B1, I32Ty), 8));
    V = B.CreateOr(V, B.CreateShl(B.CreateZExt(B2, I32Ty), 16));
    V = B.CreateOr(V, B.CreateShl(B.CreateZExt(B3, I32Ty), 24));
    return V;
  }

  // Helper: read encrypted u64 from bytecode (little endian)
  llvm::Value* readEncU64(llvm::IRBuilder<> &B, llvm::Value *CodePtr,
                          llvm::Value *EndPtr, llvm::Value *KeyState) {
    using namespace llvm;
    Value *V = ConstantInt::get(I64Ty, 0);
    for (int i = 0; i < 8; i++) {
      Value *Bi = readEncU8(B, CodePtr, EndPtr, KeyState);
      Value *Shifted = B.CreateShl(B.CreateZExt(Bi, I64Ty), i * 8);
      V = B.CreateOr(V, Shifted);
    }
    return V;
  }

  llvm::Value* crc16Update(llvm::IRBuilder<> &B, llvm::Value *Crc, llvm::Value *Byte) {
    using namespace llvm;
    Value *C = B.CreateXor(Crc, B.CreateShl(B.CreateZExt(Byte, I32Ty), 8));
    for (int i = 0; i < 8; ++i) {
      Value *Msb = B.CreateAnd(C, ConstantInt::get(I32Ty, 0x8000));
      Value *Shift = B.CreateShl(C, 1);
      Value *XorVal = B.CreateXor(Shift, ConstantInt::get(I32Ty, 0x1021));
      Value *UseXor = B.CreateICmpNE(Msb, ConstantInt::get(I32Ty, 0));
      C = B.CreateSelect(UseXor, XorVal, Shift);
    }
    return B.CreateAnd(C, ConstantInt::get(I32Ty, 0xFFFF));
  }

  llvm::Value* computeCrcOverPayload(llvm::IRBuilder<> &B, llvm::Value *TmpPtr,
                                     llvm::Value *EndPtr, llvm::Value *KeyState,
                                     llvm::Value *Len) {
    using namespace llvm;
    Function *F = B.GetInsertBlock()->getParent();
    Value *CrcVar = B.CreateAlloca(I32Ty, nullptr, "crc");
    B.CreateStore(ConstantInt::get(I32Ty, 0xFFFF), CrcVar);

    Value *Idx = B.CreateAlloca(I32Ty, nullptr, "crc.i");
    B.CreateStore(ConstantInt::get(I32Ty, 0), Idx);

    BasicBlock *LoopHdr = BasicBlock::Create(Ctx, "crc.hdr", F);
    BasicBlock *LoopBody = BasicBlock::Create(Ctx, "crc.body", F);
    BasicBlock *LoopEnd = BasicBlock::Create(Ctx, "crc.end", F);
    B.CreateBr(LoopHdr);

    B.SetInsertPoint(LoopHdr);
    Value *I = B.CreateLoad(I32Ty, Idx);
    Value *Cond = B.CreateICmpULT(I, Len);
    B.CreateCondBr(Cond, LoopBody, LoopEnd);

    B.SetInsertPoint(LoopBody);
    Value *Byte = readEncU8(B, TmpPtr, EndPtr, KeyState);
    Value *Crc = B.CreateLoad(I32Ty, CrcVar);
    Value *CrcNext = crc16Update(B, Crc, Byte);
    B.CreateStore(CrcNext, CrcVar);
    Value *IInc = B.CreateAdd(I, ConstantInt::get(I32Ty, 1));
    B.CreateStore(IInc, Idx);
    B.CreateBr(LoopHdr);

    B.SetInsertPoint(LoopEnd);
    return B.CreateLoad(I32Ty, CrcVar);
  }

#ifdef SVM_INTERP_DEBUG
  void emitDebugDecode(llvm::IRBuilder<> &B, llvm::Value *Ip, llvm::Value *Len,
                       llvm::Value *Opcode, llvm::Value *Key,
                       llvm::Value *KeyMode) {
    using namespace llvm;
    Value *Ip64 = B.CreateZExtOrTrunc(Ip, I64Ty);
    Value *Len32 = B.CreateZExtOrTrunc(Len, I32Ty);
    Value *Op32 = B.CreateZExt(Opcode, I32Ty);
    Value *Mode32 = B.CreateZExt(B.CreateLoad(I8Ty, KeyMode), I32Ty);
    Value *Fmt = B.CreateGlobalStringPtr(
        "[svm.exec] ip=0x%llx len=%u op=0x%02x key=0x%llx mode=%u\n");
    B.CreateCall(getPrintf(), {Fmt, Ip64, Len32, Op32, Key, Mode32});
  }

  void emitDebugBadKey(llvm::IRBuilder<> &B, llvm::Value *Ip) {
    using namespace llvm;
    Value *Ip64 = B.CreateZExtOrTrunc(Ip, I64Ty);
    Value *Fmt = B.CreateGlobalStringPtr("[svm.exec] badkey ip=0x%llx\n");
    B.CreateCall(getPrintf(), {Fmt, Ip64});
  }

  void emitDebugFetch(llvm::IRBuilder<> &B, llvm::Value *Ip, llvm::Value *Key) {
    using namespace llvm;
    Value *Ip64 = B.CreateZExtOrTrunc(Ip, I64Ty);
    Value *Fmt = B.CreateGlobalStringPtr(
        "[svm.exec] fetch ip=0x%llx key=0x%llx\n");
    B.CreateCall(getPrintf(), {Fmt, Ip64, Key});
  }

  void emitDebugPtr(llvm::IRBuilder<> &B, llvm::Value *P, llvm::Value *End) {
    using namespace llvm;
    Value *P64 = B.CreateZExt(B.CreatePtrToInt(P, IntPtrTy), I64Ty);
    Value *E64 = B.CreateZExt(B.CreatePtrToInt(End, IntPtrTy), I64Ty);
    Value *Fmt = B.CreateGlobalStringPtr(
        "[svm.exec] ptr p=0x%llx end=0x%llx\n");
    B.CreateCall(getPrintf(), {Fmt, P64, E64});
  }

  void emitDebugHdr(llvm::IRBuilder<> &B, llvm::Value *Ip,
                    llvm::Value *Len, llvm::Value *Crc,
                    llvm::Value *LenOk) {
    using namespace llvm;
    Value *Ip64 = B.CreateZExtOrTrunc(Ip, I64Ty);
    Value *Len32 = B.CreateZExtOrTrunc(Len, I32Ty);
    Value *Crc32 = B.CreateZExtOrTrunc(Crc, I32Ty);
    Value *LenOk32 = B.CreateZExtOrTrunc(LenOk, I32Ty);
    Value *Fmt = B.CreateGlobalStringPtr(
        "[svm.exec] hdr ip=0x%llx len=%u crc=0x%04x lenok=%u\n");
    B.CreateCall(getPrintf(), {Fmt, Ip64, Len32, Crc32, LenOk32});
  }

  void emitDebugCrcFail(llvm::IRBuilder<> &B, llvm::Value *Ip,
                        llvm::Value *Len, llvm::Value *Crc,
                        llvm::Value *LenOk) {
    using namespace llvm;
    Value *Ip64 = B.CreateZExtOrTrunc(Ip, I64Ty);
    Value *Len32 = B.CreateZExtOrTrunc(Len, I32Ty);
    Value *Crc32 = B.CreateZExtOrTrunc(Crc, I32Ty);
    Value *LenOk32 = B.CreateZExtOrTrunc(LenOk, I32Ty);
    Value *Fmt = B.CreateGlobalStringPtr(
        "[svm.exec] crc1.fail ip=0x%llx len=%u crc=0x%04x lenok=%u\n");
    B.CreateCall(getPrintf(), {Fmt, Ip64, Len32, Crc32, LenOk32});
  }

  void emitDebugStore(llvm::IRBuilder<> &B, llvm::Value *Ip,
                      llvm::Value *Ptr, llvm::Value *Val,
                      llvm::Value *IsSlot) {
    using namespace llvm;
    Value *Ip64 = B.CreateZExtOrTrunc(Ip, I64Ty);
    Value *Ptr64 = B.CreateZExtOrTrunc(Ptr, I64Ty);
    Value *Val64 = B.CreateZExtOrTrunc(Val, I64Ty);
    Value *IsSlot32 = B.CreateZExt(IsSlot, I32Ty);
    Value *Fmt = B.CreateGlobalStringPtr(
        "[svm.exec] store ip=0x%llx ptr=0x%llx val=0x%llx slot=%u\n");
    B.CreateCall(getPrintf(), {Fmt, Ip64, Ptr64, Val64, IsSlot32});
  }

  void emitDebugBr(llvm::IRBuilder<> &B, llvm::Value *Ip, llvm::Value *TargetOff) {
    using namespace llvm;
    Value *Ip64 = B.CreateZExtOrTrunc(Ip, I64Ty);
    Value *T32 = B.CreateZExtOrTrunc(TargetOff, I32Ty);
    Value *Fmt = B.CreateGlobalStringPtr(
        "[svm.exec] br ip=0x%llx target=0x%08x\n");
    B.CreateCall(getPrintf(), {Fmt, Ip64, T32});
  }

  void emitDebugAlloca(llvm::IRBuilder<> &B, llvm::Value *Ip,
                       llvm::Value *Dst, llvm::Value *Area,
                       llvm::Value *PtrVal) {
    using namespace llvm;
    Value *Ip64 = B.CreateZExtOrTrunc(Ip, I64Ty);
    Value *Dst32 = B.CreateZExtOrTrunc(Dst, I32Ty);
    Value *Area32 = B.CreateZExtOrTrunc(Area, I32Ty);
    Value *Ptr64 = B.CreateZExtOrTrunc(PtrVal, I64Ty);
    Value *Fmt = B.CreateGlobalStringPtr(
        "[svm.exec] alloca ip=0x%llx dst=%u area=%u ptr=0x%llx\n");
    B.CreateCall(getPrintf(), {Fmt, Ip64, Dst32, Area32, Ptr64});
  }
#endif

  // Helper: ensure data segment capacity
  void ensureData(llvm::IRBuilder<> &B, llvm::Value *DsBuf, llvm::Value *DsSz,
                  llvm::Value *DsCap, llvm::Value *Need) {
    using namespace llvm;

    // Load current values
    Value *Cap = B.CreateLoad(I32Ty, DsCap);
    Value *NeedCmp = B.CreateICmpUGT(Need, Cap);

    // Create blocks for realloc path
    Function *F = B.GetInsertBlock()->getParent();
    BasicBlock *ReallocBB = BasicBlock::Create(Ctx, "ensure.realloc", F);
    BasicBlock *ContBB = BasicBlock::Create(Ctx, "ensure.cont", F);

    B.CreateCondBr(NeedCmp, ReallocBB, ContBB);

    // Realloc path
    B.SetInsertPoint(ReallocBB);
    // Calculate new capacity: max(4096, cap * 2) until >= need
    Value *NewCap = B.CreateSelect(
        B.CreateICmpEQ(Cap, ConstantInt::get(I32Ty, 0)),
        ConstantInt::get(I32Ty, 4096),
        B.CreateShl(Cap, 1));

    // Simple: just use need * 2 for now
    NewCap = B.CreateShl(Need, 1);
    NewCap = B.CreateSelect(
        B.CreateICmpULT(NewCap, ConstantInt::get(I32Ty, 4096)),
        ConstantInt::get(I32Ty, 4096), NewCap);

    Value *OldBuf = B.CreateLoad(PtrTy, DsBuf);
    Value *NewBuf = B.CreateCall(getRealloc(),
        {OldBuf, B.CreateZExtOrTrunc(NewCap, IntPtrTy)});

    // Memset new area
    Value *OldCap = B.CreateLoad(I32Ty, DsCap);
    Value *NewArea = B.CreateGEP(I8Ty, NewBuf, B.CreateZExt(OldCap, I64Ty));
    Value *ClearLen = B.CreateSub(NewCap, OldCap);
    B.CreateCall(getMemset(), {NewArea, ConstantInt::get(I32Ty, 0), B.CreateZExtOrTrunc(ClearLen, IntPtrTy)});

    B.CreateStore(NewBuf, DsBuf);
    B.CreateStore(NewCap, DsCap);
    B.CreateStore(Need, DsSz);
    B.CreateBr(ContBB);

    B.SetInsertPoint(ContBB);
  }

  // Helper: store u64 to data segment
  void dsStoreU64(llvm::IRBuilder<> &B, llvm::Value *DsBuf, llvm::Value *DsSz,
                  llvm::Value *DsCap, llvm::Value *Off, llvm::Value *Val) {
    using namespace llvm;
    Value *OffPlus8 = B.CreateAdd(Off, ConstantInt::get(I32Ty, 8));
    ensureData(B, DsBuf, DsSz, DsCap, OffPlus8);
    Value *Buf = B.CreateLoad(PtrTy, DsBuf);
    Value *Ptr = B.CreateGEP(I8Ty, Buf, B.CreateZExt(Off, I64Ty));
    B.CreateStore(Val, Ptr);
  }

  // Helper: load u64 from data segment
  llvm::Value* dsLoadU64(llvm::IRBuilder<> &B, llvm::Value *DsBuf, llvm::Value *DsSz,
                         llvm::Value *DsCap, llvm::Value *Off) {
    using namespace llvm;
    Value *OffPlus8 = B.CreateAdd(Off, ConstantInt::get(I32Ty, 8));
    ensureData(B, DsBuf, DsSz, DsCap, OffPlus8);
    Value *Buf = B.CreateLoad(PtrTy, DsBuf);
    Value *Ptr = B.CreateGEP(I8Ty, Buf, B.CreateZExt(Off, I64Ty));
    return B.CreateLoad(I64Ty, Ptr);
  }

  // Read encoded value from bytecode
  // Returns: {is_slot, value_u64, slot_off}
  struct EncodedValue {
    llvm::Value *IsSlot;
    llvm::Value *ValueU64;
    llvm::Value *SlotOff;
  };

  EncodedValue readValue(llvm::IRBuilder<> &B, llvm::Value *CodePtr, llvm::Value *EndPtr,
                         llvm::Value *KeyState,
                         llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap) {
    using namespace llvm;
    EncodedValue EV;

    Value *Tag = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *IsSlot = B.CreateICmpEQ(Tag, ConstantInt::get(I8Ty, VT_SLOT));

    Function *F = B.GetInsertBlock()->getParent();
    BasicBlock *SlotBB = BasicBlock::Create(Ctx, "val.slot", F);
    BasicBlock *ConstBB = BasicBlock::Create(Ctx, "val.const", F);
    BasicBlock *MergeBB = BasicBlock::Create(Ctx, "val.merge", F);

    B.CreateCondBr(IsSlot, SlotBB, ConstBB);

    // Slot path
    B.SetInsertPoint(SlotBB);
    Value *SlotOff = readEncU32(B, CodePtr, EndPtr, KeyState);
    Value *SlotVal = dsLoadU64(B, DsBuf, DsSz, DsCap, SlotOff);
    B.CreateBr(MergeBB);
    BasicBlock *SlotBBEnd = B.GetInsertBlock();

    // Const path
    B.SetInsertPoint(ConstBB);
    Value *Ck = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *Sz = readEncU32(B, CodePtr, EndPtr, KeyState);
    // For simplicity, always read 8 bytes (handles most cases)
    Value *ConstVal = readEncU64(B, CodePtr, EndPtr, KeyState);
    B.CreateBr(MergeBB);
    BasicBlock *ConstBBEnd = B.GetInsertBlock();

    // Merge
    B.SetInsertPoint(MergeBB);
    PHINode *ValPhi = B.CreatePHI(I64Ty, 2, "val.u64");
    ValPhi->addIncoming(SlotVal, SlotBBEnd);
    ValPhi->addIncoming(ConstVal, ConstBBEnd);

    PHINode *OffPhi = B.CreatePHI(I32Ty, 2, "val.off");
    OffPhi->addIncoming(SlotOff, SlotBBEnd);
    OffPhi->addIncoming(ConstantInt::get(I32Ty, 0), ConstBBEnd);

    PHINode *IsSlotPhi = B.CreatePHI(I1Ty, 2, "val.isslot");
    IsSlotPhi->addIncoming(ConstantInt::getTrue(Ctx), SlotBBEnd);
    IsSlotPhi->addIncoming(ConstantInt::getFalse(Ctx), ConstBBEnd);

    EV.IsSlot = IsSlotPhi;
    EV.ValueU64 = ValPhi;
    EV.SlotOff = OffPhi;

    return EV;
  }

  // OP_PARAMMAP handler
  void generateOpParamMap(llvm::Function *F, llvm::SwitchInst *Sw,
                          llvm::Value *CodePtr, llvm::Value *EndPtr,
                          llvm::Value *KeyState,
                          llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                          llvm::Value *ArgArgs, llvm::Value *ArgNum,
                          llvm::BasicBlock *LoopNext) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.parammap", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_PARAMMAP)), BB);

    IRBuilder<> B(BB);
    Value *N = readEncU32(B, CodePtr, EndPtr, KeyState);

    // Loop: for i = 0 to N
    BasicBlock *LoopHdr = BasicBlock::Create(Ctx, "pm.loop.hdr", F);
    BasicBlock *LoopBody = BasicBlock::Create(Ctx, "pm.loop.body", F);
    BasicBlock *LoopEnd = BasicBlock::Create(Ctx, "pm.loop.end", F);

    Value *I = B.CreateAlloca(I32Ty);
    B.CreateStore(ConstantInt::get(I32Ty, 0), I);
    B.CreateBr(LoopHdr);

    B.SetInsertPoint(LoopHdr);
    Value *Idx = B.CreateLoad(I32Ty, I);
    Value *Cond = B.CreateICmpULT(Idx, N);
    B.CreateCondBr(Cond, LoopBody, LoopEnd);

    B.SetInsertPoint(LoopBody);
    Value *Off = readEncU32(B, CodePtr, EndPtr, KeyState);
    Value *Sz = readEncU32(B, CodePtr, EndPtr, KeyState);

    // Ensure data segment has space
    Value *OffPlusSz = B.CreateAdd(Off, Sz);
    ensureData(B, DsBuf, DsSz, DsCap, OffPlusSz);

    // Check if i < num && args[i] != null
    Value *Idx64 = B.CreateZExt(Idx, I64Ty);
    Value *InRange = B.CreateICmpULT(Idx64, ArgNum);

    BasicBlock *CopyBB = BasicBlock::Create(Ctx, "pm.copy", F);
    BasicBlock *ZeroBB = BasicBlock::Create(Ctx, "pm.zero", F);
    BasicBlock *NextBB = BasicBlock::Create(Ctx, "pm.next", F);

    B.CreateCondBr(InRange, CopyBB, ZeroBB);

    // Copy from args[i]
    B.SetInsertPoint(CopyBB);
    Value *ArgsSlot = B.CreateGEP(PtrTy, ArgArgs, Idx64);
    Value *ArgPtr = B.CreateLoad(PtrTy, ArgsSlot);
    Value *ArgNotNull = B.CreateICmpNE(ArgPtr, ConstantPointerNull::get(cast<PointerType>(PtrTy)));

    BasicBlock *DoCopyBB = BasicBlock::Create(Ctx, "pm.docopy", F);
    B.CreateCondBr(ArgNotNull, DoCopyBB, ZeroBB);

    B.SetInsertPoint(DoCopyBB);
    Value *Buf = B.CreateLoad(PtrTy, DsBuf);
    Value *DstPtr = B.CreateGEP(I8Ty, Buf, B.CreateZExt(Off, I64Ty));
    B.CreateCall(getMemcpy(), {DstPtr, ArgPtr, B.CreateZExtOrTrunc(Sz, IntPtrTy)});
    B.CreateBr(NextBB);

    // Zero fill
    B.SetInsertPoint(ZeroBB);
    Buf = B.CreateLoad(PtrTy, DsBuf);
    DstPtr = B.CreateGEP(I8Ty, Buf, B.CreateZExt(Off, I64Ty));
    B.CreateCall(getMemset(), {DstPtr, ConstantInt::get(I32Ty, 0), B.CreateZExtOrTrunc(Sz, IntPtrTy)});
    B.CreateBr(NextBB);

    // Next iteration
    B.SetInsertPoint(NextBB);
    Value *IdxInc = B.CreateAdd(B.CreateLoad(I32Ty, I), ConstantInt::get(I32Ty, 1));
    B.CreateStore(IdxInc, I);
    B.CreateBr(LoopHdr);

    // End
    B.SetInsertPoint(LoopEnd);
    B.CreateBr(LoopNext);
  }

  // OP_ALLOCA handler
  void generateOpAlloca(llvm::Function *F, llvm::SwitchInst *Sw,
                        llvm::Value *CodePtr, llvm::Value *EndPtr,
                        llvm::Value *KeyState,
                        llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                        llvm::Value *CurIp,
                        llvm::BasicBlock *LoopNext) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.alloca", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_ALLOCA)), BB);

    IRBuilder<> B(BB);
    Value *Dst = readEncU32(B, CodePtr, EndPtr, KeyState);
    Value *Area = readEncU32(B, CodePtr, EndPtr, KeyState);

    // malloc(area)
    Value *Mem = B.CreateCall(getMalloc(), {B.CreateZExtOrTrunc(Area, IntPtrTy)});
    Value *MemI64 = B.CreateZExt(B.CreatePtrToInt(Mem, IntPtrTy), I64Ty);

    // Store to data segment
    dsStoreU64(B, DsBuf, DsSz, DsCap, Dst, MemI64);
#ifdef SVM_INTERP_DEBUG
    emitDebugAlloca(B, B.CreateLoad(I64Ty, CurIp), Dst, Area, MemI64);
#endif

    B.CreateBr(LoopNext);
  }

  // OP_LOAD handler
  void generateOpLoad(llvm::Function *F, llvm::SwitchInst *Sw,
                      llvm::Value *CodePtr, llvm::Value *EndPtr,
                      llvm::Value *KeyState,
                      llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                      llvm::Value *CurIp,
                      llvm::BasicBlock *LoopNext) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.load", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_LOAD)), BB);

    IRBuilder<> B(BB);
    Value *Dst = readEncU32(B, CodePtr, EndPtr, KeyState);
    EncodedValue PtrVal = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);

    // Load 8 bytes from address
    Value *Addr = B.CreateIntToPtr(PtrVal.ValueU64, PtrTy);
    Value *Val = B.CreateLoad(I64Ty, Addr);

    dsStoreU64(B, DsBuf, DsSz, DsCap, Dst, Val);
    B.CreateBr(LoopNext);
  }

  // OP_STORE handler
  void generateOpStore(llvm::Function *F, llvm::SwitchInst *Sw,
                       llvm::Value *CodePtr, llvm::Value *EndPtr,
                       llvm::Value *KeyState,
                       llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                       llvm::Value *CurIp,
                       llvm::BasicBlock *LoopNext) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.store", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_STORE)), BB);

    IRBuilder<> B(BB);
    EncodedValue Val = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);
    EncodedValue PtrVal = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);

#ifdef SVM_INTERP_DEBUG
    emitDebugStore(B, B.CreateLoad(I64Ty, CurIp), PtrVal.ValueU64, Val.ValueU64, PtrVal.IsSlot);
#endif
    Value *Addr = B.CreateIntToPtr(PtrVal.ValueU64, PtrTy);
    B.CreateStore(Val.ValueU64, Addr);

    B.CreateBr(LoopNext);
  }

  // OP_GEP handler
  void generateOpGep(llvm::Function *F, llvm::SwitchInst *Sw,
                     llvm::Value *CodePtr, llvm::Value *EndPtr,
                     llvm::Value *KeyState,
                     llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                     llvm::BasicBlock *LoopNext) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.gep", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_GEP)), BB);

    IRBuilder<> B(BB);
    Value *Dst = readEncU32(B, CodePtr, EndPtr, KeyState);
    EncodedValue Base = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);
    Value *NIdx = readEncU32(B, CodePtr, EndPtr, KeyState);

    // Loop to accumulate offsets
    Value *Off = B.CreateAlloca(I64Ty);
    B.CreateStore(ConstantInt::get(I64Ty, 0), Off);

    BasicBlock *LoopHdr = BasicBlock::Create(Ctx, "gep.loop.hdr", F);
    BasicBlock *LoopBody = BasicBlock::Create(Ctx, "gep.loop.body", F);
    BasicBlock *LoopEnd = BasicBlock::Create(Ctx, "gep.loop.end", F);

    Value *I = B.CreateAlloca(I32Ty);
    B.CreateStore(ConstantInt::get(I32Ty, 0), I);
    B.CreateBr(LoopHdr);

    B.SetInsertPoint(LoopHdr);
    Value *Idx = B.CreateLoad(I32Ty, I);
    Value *Cond = B.CreateICmpULT(Idx, NIdx);
    B.CreateCondBr(Cond, LoopBody, LoopEnd);

    B.SetInsertPoint(LoopBody);
    EncodedValue IdxVal = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);
    Value *CurOff = B.CreateLoad(I64Ty, Off);
    Value *NewOff = B.CreateAdd(CurOff, IdxVal.ValueU64);
    B.CreateStore(NewOff, Off);

    Value *IdxInc = B.CreateAdd(Idx, ConstantInt::get(I32Ty, 1));
    B.CreateStore(IdxInc, I);
    B.CreateBr(LoopHdr);

    B.SetInsertPoint(LoopEnd);
    Value *FinalOff = B.CreateLoad(I64Ty, Off);
    Value *Result = B.CreateAdd(Base.ValueU64, FinalOff);
    dsStoreU64(B, DsBuf, DsSz, DsCap, Dst, Result);
    B.CreateBr(LoopNext);
  }

  // OP_CMP handler
  void generateOpCmp(llvm::Function *F, llvm::SwitchInst *Sw,
                     llvm::Value *CodePtr, llvm::Value *EndPtr,
                     llvm::Value *KeyState,
                     llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                     llvm::BasicBlock *LoopNext) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.cmp", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_CMP)), BB);

    IRBuilder<> B(BB);
    Value *Pred = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *Dst = readEncU32(B, CodePtr, EndPtr, KeyState);
    EncodedValue L = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);
    EncodedValue R = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);

    // Simplified: treat as ICmp for now
    Value *IsFCmp = B.CreateICmpULT(Pred, ConstantInt::get(I8Ty, 32));

    BasicBlock *FCmpBB = BasicBlock::Create(Ctx, "cmp.fcmp", F);
    BasicBlock *ICmpBB = BasicBlock::Create(Ctx, "cmp.icmp", F);
    BasicBlock *MergeBB = BasicBlock::Create(Ctx, "cmp.merge", F);

    B.CreateCondBr(IsFCmp, FCmpBB, ICmpBB);

    // FCmp path (simplified - just do ordered comparison)
    B.SetInsertPoint(FCmpBB);
    Value *LF = B.CreateBitCast(L.ValueU64, F64Ty);
    Value *RF = B.CreateBitCast(R.ValueU64, F64Ty);
    // Default to OEQ for now
    Value *FCmpRes = B.CreateFCmpOEQ(LF, RF);
    Value *FCmpI64 = B.CreateZExt(FCmpRes, I64Ty);
    B.CreateBr(MergeBB);
    BasicBlock *FCmpEnd = B.GetInsertBlock();

    // ICmp path
    B.SetInsertPoint(ICmpBB);
    // Create switch for different predicates
    Value *Pred32 = B.CreateZExt(Pred, I32Ty);

    // Simplified: common cases
    Value *IsEQ = B.CreateICmpEQ(Pred32, ConstantInt::get(I32Ty, ICMP_EQ));
    Value *IsNE = B.CreateICmpEQ(Pred32, ConstantInt::get(I32Ty, ICMP_NE));
    Value *IsULT = B.CreateICmpEQ(Pred32, ConstantInt::get(I32Ty, ICMP_ULT));
    Value *IsULE = B.CreateICmpEQ(Pred32, ConstantInt::get(I32Ty, ICMP_ULE));
    Value *IsUGT = B.CreateICmpEQ(Pred32, ConstantInt::get(I32Ty, ICMP_UGT));
    Value *IsUGE = B.CreateICmpEQ(Pred32, ConstantInt::get(I32Ty, ICMP_UGE));
    Value *IsSLT = B.CreateICmpEQ(Pred32, ConstantInt::get(I32Ty, ICMP_SLT));
    Value *IsSLE = B.CreateICmpEQ(Pred32, ConstantInt::get(I32Ty, ICMP_SLE));
    Value *IsSGT = B.CreateICmpEQ(Pred32, ConstantInt::get(I32Ty, ICMP_SGT));
    Value *IsSGE = B.CreateICmpEQ(Pred32, ConstantInt::get(I32Ty, ICMP_SGE));

    Value *CmpEQ = B.CreateICmpEQ(L.ValueU64, R.ValueU64);
    Value *CmpNE = B.CreateICmpNE(L.ValueU64, R.ValueU64);
    Value *CmpULT = B.CreateICmpULT(L.ValueU64, R.ValueU64);
    Value *CmpULE = B.CreateICmpULE(L.ValueU64, R.ValueU64);
    Value *CmpUGT = B.CreateICmpUGT(L.ValueU64, R.ValueU64);
    Value *CmpUGE = B.CreateICmpUGE(L.ValueU64, R.ValueU64);
    Value *CmpSLT = B.CreateICmpSLT(L.ValueU64, R.ValueU64);
    Value *CmpSLE = B.CreateICmpSLE(L.ValueU64, R.ValueU64);
    Value *CmpSGT = B.CreateICmpSGT(L.ValueU64, R.ValueU64);
    Value *CmpSGE = B.CreateICmpSGE(L.ValueU64, R.ValueU64);

    Value *Res = B.CreateSelect(IsEQ, CmpEQ,
                  B.CreateSelect(IsNE, CmpNE,
                  B.CreateSelect(IsULT, CmpULT,
                  B.CreateSelect(IsULE, CmpULE,
                  B.CreateSelect(IsUGT, CmpUGT,
                  B.CreateSelect(IsUGE, CmpUGE,
                  B.CreateSelect(IsSLT, CmpSLT,
                  B.CreateSelect(IsSLE, CmpSLE,
                  B.CreateSelect(IsSGT, CmpSGT,
                  B.CreateSelect(IsSGE, CmpSGE, ConstantInt::getFalse(Ctx)))))))))));

    Value *ICmpI64 = B.CreateZExt(Res, I64Ty);
    B.CreateBr(MergeBB);
    BasicBlock *ICmpEnd = B.GetInsertBlock();

    // Merge
    B.SetInsertPoint(MergeBB);
    PHINode *ResPhi = B.CreatePHI(I64Ty, 2);
    ResPhi->addIncoming(FCmpI64, FCmpEnd);
    ResPhi->addIncoming(ICmpI64, ICmpEnd);

    dsStoreU64(B, DsBuf, DsSz, DsCap, Dst, ResPhi);
    B.CreateBr(LoopNext);
  }

  // OP_CAST handler
  void generateOpCast(llvm::Function *F, llvm::SwitchInst *Sw,
                      llvm::Value *CodePtr, llvm::Value *EndPtr,
                      llvm::Value *KeyState,
                      llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                      llvm::BasicBlock *LoopNext) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.cast", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_CAST)), BB);

    IRBuilder<> B(BB);
    Value *Cop = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *Dst = readEncU32(B, CodePtr, EndPtr, KeyState);
    EncodedValue V = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);

    // For most casts, just pass through or do simple transform
    // This is simplified - full implementation would handle all cast types
    Value *Result = V.ValueU64;  // Default: pass through

    dsStoreU64(B, DsBuf, DsSz, DsCap, Dst, Result);
    B.CreateBr(LoopNext);
  }

  // OP_BR handler
  void generateOpBr(llvm::Function *F, llvm::SwitchInst *Sw,
                    llvm::Value *CodePtr, llvm::Value *EndPtr,
                    llvm::Value *KeyState,
                    llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                    llvm::Value *ArgCode, llvm::Value *CurIp,
                    llvm::Value *Seed, llvm::Value *NextKey,
                    llvm::BasicBlock *LoopNext) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.br", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_BR)), BB);

    IRBuilder<> B(BB);
    Value *IsCond = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *IsCondBr = B.CreateICmpNE(IsCond, ConstantInt::get(I8Ty, 0));

    BasicBlock *UncondBB = BasicBlock::Create(Ctx, "br.uncond", F);
    BasicBlock *CondBB = BasicBlock::Create(Ctx, "br.cond", F);

    B.CreateCondBr(IsCondBr, CondBB, UncondBB);

    // Unconditional branch
    B.SetInsertPoint(UncondBB);
    Value *Target = readEncU32(B, CodePtr, EndPtr, KeyState);
#ifdef SVM_INTERP_DEBUG
    emitDebugBr(B, B.CreateLoad(I64Ty, CurIp), Target);
#endif
    Value *Target64 = B.CreateZExt(Target, I64Ty);
    Value *NewKey = hashSeedIp(B, Seed, Target64);
    B.CreateStore(NewKey, NextKey);
    Value *NewP = B.CreateGEP(I8Ty, ArgCode, B.CreateZExt(Target, I64Ty));
    B.CreateStore(NewP, CodePtr);
    B.CreateBr(LoopNext);

    // Conditional branch
    B.SetInsertPoint(CondBB);
    EncodedValue C = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);
    Value *TrueTarget = readEncU32(B, CodePtr, EndPtr, KeyState);
    Value *FalseTarget = readEncU32(B, CodePtr, EndPtr, KeyState);

    Value *Cond = B.CreateICmpNE(C.ValueU64, ConstantInt::get(I64Ty, 0));
    Value *TargetOff = B.CreateSelect(Cond, TrueTarget, FalseTarget);
#ifdef SVM_INTERP_DEBUG
    emitDebugBr(B, B.CreateLoad(I64Ty, CurIp), TargetOff);
#endif
    Value *TargetOff64 = B.CreateZExt(TargetOff, I64Ty);
    Value *NewKey2 = hashSeedIp(B, Seed, TargetOff64);
    B.CreateStore(NewKey2, NextKey);
    NewP = B.CreateGEP(I8Ty, ArgCode, B.CreateZExt(TargetOff, I64Ty));
    B.CreateStore(NewP, CodePtr);
    B.CreateBr(LoopNext);
  }

  // OP_ARITH handler
  void generateOpArith(llvm::Function *F, llvm::SwitchInst *Sw,
                       llvm::Value *CodePtr, llvm::Value *EndPtr,
                       llvm::Value *KeyState,
                       llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                       llvm::BasicBlock *LoopNext) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.arith", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_ARITH)), BB);

    IRBuilder<> B(BB);
    Value *Sub = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *Tk = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *Bits = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *Dst = readEncU32(B, CodePtr, EndPtr, KeyState);
    EncodedValue L = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);
    EncodedValue R = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);

    // Integer arithmetic
    Value *Sub32 = B.CreateZExt(Sub, I32Ty);

    Value *Add = B.CreateAdd(L.ValueU64, R.ValueU64);
    Value *SubV = B.CreateSub(L.ValueU64, R.ValueU64);
    Value *Mul = B.CreateMul(L.ValueU64, R.ValueU64);
    Value *UDiv = B.CreateUDiv(L.ValueU64, B.CreateSelect(
        B.CreateICmpEQ(R.ValueU64, ConstantInt::get(I64Ty, 0)),
        ConstantInt::get(I64Ty, 1), R.ValueU64));
    Value *SDiv = B.CreateSDiv(L.ValueU64, B.CreateSelect(
        B.CreateICmpEQ(R.ValueU64, ConstantInt::get(I64Ty, 0)),
        ConstantInt::get(I64Ty, 1), R.ValueU64));
    Value *URem = B.CreateURem(L.ValueU64, B.CreateSelect(
        B.CreateICmpEQ(R.ValueU64, ConstantInt::get(I64Ty, 0)),
        ConstantInt::get(I64Ty, 1), R.ValueU64));
    Value *SRem = B.CreateSRem(L.ValueU64, B.CreateSelect(
        B.CreateICmpEQ(R.ValueU64, ConstantInt::get(I64Ty, 0)),
        ConstantInt::get(I64Ty, 1), R.ValueU64));
    Value *Shl = B.CreateShl(L.ValueU64, B.CreateAnd(R.ValueU64, ConstantInt::get(I64Ty, 63)));
    Value *LShr = B.CreateLShr(L.ValueU64, B.CreateAnd(R.ValueU64, ConstantInt::get(I64Ty, 63)));
    Value *AShr = B.CreateAShr(L.ValueU64, B.CreateAnd(R.ValueU64, ConstantInt::get(I64Ty, 63)));
    Value *And = B.CreateAnd(L.ValueU64, R.ValueU64);
    Value *Or = B.CreateOr(L.ValueU64, R.ValueU64);
    Value *Xor = B.CreateXor(L.ValueU64, R.ValueU64);

    // FP arithmetic (f64)
    Value *LF = B.CreateBitCast(L.ValueU64, F64Ty);
    Value *RF = B.CreateBitCast(R.ValueU64, F64Ty);
    Value *FAdd = B.CreateBitCast(B.CreateFAdd(LF, RF), I64Ty);
    Value *FSub = B.CreateBitCast(B.CreateFSub(LF, RF), I64Ty);
    Value *FMul = B.CreateBitCast(B.CreateFMul(LF, RF), I64Ty);
    Value *FDiv = B.CreateBitCast(B.CreateFDiv(LF, RF), I64Ty);
    Value *FRem = B.CreateBitCast(B.CreateFRem(LF, RF), I64Ty);

    // Select result based on sub-opcode
    Value *IsAdd = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_ADD));
    Value *IsSub = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_SUB));
    Value *IsMul = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_MUL));
    Value *IsUDiv = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_UDIV));
    Value *IsSDiv = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_SDIV));
    Value *IsURem = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_UREM));
    Value *IsSRem = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_SREM));
    Value *IsShl = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_SHL));
    Value *IsLShr = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_LSHR));
    Value *IsAShr = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_ASHR));
    Value *IsAnd = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_AND));
    Value *IsOr = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_OR));
    Value *IsXor = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_XOR));
    Value *IsFAdd = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_FADD));
    Value *IsFSub = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_FSUB));
    Value *IsFMul = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_FMUL));
    Value *IsFDiv = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_FDIV));
    Value *IsFRem = B.CreateICmpEQ(Sub32, ConstantInt::get(I32Ty, AR_FREM));

    Value *Result = B.CreateSelect(IsAdd, Add,
                    B.CreateSelect(IsSub, SubV,
                    B.CreateSelect(IsMul, Mul,
                    B.CreateSelect(IsUDiv, UDiv,
                    B.CreateSelect(IsSDiv, SDiv,
                    B.CreateSelect(IsURem, URem,
                    B.CreateSelect(IsSRem, SRem,
                    B.CreateSelect(IsShl, Shl,
                    B.CreateSelect(IsLShr, LShr,
                    B.CreateSelect(IsAShr, AShr,
                    B.CreateSelect(IsAnd, And,
                    B.CreateSelect(IsOr, Or,
                    B.CreateSelect(IsXor, Xor,
                    B.CreateSelect(IsFAdd, FAdd,
                    B.CreateSelect(IsFSub, FSub,
                    B.CreateSelect(IsFMul, FMul,
                    B.CreateSelect(IsFDiv, FDiv,
                    B.CreateSelect(IsFRem, FRem, ConstantInt::get(I64Ty, 0)))))))))))))))))));

    dsStoreU64(B, DsBuf, DsSz, DsCap, Dst, Result);
    B.CreateBr(LoopNext);
  }

  // OP_CALL handler
  void generateOpCall(llvm::Function *F, llvm::SwitchInst *Sw,
                      llvm::Value *CodePtr, llvm::Value *EndPtr,
                      llvm::Value *KeyState,
                      llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                      llvm::BasicBlock *LoopNext) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.call", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_CALL)), BB);

    IRBuilder<> B(BB);
    Value *Id = readEncU64(B, CodePtr, EndPtr, KeyState);
    Value *Dst = readEncU32(B, CodePtr, EndPtr, KeyState);
    Value *DynN = readEncU32(B, CodePtr, EndPtr, KeyState);

    // Allocate args array
    Value *DynNPtr = B.CreateZExt(DynN, IntPtrTy);
    Value *ArgsSize = B.CreateMul(DynNPtr, ConstantInt::get(IntPtrTy, 8));
    Value *Args = B.CreateCall(getMalloc(), {ArgsSize});

    // Fill args array
    BasicBlock *LoopHdr = BasicBlock::Create(Ctx, "call.loop.hdr", F);
    BasicBlock *LoopBody = BasicBlock::Create(Ctx, "call.loop.body", F);
    BasicBlock *LoopEnd = BasicBlock::Create(Ctx, "call.loop.end", F);

    Value *I = B.CreateAlloca(I32Ty);
    B.CreateStore(ConstantInt::get(I32Ty, 0), I);
    B.CreateBr(LoopHdr);

    B.SetInsertPoint(LoopHdr);
    Value *Idx = B.CreateLoad(I32Ty, I);
    Value *Cond = B.CreateICmpULT(Idx, DynN);
    B.CreateCondBr(Cond, LoopBody, LoopEnd);

    B.SetInsertPoint(LoopBody);
    Value *Off = readEncU32(B, CodePtr, EndPtr, KeyState);
    // Get pointer to data segment offset
    Value *OffPlus = B.CreateAdd(Off, ConstantInt::get(I32Ty, 1));
    ensureData(B, DsBuf, DsSz, DsCap, OffPlus);
    Value *Buf = B.CreateLoad(PtrTy, DsBuf);
    Value *ArgPtr = B.CreateGEP(I8Ty, Buf, B.CreateZExt(Off, I64Ty));

    // Store to args[i]
    Value *Idx64 = B.CreateZExt(Idx, I64Ty);
    Value *ArgsSlot = B.CreateGEP(PtrTy, Args, Idx64);
    B.CreateStore(ArgPtr, ArgsSlot);

    Value *IdxInc = B.CreateAdd(Idx, ConstantInt::get(I32Ty, 1));
    B.CreateStore(IdxInc, I);
    B.CreateBr(LoopHdr);

    B.SetInsertPoint(LoopEnd);
    // Call handle_call (expects I64 for num parameter)
    Value *DynN64 = B.CreateZExt(DynN, I64Ty);
    Value *Result = B.CreateCall(getHandleCall(), {Id, Args, DynN64});

    // Free args
    B.CreateCall(getFree(), {Args});

    // Store result if dst != 0xFFFFFFFF
    Value *HasDst = B.CreateICmpNE(Dst, ConstantInt::get(I32Ty, 0xFFFFFFFF));
    BasicBlock *StoreBB = BasicBlock::Create(Ctx, "call.store", F);
    BasicBlock *ContBB = BasicBlock::Create(Ctx, "call.cont", F);
    B.CreateCondBr(HasDst, StoreBB, ContBB);

    B.SetInsertPoint(StoreBB);
    dsStoreU64(B, DsBuf, DsSz, DsCap, Dst, Result);
    B.CreateBr(ContBB);

    B.SetInsertPoint(ContBB);
    B.CreateBr(LoopNext);
  }

  // OP_RET handler
  void generateOpRet(llvm::Function *F, llvm::SwitchInst *Sw,
                     llvm::Value *CodePtr, llvm::Value *EndPtr,
                     llvm::Value *KeyState,
                     llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                     llvm::Value *RetVal,
                     llvm::BasicBlock *ExitBlock) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.ret", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_RET)), BB);

    IRBuilder<> B(BB);
    EncodedValue V = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);
    B.CreateStore(V.ValueU64, RetVal);
    B.CreateBr(ExitBlock);
  }

  // OP_SELECT handler
  void generateOpSelect(llvm::Function *F, llvm::SwitchInst *Sw,
                        llvm::Value *CodePtr, llvm::Value *EndPtr,
                        llvm::Value *KeyState,
                        llvm::Value *DsBuf, llvm::Value *DsSz, llvm::Value *DsCap,
                        llvm::BasicBlock *LoopNext) {
    using namespace llvm;

    BasicBlock *BB = BasicBlock::Create(Ctx, "op.select", F);
    Sw->addCase(cast<ConstantInt>(ConstantInt::get(I8Ty, OP_SELECT)), BB);

    IRBuilder<> B(BB);
    Value *Tk = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *Bits = readEncU8(B, CodePtr, EndPtr, KeyState);
    Value *Dst = readEncU32(B, CodePtr, EndPtr, KeyState);
    EncodedValue C = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);
    EncodedValue T = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);
    EncodedValue FV = readValue(B, CodePtr, EndPtr, KeyState, DsBuf, DsSz, DsCap);

    Value *Cond = B.CreateICmpNE(C.ValueU64, ConstantInt::get(I64Ty, 0));
    Value *Result = B.CreateSelect(Cond, T.ValueU64, FV.ValueU64);

    dsStoreU64(B, DsBuf, DsSz, DsCap, Dst, Result);
    B.CreateBr(LoopNext);
  }
};

} // namespace svm
