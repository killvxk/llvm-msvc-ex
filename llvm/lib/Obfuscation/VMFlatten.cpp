#include "VMFlatten.h"

#include "BogusControlFlow.h"
#include "ConstObfuscation.h"
#include "CryptoUtils.h"
#include "Flattening.h"
#include "IndirectGlobalVars.h"
#include "Utils.h"

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


// #define RUN_BLOCK 1
// #define JMP_BORING 2
// #define JMP_SELECT 3

using namespace llvm;
static cl::opt<bool>
    RunVmFlatObfuscationPass("vm-fla", cl::init(false),
                             cl::desc("OLLVM - VmFlattenObfuscationPass"));


/*
static cl::opt<bool>
    RunVmFlatObfuscationPassEnc("vm-fla-enc", cl::init(false),
                             cl::desc("OLLVM - VmFlattenObfuscationPassEnc"));

static cl::opt<bool>
    RunVmFlatObfuscationPassSym("vm-fla-sym", cl::init(false),
                             cl::desc("OLLVM - VmFlattenObfuscationPass Anti Sym and Taint"));

static cl::opt<bool>
    RunVmFlatObfuscationPassLight("vm-fla-light", cl::init(false),
                             cl::desc("OLLVM - VmFlattenObfuscationPass Light"));
*/

static cl::opt<int> VmObfuProbRate(
    "vm-prob", cl::init(100),
    cl::desc("Choose the probability <vm-prob> for each basic blocks will "
             "be obfuscated by VmFlattenObfuscationPass"));

static cl::opt<int> VmObfuscationLevel(
    "vm-fla-level", cl::init(7),
    cl::desc("OLLVM - VmFlattenObfuscationPass Level"));

// static cl::opt<bool> VmFlatObfusWin32(
//       "use-vm32", cl::init(false),
//       cl::desc("OLLVM - VmFlattenObfuscationPass Win32"));


namespace  llvm {
    int get_vm_fla_level() {
      return VmObfuscationLevel;
    }
}
//0 Only-vm-fla
//1 vm-fla with enc const
//2 vm-fla with enc const with global ind
//3 vm-fla with enc const with global ind with antitaint
//4 vm-fla with enc const with global ind with antitaint and sym
//5 vm-fla with enc const with global ind with antitaint and sym with fla for some
//6 vm-fla with enc const with global ind with antitaint and sym with fla for some with bcf
//7 vm-fla with enc const with global ind with antitaint and sym with fla for some with bcf for all 

namespace {
struct VMFlat {
  struct Node {
    unsigned int value;
    Node *bb1, *bb2;
    BasicBlock *data;
  };
  struct VMInst {
    unsigned int type;
    unsigned int op1, op2;
  };
  unsigned int VM_LEVEL;
  unsigned int RUN_BLOCK;
  unsigned int JMP_BORING;
  unsigned int JMP_SELECT;
  std::string new_name_pre;
  static std::vector<BasicBlock *> *get_blocks(Function *function,
                                              std::vector<BasicBlock *> *lists);

  static unsigned int get_unique_number(const std::vector<unsigned int> *rand_list);

  static Node *new_node(unsigned int value);

  //BasicBlock* handle_first_basic_block(Function* f, std::vector<BasicBlock*>& orig_bb) const;

  static VMInst *new_inst(unsigned int type, unsigned int op1, unsigned int op2);

  void create_node_inst(std::vector<VMInst *> *all_inst,
                        std::map<Node *, unsigned int> *inst_map, Node *node) const;

  void gen_inst(std::vector<VMInst *> *all_inst,
                std::map<Node *, unsigned int> *inst_map, const Node *node);

  static Node *find_bb_node(const BasicBlock *bb, const std::vector<Node *> *all_node);

