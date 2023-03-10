/*
    LLVM ConstantEncryption Pass
    Copyright (C) 2017 Zhang(http://mayuyu.io)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published
    by the Free Software Foundation, either version 3 of the License, or
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "llvm/Transforms/Obfuscation/ConstantEncryption.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/SubstituteImpl.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

static cl::opt<bool>
    SubstituteXor("constenc_subxor",
                  cl::desc("Substitute xor operator of ConstantEncryption"),
                  cl::value_desc("Substitute xor operator"), cl::init(false),
                  cl::Optional);
static cl::opt<bool>
    ConstToGV("constenc_togv",
              cl::desc("Replace ConstantInt with GlobalVariable"),
              cl::value_desc("ConstantInt to GlobalVariable"), cl::init(false),
              cl::Optional);
static cl::opt<unsigned int>
    ObfProbRate("constenc_prob",
                cl::desc("Choose the probability [%] each instructions will be "
                         "obfuscated by the ConstantEncryption pass"),
                cl::value_desc("probability rate"), cl::init(50), cl::Optional);
static cl::opt<int> ObfTimes(
    "constenc_times",
    cl::desc(
        "Choose how many time the ConstantEncryption pass loop on a function"),
    cl::value_desc("Number of Times"), cl::init(1), cl::Optional);

namespace llvm {
struct ConstantEncryption : public ModulePass {
  static char ID;
  bool flag;
  std::vector<BinaryOperator *> obfedbos;
  ConstantEncryption(bool flag) : ModulePass(ID) { this->flag = flag; }
  ConstantEncryption() : ModulePass(ID) { this->flag = true; }
  bool shouldEncryptConstant(Instruction *I) {
    if (isa<IntrinsicInst>(I) || isa<GetElementPtrInst>(I) || isa<PHINode>(I) ||
        I->isAtomic())
      return false;
    if (!(cryptoutils->get_range(100) <= ObfProbRate))
      return false;
    return true;
  }
  bool runOnModule(Module &M) override {
    if (ObfProbRate > 100) {
      errs() << "ConstantEncryption application instruction percentage "
                "-constenc_prob=x must be 0 < x <= 100";
      return false;
    }
    const DataLayout &DL = M.getDataLayout();
    for (Function &F : M)
      if (toObfuscate(flag, &F, "constenc") && !F.isPresplitCoroutine()) {
        errs() << "Running ConstantEncryption On " << F.getName() << "\n";
        int times = ObfTimes;
        while (times) {
          for (Instruction &I : instructions(F)) {
            if (!shouldEncryptConstant(&I))
              continue;
            for (unsigned i = 0; i < I.getNumOperands(); i++) {
              if (isa<SwitchInst>(&I) && i != 0)
                break;
              Value *Op = I.getOperand(i);
              if (isa<ConstantInt>(Op))
                HandleConstantIntOperand(&I, i);
              if (GlobalVariable *G =
                      dyn_cast<GlobalVariable>(Op->stripPointerCasts()))
                if (G->hasInitializer() &&
                    (G->hasPrivateLinkage() || G->hasInternalLinkage()) &&
                    isa<ConstantInt>(G->getInitializer()))
                  HandleConstantIntInitializerGV(G);
            }
          }
          if (ConstToGV) {
            std::vector<Instruction *> ins;
            for (Instruction &I : instructions(F)) {
              if (!shouldEncryptConstant(&I))
                continue;
              for (unsigned int i = 0; i < I.getNumOperands(); i++)
                if (ConstantInt *CI = dyn_cast<ConstantInt>(I.getOperand(i))) {
                  if (isa<SwitchInst>(&I) && i != 0)
                    break;
                  GlobalVariable *GV = new GlobalVariable(
                      M, CI->getType(), false,
                      GlobalValue::LinkageTypes::PrivateLinkage,
                      ConstantInt::get(CI->getType(), CI->getValue()),
                      "ConstantEncryptionConstToGlobal");
                  appendToCompilerUsed(M, GV);
                  I.setOperand(i, new LoadInst(GV->getValueType(), GV, "", &I));
                }
              ins.emplace_back(&I);
            }
            for (Instruction *I : ins) {
              if (BinaryOperator *BO = dyn_cast<BinaryOperator>(I)) {
                if (!BO->getType()->isIntegerTy())
                  continue;
                if (std::find(obfedbos.begin(), obfedbos.end(), BO) !=
                    obfedbos.end())
                  continue;
                IntegerType *IT = cast<IntegerType>(BO->getType());
                uint64_t dummy = 0;
                if (IT->getBitWidth() == 8)
                  dummy = cryptoutils->get_uint8_t();
                else if (IT->getBitWidth() == 16)
                  dummy = cryptoutils->get_uint16_t();
                else if (IT->getBitWidth() == 32)
                  dummy = cryptoutils->get_uint32_t();
                else if (IT->getBitWidth() == 64)
                  dummy = cryptoutils->get_uint64_t();
                else if (IT->getBitWidth() != 0)
                  continue;
                GlobalVariable *GV = new GlobalVariable(
                    M, BO->getType(), false,
                    GlobalValue::LinkageTypes::PrivateLinkage,
                    ConstantInt::get(BO->getType(), dummy),
                    "ConstantEncryptionBOStore");
                StoreInst *SI = new StoreInst(
                    BO, GV, false, DL.getABITypeAlign(BO->getType()));
                SI->insertAfter(BO);
                LoadInst *LI = new LoadInst(GV->getValueType(), GV, "", false,
                                            DL.getABITypeAlign(BO->getType()));
                LI->insertAfter(SI);
                BO->replaceUsesWithIf(
                    LI, [SI](Use &U) { return U.getUser() != SI; });
                obfedbos.emplace_back(BO);
              }
            }
          }
          times--;
        }
      }
    return true;
  }

  void HandleConstantIntInitializerGV(GlobalVariable *GVPtr) {
    // Prepare Types and Keys
    ConstantInt *CI = dyn_cast<ConstantInt>(GVPtr->getInitializer());
    std::pair<ConstantInt * /*key*/, ConstantInt * /*new*/> keyandnew =
        PairConstantInt(CI);
    ConstantInt *XORKey = keyandnew.first;
    ConstantInt *newGVInit = keyandnew.second;
    if (!XORKey || !newGVInit)
      return;
    GVPtr->setInitializer(newGVInit);
    for (User *U : GVPtr->users()) {
      BinaryOperator *XORInst = nullptr;
      if (LoadInst *LI = dyn_cast<LoadInst>(U)) {
        XORInst = BinaryOperator::Create(Instruction::Xor, LI, XORKey);
        XORInst->insertAfter(LI);
        LI->replaceUsesWithIf(
            XORInst, [XORInst](Use &U) { return U.getUser() != XORInst; });
      } else if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
        XORInst = BinaryOperator::Create(Instruction::Xor, SI->getOperand(0),
                                         XORKey, "", SI);
        SI->getOperand(0)->replaceUsesWithIf(
            XORInst, [XORInst](Use &U) { return U.getUser() != XORInst; });
      }
      if (XORInst && SubstituteXor)
        SubstituteImpl::substituteXor(XORInst);
    }
  }

  void HandleConstantIntOperand(Instruction *I, unsigned opindex) {
    std::pair<ConstantInt * /*key*/, ConstantInt * /*new*/> keyandnew =
        PairConstantInt(cast<ConstantInt>(I->getOperand(opindex)));
    ConstantInt *Key = keyandnew.first;
    ConstantInt *New = keyandnew.second;
    if (!Key || !New)
      return;
    BinaryOperator *NewOperand =
        BinaryOperator::Create(Instruction::Xor, New, Key, "", I);
    I->setOperand(opindex, NewOperand);
    if (SubstituteXor)
      SubstituteImpl::substituteXor(NewOperand);
  }

  std::pair<ConstantInt * /*key*/, ConstantInt * /*new*/>
  PairConstantInt(ConstantInt *C) {
    IntegerType *IT = cast<IntegerType>(C->getType());
    uint64_t K;
    if (IT->getBitWidth() == 1 || IT->getBitWidth() == 8)
      K = cryptoutils->get_uint8_t();
    else if (IT->getBitWidth() == 16)
      K = cryptoutils->get_uint16_t();
    else if (IT->getBitWidth() == 32)
      K = cryptoutils->get_uint32_t();
    else if (IT->getBitWidth() == 64)
      K = cryptoutils->get_uint64_t();
    else
      return std::make_pair(nullptr, nullptr);
    ConstantInt *CI =
        cast<ConstantInt>(ConstantInt::get(IT, K ^ C->getValue()));
    return std::make_pair(ConstantInt::get(IT, K), CI);
  }
};

ModulePass *createConstantEncryptionPass(bool flag) {
  return new ConstantEncryption(flag);
}
} // namespace llvm
char ConstantEncryption::ID = 0;
INITIALIZE_PASS(ConstantEncryption, "constenc",
                "Enable ConstantInt GV Encryption.", true, true)