/*
 * Copyright 2011 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors: Tom Stellard <thomas.stellard@amd.com>
 *
 */


#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

#include "AMDIL.h"
#include "AMDISA.h"
#include "AMDILInstrInfo.h"

#include <vector>

using namespace llvm;

namespace {
  class AMDISALowerShaderInstructionsPass : public MachineFunctionPass {

  private:
    static char ID;
    TargetMachine &TM;
    MachineRegisterInfo * MRI;

    void lowerEXPORT_REG_FAKE(MachineInstr &MI, MachineBasicBlock &MBB,
        MachineBasicBlock::iterator I);
    void lowerLOAD_INPUT(MachineInstr & MI);
    bool lowerSTORE_OUTPUT(MachineInstr & MI, MachineBasicBlock &MBB,
        MachineBasicBlock::iterator I);
    void lowerSWIZZLE(MachineInstr &MI);

  public:
    AMDISALowerShaderInstructionsPass(TargetMachine &tm) :
      MachineFunctionPass(ID), TM(tm) { }

      bool runOnMachineFunction(MachineFunction &MF);

      const char *getPassName() const { return "AMDISA Lower Shader Instructions"; }
    };
} /* End anonymous namespace */

char AMDISALowerShaderInstructionsPass::ID = 0;

FunctionPass *llvm::createAMDISALowerShaderInstructionsPass(TargetMachine &tm) {
    return new AMDISALowerShaderInstructionsPass(tm);
}

#define INSTR_CASE_FLOAT_V(inst) \
  case AMDIL:: inst##_v4f32: \

#define INSTR_CASE_FLOAT_S(inst) \
  case AMDIL:: inst##_f32:

#define INSTR_CASE_FLOAT(inst) \
  INSTR_CASE_FLOAT_V(inst) \
  INSTR_CASE_FLOAT_S(inst)
bool AMDISALowerShaderInstructionsPass::runOnMachineFunction(MachineFunction &MF)
{
  MRI = &MF.getRegInfo();
  std::vector<MachineInstr *> storeOutputInstrs;
  std::vector<MachineInstr *> exportRegInstrs;

   /* Move EXPORT_REG, STORE_OUTPUT to the end; Move RESERVE_REG and LOAD_INPUT to the front*/
  for (MachineFunction::iterator BB = MF.begin(), BB_E = MF.end();
                                                  BB != BB_E; ++BB) {
    MachineBasicBlock &MBB = *BB;
    for (MachineBasicBlock::iterator I = MBB.begin(), Next = llvm::next(I);
         I != MBB.end(); I = Next, Next = llvm::next(I) ) {
      MachineInstr &MI = *I;
      switch (MI.getOpcode()) {
      default: break;
      case AMDIL::EXPORT_REG:
        exportRegInstrs.push_back(MI.removeFromParent());
        break;
      case AMDIL::STORE_OUTPUT:
        storeOutputInstrs.push_back(MI.removeFromParent());
        break;
      case AMDIL::RESERVE_REG:
      case AMDIL::LOAD_INPUT:
        MBB.insert(MBB.begin(), MI.removeFromParent());
        break;
      case AMDIL::RETURN:
        for (std::vector<MachineInstr*>::iterator SO = storeOutputInstrs.begin(),
                            SO_E = storeOutputInstrs.end();  SO != SO_E; ++SO) {
          MBB.insert(I, *SO);
        }
        for (std::vector<MachineInstr*>::iterator ER = exportRegInstrs.begin(),
                               ER_E = exportRegInstrs.end(); ER != ER_E; ++ER) {
          MBB.insert(I, *ER);
        }
        storeOutputInstrs.clear();
        exportRegInstrs.clear();
        break;
      }
    }
  }


  for (MachineFunction::iterator BB = MF.begin(), BB_E = MF.end();
                                                  BB != BB_E; ++BB) {
    MachineBasicBlock &MBB = *BB;
    for (MachineBasicBlock::iterator I = MBB.begin(); I != MBB.end(); ++I) {
      MachineInstr &MI = *I;
      bool deleteInstr = false;
      switch (MI.getOpcode()) {

      default: break;

      case AMDIL::RESERVE_REG:
      case AMDIL::LOAD_INPUT:
        lowerLOAD_INPUT(MI);
        break;

      case AMDIL::STORE_OUTPUT:
        deleteInstr = lowerSTORE_OUTPUT(MI, MBB, I);
        break;

      case AMDIL::SWIZZLE:
        lowerSWIZZLE(MI);
        deleteInstr = true;
        break;
      }

      if (deleteInstr) {
        --I;
        MI.eraseFromParent();
      }
    }
  }
//  MF.dump();
  return false;
}

/* The goal of this function is to replace the virutal destination register of
 * a LOAD_INPUT instruction with the correct physical register that will.
 *
 * XXX: I don't think this is the right way things assign physical registers,
 * but I'm not sure of another way to do this.
 */
void AMDISALowerShaderInstructionsPass::lowerLOAD_INPUT(MachineInstr &MI)
{
  MachineOperand &dst = MI.getOperand(0);
  MachineOperand &arg = MI.getOperand(1);
  int64_t inputIndex = arg.getImm();
  const TargetRegisterClass * inputClass = TM.getRegisterInfo()->getRegClass(AMDIL::GPRF32RegClassID);
  unsigned newRegister = inputClass->getRegister(inputIndex);
  MRI->replaceRegWith(dst.getReg(), newRegister);
}

bool AMDISALowerShaderInstructionsPass::lowerSTORE_OUTPUT(MachineInstr &MI,
    MachineBasicBlock &MBB, MachineBasicBlock::iterator I)
{
  MachineOperand &dstOp = MI.getOperand(0);
  MachineOperand &valueOp = MI.getOperand(1);
  MachineOperand &indexOp = MI.getOperand(2);
  unsigned valueReg = valueOp.getReg();
  int64_t outputIndex = indexOp.getImm();
  const TargetRegisterClass * outputClass = TM.getRegisterInfo()->getRegClass(AMDIL::GPRF32RegClassID);
  unsigned newRegister = outputClass->getRegister(outputIndex);
  MRI->replaceRegWith(dstOp.getReg(), newRegister);


  BuildMI(MBB, I, MBB.findDebugLoc(I), TM.getInstrInfo()->get(AMDIL::MOVE_f32),
                  newRegister)
      .addOperand(MI.getOperand(1));

  return true;

}

void AMDISALowerShaderInstructionsPass::lowerSWIZZLE(MachineInstr &MI)
{
  MachineOperand &dstOp = MI.getOperand(0);
  MachineOperand &valOp = MI.getOperand(1);
  MachineOperand &swzOp = MI.getOperand(2);
  int64_t swizzle = swzOp.getImm();

  /* Set the swizzle for all of the uses */
  for (MachineRegisterInfo::use_iterator UI = MRI->use_begin(dstOp.getReg()),
       UE = MRI->use_end(); UI != UE; ++UI) {
    UI.getOperand().setTargetFlags(swizzle);
  }

  /* Progate the swizzle instruction */
  MRI->replaceRegWith(dstOp.getReg(), valOp.getReg());
}
