#include "BogusControlFlow.h"
#include "CombineFunctions.h"
#include "ConstObfuscation.h"
#include "DataObfuscation.h"
#include "EasyCFG.hpp"
#include "Flattening.h"
#include "FlatteningEnhanced.h"
#include "IndirectCall.h"
#include "IndirectGlobalVars.h"
#include "Linearize.h"
#include "LinearMBA.h"
#include "MBAObfuscation.h"
#include "SmallVmpPass.h"
#include "SplitBasicBlock.h"
#include "StringObfuscation.h"
#include "Substitution.h"
#include "VMFlatten.h"
#include "VariableRotation.h"
#include "xVMProtect.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

#include <AliasAccess.h>
#include <GlobalsEncryption.h>
#include <IndirectBranch.h>
#include <CustomCC.h>
#include <AntiIDA.h>
#include <CodePicPass.h>
#include <VMObfuscatorPass.h>

using namespace llvm;

llvm::PassPluginLibraryInfo getObfuscationPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION, "Obfuscation", LLVM_VERSION_STRING,
      [](PassBuilder &PB) {
        // PORT-OBF: the source fork accepted ANY pipeline name here and
        // returned true, which hijacks every named-pipeline resolution.
        // NeverC's linker parses named pipelines (parsePassPipeline in
        // CommonLTOConfig), so the callbacks now only claim "code-pic" and
        // leave all other names to the native registry. CodePicPass remains
        // available through the PipelineStart EP registration (-mllvm -x-pic).
        PB.registerPipelineParsingCallback([](StringRef Name, ModulePassManager &MPM, ArrayRef<PassBuilder::PipelineElement>) {
          if (Name != "code-pic")
            return false;
          MPM.addPass(CodePicPass());
          return true;
        });
        PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM, ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name != "code-pic")
                    return false;
                  FPM.addPass(CodePicPass());
                  return true;
                });

        PB.registerPipelineStartEPCallback([](llvm::ModulePassManager &MPM,
                                              OptimizationLevel Level) {
          MPM.addPass(SmallVmpPass());
          MPM.addPass(VmObfuscatorPass());
          MPM.addPass(createModuleToFunctionPassAdaptor(SplitBasicBlockPass()));
          MPM.addPass(
              createModuleToFunctionPassAdaptor(BogusControlFlowPass()));
          MPM.addPass(createModuleToFunctionPassAdaptor(SubstitutionPass()));
          MPM.addPass(createModuleToFunctionPassAdaptor(MBAObfuscationPass()));
          MPM.addPass(createModuleToFunctionPassAdaptor(LinearMBAPass()));
          MPM.addPass(createModuleToFunctionPassAdaptor(FlatteningPass()));
          MPM.addPass(CodePicPass());
          MPM.addPass(createModuleToFunctionPassAdaptor(CodePicPass()));

        });
        PB.registerOptimizerEarlyEPCallback([](llvm::ModulePassManager &MPM,
                                               OptimizationLevel Level) {
          MPM.addPass(StringObfuscationPass());
          MPM.addPass(createModuleToFunctionPassAdaptor(DataObfuscationPass()));
          MPM.addPass(xvmPass());
          MPM.addPass(CombineFunctionsPass());
          MPM.addPass(createModuleToFunctionPassAdaptor(FlatteningEnhanced()));
          MPM.addPass(VariableRotationPass());
          MPM.addPass(AliasAccess());
          MPM.addPass(GlobalsEncryption());
          MPM.addPass(CustomCC());
        });

        PB.registerOptimizerLastEPCallback([](llvm::ModulePassManager &MPM,
                                              OptimizationLevel Level) {


          MPM.addPass(createModuleToFunctionPassAdaptor(ConstObfuscationPass()));
          MPM.addPass(createModuleToFunctionPassAdaptor(IndirectCallPass()));
          MPM.addPass(IngvObfuscationPass());
          MPM.addPass(createModuleToFunctionPassAdaptor(VmFlatObfuscationPass()));
          MPM.addPass(Linearize());
          MPM.addPass(EasyCfgPass());
          MPM.addPass(createModuleToFunctionPassAdaptor(IndirectBranch()));
          MPM.addPass(AntiIDAPass());
          
        });
        //PB.registerVectorizerStartEPCallback(
        //    [](FunctionPassManager &FPM, OptimizationLevel Level) {});
      }};
}

#ifndef LLVM_OBFUSCATION_LINK_INTO_TOOLS
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getObfuscationPluginInfo();
}
#endif