  [[maybe_unused]] void dump_inst(const std::vector<VMInst *> *all_inst) const;
  bool DoFlatten(Function *f);
  void insertMemoryAttackTaint(Function &F);
  void insertSymbolicMemorySnippet(Function &F);
  void hex2i64(uint8_t *hex, uint32_t size, uint64_t *i64_arr);
  void hex2i32(uint8_t *hex, uint32_t size, uint32_t *i32_arr);
  bool runVmFlaOnFunction(Function &function);
  bool isFunction32Bit(llvm::Function *F) {
    llvm::Module *M = F->getParent();
    if (!M) return false;
    
    const llvm::DataLayout &DL = M->getDataLayout();
    return DL.getPointerSize() == 4; // 32位系统的指针大小为4字节
  }  
};

std::vector<BasicBlock *> *VMFlat::get_blocks(Function *function,
                                             std::vector<BasicBlock *> *lists) {
  lists->clear();
  for (BasicBlock &basicBlock : *function)
    lists->push_back(&basicBlock);
  return lists;
}

unsigned VMFlat::get_unique_number(const std::vector<unsigned> *rand_list) {
  unsigned int num = cryptoutils->get_uint32_t();
  while (true) {
    bool state = true;
    for (const unsigned int &n : *rand_list)
      if (n == num) {
        state = false;
        break;
      }
    if (state)
      break;
    num = cryptoutils->get_uint32_t();
  }
  return num;
}

VMFlat::Node *VMFlat::new_node(const unsigned value) {
  const auto node = static_cast<Node *>(malloc(sizeof(Node)));
  node->value = value;
  node->bb1 = node->bb2 = nullptr;
  return node;
}

//BasicBlock * VMFlat::handle_first_basic_block(Function *f,
//    std::vector<BasicBlock *> &orig_bb) const {
//   BasicBlock* old_entry = &*f->begin();
//    orig_bb.erase(orig_bb.begin());
//
//    // If the terminator of the old entry is a conditional branch or has more than two successors,
//    // split the old entry block just before terminator to create a new basic block.
//   const auto old_entry_terminator = old_entry->getTerminator();
//    if (isa<BranchInst>(old_entry_terminator) && cast<BranchInst>(old_entry_terminator)->isConditional() ||
//        old_entry_terminator->getNumSuccessors() > 2) {
//        auto split_point = old_entry->end();
//        if (old_entry->size() > 1) {
//            --split_point;
//        }
//        BasicBlock* split_block = old_entry->splitBasicBlock(split_point, new_name_pre + "FirstBB");
//        orig_bb.insert(orig_bb.begin(), split_block);
//        return split_block;
//    }
//
//    // If the terminator is not a conditional branch and has at most two successors,
//    // return the first successor as the first block.
//    return old_entry_terminator->getSuccessor(0);
//}

VMFlat::VMInst *VMFlat::new_inst(unsigned type, unsigned op1, unsigned op2) {
  const auto code = static_cast<VMInst *>(malloc(sizeof(VMInst)));
  code->type = type;
  code->op1 = op1;
  code->op2 = op2;
  return code;
}

void VMFlat::create_node_inst(std::vector<VMInst *> *all_inst,
                              std::map<Node *, unsigned> *inst_map,
                              Node *node) const {
  VMInst *code = new_inst(RUN_BLOCK, node->value, 0);
  all_inst->push_back(code);
  inst_map->insert(
      std::map<Node *, unsigned int>::value_type(node, all_inst->size() - 1));
}

void VMFlat::gen_inst(std::vector<VMInst *> *all_inst,
                      std::map<Node *, unsigned> *inst_map, const Node *node) {
  // assert(!(node->bb1==NULL && node->bb2!=NULL));
  if (node->bb1 != nullptr && node->bb2 == nullptr) {
    if (inst_map->count(node->bb1) == 0) {
      create_node_inst(all_inst, inst_map, node->bb1);
      gen_inst(all_inst, inst_map, node->bb1);
    } else {
      const unsigned int addr = inst_map->find(node->bb1)->second * 3;
      VMInst *code = new_inst(JMP_BORING, addr, 0);
      all_inst->push_back(code);
    }
  } else if (node->bb2 != nullptr) {
    VMInst *code = new_inst(JMP_SELECT, 0, 0);
    all_inst->push_back(code);
    if (inst_map->count(node->bb1) == 0) {
      create_node_inst(all_inst, inst_map, node->bb1);
      gen_inst(all_inst, inst_map, node->bb1);
    }
    if (inst_map->count(node->bb2) == 0) {
      create_node_inst(all_inst, inst_map, node->bb2);
      gen_inst(all_inst, inst_map, node->bb2);
    }
    code->op1 = (*inst_map->find(node->bb1)).second * 3;
    code->op2 = (*inst_map->find(node->bb2)).second * 3;
  } else
    return;
}

VMFlat::Node *VMFlat::find_bb_node(const BasicBlock *bb,
                                 const std::vector<Node *> *all_node) {
  for (const auto &i : *all_node) {
    if (bb == i->data)
      return i;
  }
  return nullptr;
}

void VMFlat::dump_inst(const std::vector<VMInst *> *all_inst) const {
  unsigned int x = 0;
  for (const auto c : *all_inst) {
    printf("0x%02x: ", x++);
    if (c->type == RUN_BLOCK)
      printf("RUN_BLOCK 0x%02x\n", c->op1);
    if (c->type == JMP_BORING)
      printf("JMP_BORING 0x%02x\n", c->op1);
    if (c->type == JMP_SELECT)
      printf("JMP_SELECT 0x%02x 0x%02x\n", c->op1, c->op2);
  }
}

bool VMFlat::DoFlatten(Function *f) {

  //errs()<<"Function Name = "<<f->getName()<<"\r\n";
  if (f->isDeclaration() || f->hasAvailableExternallyLinkage()){
    return false;
  }

  if(f->getName().starts_with("??") || f->getName().contains("std@")) {
    if(VM_LEVEL<5)
      return false;
    if(VM_LEVEL<6)
      return ollvm::flatten(*f);
    ollvm::bogus(*f);
    ollvm::doF(*f->getParent(),*f);
    return ollvm::flatten(*f);
  }

  if(isMemberFunction(f)||
      f->hasCXXEH() || f->hasCXXSEH() )
  {
    //errs()<<"FLA-Function Name = "<<f->getName()<<"\r\n";
    if(VM_LEVEL<=4)
      return false;
    if(VM_LEVEL<6)
      return ollvm::flatten(*f);
    ollvm::bogus(*f);
    ollvm::doF(*f->getParent(),*f);
    return ollvm::flatten(*f);
  }

  //errs()<<f->getName()<<"\r\n";

  if(f->getName().starts_with("genrand."))
  {
    if(VM_LEVEL==7)
      return true;
  }


  RUN_BLOCK = cryptoutils->get_uint32_t();
  JMP_BORING = RUN_BLOCK + 1;
  JMP_SELECT = RUN_BLOCK + 2;

  std::vector<BasicBlock *> orig_bb;
  get_blocks(f, &orig_bb);
  if (orig_bb.size() <= 1) {
    if(VM_LEVEL==7){
      ollvm::bogus(*f);
      ollvm::doF(*f->getParent(),*f);
      return DoFlatten(f);
    }
    return false;
  }



  // errs() << "Count 1 = " << orig_bb.size() << "\r\n";
  //   unsigned int rand_val = seed;
  #if 1
  //First
  auto tmp = f->begin();
  BasicBlock *old_entry = &*tmp;
  orig_bb.erase(orig_bb.begin());
  BranchInst *first_br = nullptr;
  if (isa<BranchInst>(old_entry->getTerminator())) {
    first_br = cast<BranchInst>(old_entry->getTerminator());
  }
  BasicBlock *firstbb = old_entry->getTerminator()->getSuccessor(0);
  if ((first_br != nullptr && first_br->isConditional()) ||
      old_entry->getTerminator()->getNumSuccessors() >
          2) { // Split the first basic block
    auto iter = old_entry->end();
    --iter;
    if (old_entry->size() > 1) {
      --iter;
    }
    BasicBlock *splited =
        old_entry->splitBasicBlock(iter, Twine(new_name_pre + "FirstBB"));
    firstbb = splited;
    orig_bb.insert(orig_bb.begin(), splited);
  }
  #endif

  std::vector<Node *> all_node;
  // unsigned int val=0;
  std::vector<unsigned int> rand_list;
  for (auto &i : orig_bb) {
    unsigned int num = get_unique_number(&rand_list);
    rand_list.push_back(num);
    Node *tmp1 = new_node(num);
    all_node.push_back(tmp1);
    tmp1->data = i;
  }

  // errs() << "Count = " << all_node.size() << "\r\n";

  for (auto i = all_node.begin(); i != all_node.end(); ++i) {
    Node *tmp = *i;
    BasicBlock *bb = tmp->data;
    if (bb->getTerminator()->getNumSuccessors() == 2) {
      BasicBlock *bb1 = bb->getTerminator()->getSuccessor(0),
                 *bb2 = bb->getTerminator()->getSuccessor(1);
      Node *n1 = find_bb_node(bb1, &all_node), *n2 = find_bb_node(bb2, &all_node);
      tmp->bb1 = n1;
      tmp->bb2 = n2;
    } else if (bb->getTerminator()->getNumSuccessors() == 1) {
      BasicBlock *bb1 = bb->getTerminator()->getSuccessor(0);
      Node *n = find_bb_node(bb1, &all_node);
      tmp->bb1 = n;
    } else {
      continue;
    }
    // for(std::vector<Node*>::iterator
    // j=all_node.begin();j!=all_node.end();j++)
  }


  Node *start = find_bb_node(firstbb, &all_node);
  Node *fake = new_node(0x7FFFFFFF);
  std::vector<VMInst *> all_inst;
  std::map<Node *, unsigned int> inst_map;
  fake->bb1 = start;
  // errs() << "begin gen ins\r\n";
  gen_inst(&all_inst, &inst_map, fake);
  // errs() << "end gen ins\r\n";
  // dump_inst(&all_inst);
  std::vector<Constant *> opcodes;
  auto type_int_ty =Type::getInt64Ty(f->getContext());
  [[maybe_unused]] auto type_int32_ty = Type::getInt32Ty(f->getContext());
  if (isFunction32Bit(f))
    type_int_ty = type_int32_ty;
  
  for (auto inst : all_inst) {
    opcodes.push_back(ConstantInt::get(type_int_ty, inst->type));
    opcodes.push_back(ConstantInt::get(type_int_ty, inst->op1));
    opcodes.push_back(ConstantInt::get(type_int_ty, inst->op2));
  }
  // errs() << "inst ok\r\n";

  ArrayType *at = ArrayType::get(type_int_ty, opcodes.size());
  Constant *opcode_array =
      ConstantArray::get(at, ArrayRef<Constant *>(opcodes));
  auto oparr_var = new GlobalVariable(*(f->getParent()), at, false,
                                      GlobalValue::LinkageTypes::PrivateLinkage,
                                      opcode_array, new_name_pre + "opcodes");
  // ȥ����һ��������ĩβ����ת
  old_entry->getTerminator()->eraseFromParent();
  auto vm_pc =
      new AllocaInst(type_int_ty, 0, Twine(new_name_pre + "VMpc"), old_entry);
  ConstantInt *init_pc = ConstantInt::get(type_int_ty, 0);
  new StoreInst(init_pc, vm_pc, old_entry);
  auto vm_flag = new AllocaInst(type_int_ty, 0,
                                Twine(new_name_pre + "VMJmpFlag"), old_entry);
  BasicBlock *vm_entry = BasicBlock::Create(
      f->getContext(), Twine(new_name_pre + "VMEntry"), f, firstbb);

  BranchInst::Create(vm_entry, old_entry);
  IRBuilder<> IRB(vm_entry);
  Value *zero = ConstantInt::get(type_int_ty, 0);

  Value *op1_offset =
      IRB.CreateAdd(IRB.CreateLoad(vm_pc->getAllocatedType(), vm_pc),
                    ConstantInt::get(type_int_ty, 1));
  Value *op2_offset =
      IRB.CreateAdd(IRB.CreateLoad(vm_pc->getAllocatedType(), vm_pc),
                    ConstantInt::get(type_int_ty, 2));

  // FIX(opaque-ptr): the opcode array holds integer VM opcodes ([N x i64/i32]).
  // Loading with gep->getType() under opaque pointers loads a *pointer*
  // (`load ptr`), which then feeds SwitchInst as a pointer condition —
  // invalid IR that bitcode readers reject ("Invalid record") and the
  // textual parser refuses ("switch condition must have integer type").
  // Latent since opaque pointers became mandatory; first exposed by the
  // NeverC port's embedded-bitcode link path. Load the array element type
  // instead, matching the switch-case constants below.
  Type *op_elem_ty = oparr_var->getValueType()->getArrayElementType();
  auto optype_gep =
      IRB.CreateGEP(oparr_var->getValueType(), oparr_var,
                    {zero, IRB.CreateLoad(vm_pc->getAllocatedType(), vm_pc)});
  Value *optype = IRB.CreateLoad(op_elem_ty, optype_gep);
  auto op1_gep =
      IRB.CreateGEP(oparr_var->getValueType(), oparr_var, {zero, op1_offset});
  Value *op1 = IRB.CreateLoad(op_elem_ty, op1_gep);
  auto op2_gep =
      IRB.CreateGEP(oparr_var->getValueType(), oparr_var, {zero, op2_offset});
  Value *op2 = IRB.CreateLoad(op_elem_ty, op2_gep);

  IRB.CreateStore(
      IRB.CreateAdd(IRB.CreateLoad(vm_pc->getAllocatedType(), vm_pc),
                    ConstantInt::get(type_int_ty, 3)),
      vm_pc);
  BasicBlock *run_block = BasicBlock::Create(
      f->getContext(), new_name_pre + "RunBlock", f, firstbb);
  BasicBlock *jmp_boring = BasicBlock::Create(
      f->getContext(), new_name_pre + "JmpBoring", f, firstbb);
  BasicBlock *jmp_select = BasicBlock::Create(
      f->getContext(), new_name_pre + "JmpSelect", f, firstbb);
  BasicBlock *defaultCase =
      BasicBlock::Create(f->getContext(), new_name_pre + "Default", f, firstbb);
  BranchInst::Create(vm_entry, defaultCase);
  SwitchInst *switch1 = IRB.CreateSwitch(optype, defaultCase, 0);
  switch1->addCase(ConstantInt::get(type_int_ty, RUN_BLOCK), run_block);
  switch1->addCase(ConstantInt::get(type_int_ty, JMP_BORING), jmp_boring);
  switch1->addCase(ConstantInt::get(type_int_ty, JMP_SELECT), jmp_select);

  // create run_block's basicblock
  // the first choice
  IRB.SetInsertPoint(run_block);
  // the seconde choice
  SwitchInst *switch2 = IRB.CreateSwitch(op1, defaultCase, 0);
  // errs() << "the seconde choice start\r\n";
  for (std::vector<BasicBlock *>::iterator b = orig_bb.begin();
       b != orig_bb.end(); b++) {
    BasicBlock *block = *b;
    block->moveBefore(defaultCase);
    Node *t = find_bb_node(block, &all_node);
    // ConstantInt *numCase =
    // cast<ConstantInt>(ConstantInt::get(switch2->getCondition()->getType(),
    // t->value));
    switch2->addCase(ConstantInt::get(type_int_ty, t->value), block);
  }
  for (auto block : orig_bb) { // Handle successors
    if (block->getTerminator()->getNumSuccessors() == 1) {
      //  ////errs() << "\033[1;32mThis block has 1 successor\033[0m\n";
      [[maybe_unused]] BasicBlock *succ =
          block->getTerminator()->getSuccessor(0);
      block->getTerminator()->eraseFromParent();
      BranchInst::Create(defaultCase, block);
    } else if (block->getTerminator()->getNumSuccessors() == 2) {
      // ////errs() << "\033[1;32mThis block has 2 successors\033[0m\n";
      auto old_br = cast<BranchInst>(block->getTerminator());
      SelectInst *select = SelectInst::Create(
          old_br->getCondition(), ConstantInt::get(type_int_ty, 1),
          ConstantInt::get(type_int_ty, 0), "", block->getTerminator());
      new StoreInst(select, vm_flag, block->getTerminator());
      block->getTerminator()->eraseFromParent();
      BranchInst::Create(defaultCase, block);
    } else {
      continue;
    }
  }
  // errs() << "the seconde choice end\r\n";
  IRB.SetInsertPoint(jmp_boring);
  IRB.CreateStore(op1, vm_pc);
  IRB.CreateBr(vm_entry);

  IRB.SetInsertPoint(jmp_select);
  BasicBlock *select_true = BasicBlock::Create(
      f->getContext(), new_name_pre + "JmpSelectTrue", f, firstbb);
  BasicBlock *select_false = BasicBlock::Create(
      f->getContext(), new_name_pre + "JmpSelectFalse", f, firstbb);
  IRB.CreateCondBr(
      IRB.CreateICmpEQ(IRB.CreateLoad(vm_flag->getAllocatedType(), vm_flag),
                       ConstantInt::get(type_int_ty, 1)),
      select_true, select_false);
  IRB.SetInsertPoint(select_true);
  IRB.CreateStore(op1, vm_pc);
  IRB.CreateBr(vm_entry);
  IRB.SetInsertPoint(select_false);
  IRB.CreateStore(op2, vm_pc);
  IRB.CreateBr(vm_entry);


  fixStack(*f,false);
  return true;
}

// ReSharper disable once CppMemberFunctionMayBeStatic
[[maybe_unused]] void VMFlat::insertMemoryAttackTaint(Function &F) {
  BasicBlock &entry = F.getEntryBlock();
  IRBuilder<> irb(entry.getFirstNonPHIOrDbgOrLifetime());
  ArrayType * array_table_type = ArrayType::get(irb.getInt8Ty(), 256);
  Value *table1 = irb.CreateAlloca(array_table_type);
  Value *table2 = irb.CreateAlloca(array_table_type);
  auto  ptr_array_type = ArrayType::get(irb.getInt64Ty(), 32);
  const auto is32 = isFunction32Bit(&F);
  if(is32)
    ptr_array_type = ArrayType::get(irb.getInt32Ty(), 32);

  Value *ptr1 = irb.CreateBitCast(table1, PointerType::get(ptr_array_type,
                                    dyn_cast<PointerType>(table1->getType())->getAddressSpace()));
  Value *ptr2 = irb.CreateBitCast(table2, PointerType::get(ptr_array_type,
                                    dyn_cast<PointerType>(table2->getType())->getAddressSpace()));

  uint8_t buf[256];
  for (int i = 0; i < 256; i++) {
    buf[i] = static_cast<uint8_t>(i);
  }
  const auto *p_buf = reinterpret_cast<uint64_t *>(buf);
  if (is32) {
    for (int i = 0; i < (256 / 4); i++) {

      irb.CreateStore(irb.getInt32(p_buf[i]),
                      irb.CreateConstGEP2_32(ptr_array_type, ptr1, 0, i))
          ->setVolatile(true);
      irb.CreateStore(irb.getInt32(p_buf[i]),
                      irb.CreateConstGEP2_32(ptr_array_type, ptr2, 0, i))
          ->setVolatile(true);
    }
  } else {
    for (int i = 0; i < (256 / 8); i++) {

      irb.CreateStore(irb.getInt64(p_buf[i]),
                      irb.CreateConstGEP2_64(ptr_array_type, ptr1, 0, i))
          ->setVolatile(true);
      irb.CreateStore(irb.getInt64(p_buf[i]),
                      irb.CreateConstGEP2_64(ptr_array_type, ptr2, 0, i))
          ->setVolatile(true);
    }
  }
  for (BasicBlock &BB: F) {
    if (&BB == &entry) {
      continue;
    }
    auto iter = BB.getFirstNonPHIOrDbgOrLifetime()->getIterator();
    std::vector<Value *> int_list;
    int_list.clear();
    while (iter != BB.end()) {
      //errs() << int_list.size();
      Instruction &I = *iter;
      if (!int_list.empty() && cryptoutils->get_uint32_t() % 3 == 2) {
        for (size_t i = 0; i < I.getNumOperands(); i++) {
          if (I.getOperand(i)->getType()->isIntegerTy() && !isa<Constant>(I.getOperand(i))) {
            IRBuilder<> builder(&I);
            const int r = cryptoutils->get_uint32_t() % int_list.size();
            Value *v = int_list[r];
            Value *index;
            switch (dyn_cast<IntegerType>(v->getType())->getIntegerBitWidth()) {
            case 8:
              index = builder.CreateURem(v, builder.getInt8(127));
              break;
            case 16:
              index = builder.CreateMul(v, builder.getInt16(cryptoutils->get_uint32_t() % 0xFFFF));
              index = builder.CreateURem(index, builder.getInt16(0xFF));
              break;
            case 32:
              index = builder.CreateXor(v, builder.CreateShl(v, cryptoutils->get_uint32_t() % 32));
              index = builder.CreateAnd(index, builder.getInt32(0xFF));
              break;
            case 64:
              index = builder.CreateXor(v, builder.CreateLShr(v,cryptoutils->get_uint32_t() % 64));
              index = builder.CreateAnd(index, builder.getInt64(0xFF));
              break;
            }
            Value *idx[2] = {builder.getInt32(0), index};
            const auto x1_gep = builder.CreateGEP(array_table_type,table1, idx);
            Value *x1 = builder.CreateLoad(array_table_type->getElementType(), x1_gep);
            cast<LoadInst>(x1)->setVolatile(true);
            const auto x2_gep = builder.CreateGEP(array_table_type,table2, idx);
            Value *x2 = builder.CreateLoad(array_table_type->getElementType(), x2_gep);
            cast<LoadInst>(x2)->setVolatile(true);
            Value *z = builder.CreateSub(x1, x2);
            z = builder.CreateAdd(I.getOperand(i), builder.CreateIntCast(z, I.getOperand(i)->getType(),
                                    false));
            I.setOperand(i, z);
            break;
          }
        }
      }
      for (size_t i = 0; i < I.getNumOperands(); i++) {
        if (I.getOperand(i)->getType()->isIntegerTy() && dyn_cast<IntegerType>(I.getOperand(i)->getType())->getBitWidth() >= 8
            && dyn_cast<IntegerType>(I.getOperand(i)->getType())->getBitWidth() <= 64
            && !isa<Constant>(I.getOperand(i))) {
          int_list.push_back(I.getOperand(i));
        }
      }
      iter = I.getIterator();
      ++iter;
    }
  }
}

// ReSharper disable once CppInconsistentNaming
[[maybe_unused]] void VMFlat::insertSymbolicMemorySnippet(Function &F) {
  BasicBlock &entryBB = F.getEntryBlock();
  IRBuilder<> builder(entryBB.getFirstNonPHIOrDbgOrLifetime());
  const auto array_type =ArrayType::get(Type::getInt8Ty(F.getContext()), 256);
  const auto array = builder.CreateAlloca(array_type);
  auto i64_ptr_type = PointerType::get(
        ArrayType::get(builder.getInt64Ty(), 32),
        dyn_cast<PointerType>(array->getType())->getAddressSpace());
  const auto is32 = isFunction32Bit(&F);
  //initialize the array
  //array[i] = i
  if (is32)
    i64_ptr_type = PointerType::get(
        ArrayType::get(builder.getInt32Ty(), 32),
        dyn_cast<PointerType>(array->getType())->getAddressSpace());

  Value *i64_ptr = builder.CreateBitCast(array,i64_ptr_type);



  uint8_t buf[256];
  for (int i = 0; i < 256; i++) {
    buf[i] = static_cast<uint8_t>(i);
  }
  if(is32){
    uint32_t i32_arr[256 / 4];
    hex2i32(buf, 256, i32_arr);
    for (int i = 0; i < (256 / 4); i++) {
      builder.CreateStore(
          builder.getInt32(i32_arr[i]),
          builder.CreateConstGEP2_32(ArrayType::get(builder.getInt32Ty(), 32),
          i64_ptr, 0, i),
          true);
    }
  } else {
    uint64_t i64_arr[256 / 8];
    hex2i64(buf, 256, i64_arr);
    for (int i = 0; i < (256 / 8); i++) {
      builder.CreateStore(
          builder.getInt64(i64_arr[i]),
          builder.CreateConstGEP2_64(ArrayType::get(builder.getInt64Ty(), 32),
                                     i64_ptr, 0, i),
          true);
    }
  }

  for (BasicBlock &bb: F) {
    if (&bb == &entryBB) {
      continue;
    }

    auto iter = bb.begin();
    while (iter != bb.end()) {
      Instruction &I = *iter;
      for (size_t i = 0; i < I.getNumOperands(); i++) {
        Value *v = I.getOperand(i);

        int len_of_bytes;
        if (v->getType()->isIntegerTy(32)) {
          len_of_bytes = 4;
        }
        else if (v->getType()->isIntegerTy(64)) {
          len_of_bytes = 8;
        }
        else {
          continue;
        }
        if (isa<ConstantInt>(v)) {
          continue;
        }

        IRBuilder<> builder(&I);
        Value *bytes[8] = {nullptr};
        Value *magic_ff;
        Value *magic_0;
        IntegerType *num_ty;
        if (len_of_bytes == 4) {
          magic_ff = builder.getInt32(0xFF);
          magic_0 = builder.getInt32(0);
          num_ty = builder.getInt32Ty();
        }
        else {
          magic_ff = builder.getInt64(0xFF);
          magic_0 = builder.getInt64(0);
          num_ty = builder.getInt64Ty();
        }

        bytes[0] = builder.CreateAnd(v, magic_ff);

        for (int j = 1; j < len_of_bytes; j++) {
          bytes[j] = builder.CreateAnd(builder.CreateLShr(v, j * 8), magic_ff);
        }

        //bytes[i] = array[bytes[i]]
        for (int j = 0; j < len_of_bytes; j++) {
          Value *idx[2] = {magic_0, bytes[j]};
          const auto load_value_gep =builder.CreateGEP(array_type,array, idx);
          LoadInst *load_value = builder.CreateLoad(array_type->getElementType(),load_value_gep);
          load_value->setVolatile(true);
          bytes[j] = builder.CreateZExt(load_value, num_ty);
        }

        //bytes[0] = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | bytes[3] << 24
        for (int j = 1; j < len_of_bytes; j++) {
          bytes[0] = builder.CreateOr(bytes[0], builder.CreateShl(bytes[j], j * 8));
        }
        //set the original operand as bytes[0]
        I.setOperand(i, bytes[0]);
        break;
      }
      iter = I.getIterator();
      ++iter;
    }
    ;
  }
}

void VMFlat::hex2i64(uint8_t *hex, uint32_t size, uint64_t *i64_arr) {
  for (uint32_t i = 0; i < size; i += 8) {
    i64_arr[i / 8] = *reinterpret_cast<uint64_t *>(hex + i);
  }
}

void VMFlat::hex2i32(uint8_t *hex, uint32_t size, uint32_t *i32_arr) {
  for (uint32_t i = 0; i < size; i += 4) {
    i32_arr[i / 4] = *reinterpret_cast<uint32_t *>(hex + i);
  }
}

bool VMFlat::runVmFlaOnFunction(Function &function) {

  if (!((VmObfuProbRate > 0) && (VmObfuProbRate <= 100))) {
    // errs() << "VmFlattenObfuscationPass application basic blocks percentage "
    "-vm_prob=x must be 0 < x <= 100";
    return false;
  }  
  VM_LEVEL = VmObfuscationLevel;
  if (toObfuscate(false, &function, "x-full")){
    VM_LEVEL = 7;
    //errs()<<"vm-full:"<<function.getName().str()<<"\n";
  }
  new_name_pre = std::to_string(cryptoutils->get_uint32_t());
  bool changed = false;
  if (static_cast<int32_t>(cryptoutils->get_range(100)) <= VmObfuProbRate) {
    changed = DoFlatten(&function);
  }
  if (changed)
  {
    if(VM_LEVEL>=3)
    {
      insertMemoryAttackTaint(function);
    }
    if(VM_LEVEL>=4)
    {
       insertSymbolicMemorySnippet(function);
    }
    if(VM_LEVEL>=1)
    {
      ConstEncryption str;
      str.runOnFunction(function,false);
    }
    if(VM_LEVEL>=2)
    {
      IndirectGlobalVariable gv;
      gv.runOnFunction(function);
    }
    turnOffOptimization(&function);
  }
  return true;
}
} // namespace

PreservedAnalyses VmFlatObfuscationPass::run(Function &F,
                                             FunctionAnalysisManager &AM) {

  if (toObfuscate(RunVmFlatObfuscationPass, &F, "vm-fla")) {
    LowerSwitchPass lower;
    lower.run(F, AM);
    VMFlat vm;
    if (vm.runVmFlaOnFunction(F))
      return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}