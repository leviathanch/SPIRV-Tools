// Copyright (c) 2018 The Khronos Group Inc.
// Copyright (c) 2018 Valve Corporation
// Copyright (c) 2018 LunarG Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dead_insert_elim_pass.h"

#include "composite.h"
#include "ir_context.h"
#include "iterator.h"
#include "spirv/1.2/GLSL.std.450.h"

#include <vector>

namespace spvtools {
namespace opt {

namespace {

const uint32_t kTypeVectorCountInIdx = 1;
const uint32_t kTypeMatrixCountInIdx = 1;
const uint32_t kTypeArrayLengthIdInIdx = 1;
const uint32_t kTypeIntWidthInIdx = 0;
const uint32_t kConstantValueInIdx = 0;
const uint32_t kInsertObjectIdInIdx = 0;
const uint32_t kInsertCompositeIdInIdx = 1;

}  // anonymous namespace

uint32_t DeadInsertElimPass::NumComponents(ir::Instruction* typeInst) {
  switch (typeInst->opcode()) {
    case SpvOpTypeVector: {
      return typeInst->GetSingleWordInOperand(kTypeVectorCountInIdx);
    } break;
    case SpvOpTypeMatrix: {
      return typeInst->GetSingleWordInOperand(kTypeMatrixCountInIdx);
    } break;
    case SpvOpTypeArray: {
      uint32_t lenId =
          typeInst->GetSingleWordInOperand(kTypeArrayLengthIdInIdx);
      ir::Instruction* lenInst = get_def_use_mgr()->GetDef(lenId);
      if (lenInst->opcode() != SpvOpConstant) return 0;
      uint32_t lenTypeId = lenInst->type_id();
      ir::Instruction* lenTypeInst = get_def_use_mgr()->GetDef(lenTypeId);
      // TODO(greg-lunarg): Support non-32-bit array length
      if (lenTypeInst->GetSingleWordInOperand(kTypeIntWidthInIdx) != 32)
        return 0;
      return lenInst->GetSingleWordInOperand(kConstantValueInIdx);
    } break;
    case SpvOpTypeStruct: {
      return typeInst->NumInOperands();
    } break;
    default: { return 0; } break;
  }
}

void DeadInsertElimPass::MarkInsertChain(ir::Instruction* insertChain,
                                         std::vector<uint32_t>* pExtIndices,
                                         uint32_t extOffset) {
  // Not currently optimizing array inserts.
  ir::Instruction* typeInst = get_def_use_mgr()->GetDef(insertChain->type_id());
  if (typeInst->opcode() == SpvOpTypeArray) return;
  // Insert chains are only composed of inserts and phis
  if (insertChain->opcode() != SpvOpCompositeInsert &&
      insertChain->opcode() != SpvOpPhi)
    return;
  // If extract indices are empty, mark all subcomponents if type
  // is constant length.
  if (pExtIndices == nullptr) {
    uint32_t cnum = NumComponents(typeInst);
    if (cnum > 0) {
      std::vector<uint32_t> extIndices;
      for (uint32_t i = 0; i < cnum; i++) {
        extIndices.clear();
        extIndices.push_back(i);
        MarkInsertChain(insertChain, &extIndices, 0);
      }
      return;
    }
  }
  ir::Instruction* insInst = insertChain;
  while (insInst->opcode() == SpvOpCompositeInsert) {
    // If no extract indices, mark insert and inserted object (which might
    // also be an insert chain) and continue up the chain though the input
    // composite.
    //
    // Note: We mark inserted objects in this function (rather than in
    // EliminateDeadInsertsOnePass) because in some cases, we can do it
    // more accurately here.
    if (pExtIndices == nullptr) {
      liveInserts_.insert(insInst->result_id());
      uint32_t objId = insInst->GetSingleWordInOperand(kInsertObjectIdInIdx);
      MarkInsertChain(get_def_use_mgr()->GetDef(objId), nullptr, 0);
    }
    // If extract indices match insert, we are done. Mark insert and
    // inserted object.
    else if (ExtInsMatch(*pExtIndices, insInst, extOffset)) {
      liveInserts_.insert(insInst->result_id());
      uint32_t objId = insInst->GetSingleWordInOperand(kInsertObjectIdInIdx);
      MarkInsertChain(get_def_use_mgr()->GetDef(objId), nullptr, 0);
      break;
    }
    // If non-matching intersection, mark insert
    else if (ExtInsConflict(*pExtIndices, insInst, extOffset)) {
      liveInserts_.insert(insInst->result_id());
      // If more extract indices than insert, we are done. Use remaining
      // extract indices to mark inserted object.
      uint32_t numInsertIndices = insInst->NumInOperands() - 2;
      if (pExtIndices->size() - extOffset > numInsertIndices) {
        uint32_t objId = insInst->GetSingleWordInOperand(kInsertObjectIdInIdx);
        MarkInsertChain(get_def_use_mgr()->GetDef(objId), pExtIndices,
                        extOffset + numInsertIndices);
        break;
      }
      // If fewer extract indices than insert, also mark inserted object and
      // continue up chain.
      else {
        uint32_t objId = insInst->GetSingleWordInOperand(kInsertObjectIdInIdx);
        MarkInsertChain(get_def_use_mgr()->GetDef(objId), nullptr, 0);
      }
    }
    // Get next insert in chain
    const uint32_t compId =
        insInst->GetSingleWordInOperand(kInsertCompositeIdInIdx);
    insInst = get_def_use_mgr()->GetDef(compId);
  }
  // If insert chain ended with phi, do recursive call on each operand
  if (insInst->opcode() != SpvOpPhi) return;
  // Mark phi visited to prevent potential infinite loop. If phi is already
  // visited, return to avoid infinite loop
  if (!visitedPhis_.insert(insInst->result_id()).second) return;
  uint32_t icnt = 0;
  insInst->ForEachInId([&icnt, &pExtIndices, &extOffset, this](uint32_t* idp) {
    if (icnt % 2 == 0) {
      ir::Instruction* pi = get_def_use_mgr()->GetDef(*idp);
      MarkInsertChain(pi, pExtIndices, extOffset);
    }
    ++icnt;
  });
  // Unmark phi when done visiting
  visitedPhis_.erase(insInst->result_id());
}

bool DeadInsertElimPass::EliminateDeadInserts(ir::Function* func) {
  bool modified = false;
  bool lastmodified = true;
  // Each pass can delete dead instructions, thus potentially revealing
  // new dead insertions ie insertions with no uses.
  while (lastmodified) {
    lastmodified = EliminateDeadInsertsOnePass(func);
    modified |= lastmodified;
  }
  return modified;
}

bool DeadInsertElimPass::EliminateDeadInsertsOnePass(ir::Function* func) {
  bool modified = false;
  liveInserts_.clear();
  visitedPhis_.clear();
  // Mark all live inserts
  for (auto bi = func->begin(); bi != func->end(); ++bi) {
    for (auto ii = bi->begin(); ii != bi->end(); ++ii) {
      // Only process Inserts and composite Phis
      SpvOp op = ii->opcode();
      ir::Instruction* typeInst = get_def_use_mgr()->GetDef(ii->type_id());
      if (op != SpvOpCompositeInsert &&
          (op != SpvOpPhi || !spvOpcodeIsComposite(typeInst->opcode())))
        continue;
      // The marking algorithm can be expensive for large arrays and the
      // efficacy of eliminating dead inserts into arrays is questionable.
      // Skip optimizing array inserts for now. Just mark them live.
      // TODO(greg-lunarg): Eliminate dead array inserts
      if (op == SpvOpCompositeInsert) {
        if (typeInst->opcode() == SpvOpTypeArray) {
          liveInserts_.insert(ii->result_id());
          continue;
        }
      }
      const uint32_t id = ii->result_id();
      get_def_use_mgr()->ForEachUser(id, [&ii, this](ir::Instruction* user) {
        switch (user->opcode()) {
          case SpvOpCompositeInsert:
          case SpvOpPhi:
            // Use by insert or phi does not initiate marking
            break;
          case SpvOpCompositeExtract: {
            // Capture extract indices
            std::vector<uint32_t> extIndices;
            uint32_t icnt = 0;
            user->ForEachInOperand([&icnt, &extIndices](const uint32_t* idp) {
              if (icnt > 0) extIndices.push_back(*idp);
              ++icnt;
            });
            // Mark all inserts in chain that intersect with extract
            MarkInsertChain(&*ii, &extIndices, 0);
          } break;
          default: {
            // Mark inserts in chain for all components
            MarkInsertChain(&*ii, nullptr, 0);
          } break;
        }
      });
    }
  }
  // Find and disconnect dead inserts
  std::vector<ir::Instruction*> dead_instructions;
  for (auto bi = func->begin(); bi != func->end(); ++bi) {
    for (auto ii = bi->begin(); ii != bi->end(); ++ii) {
      if (ii->opcode() != SpvOpCompositeInsert) continue;
      const uint32_t id = ii->result_id();
      if (liveInserts_.find(id) != liveInserts_.end()) continue;
      const uint32_t replId =
          ii->GetSingleWordInOperand(kInsertCompositeIdInIdx);
      (void)context()->ReplaceAllUsesWith(id, replId);
      dead_instructions.push_back(&*ii);
      modified = true;
    }
  }
  // DCE dead inserts
  while (!dead_instructions.empty()) {
    ir::Instruction* inst = dead_instructions.back();
    dead_instructions.pop_back();
    DCEInst(inst, [&dead_instructions](ir::Instruction* other_inst) {
      auto i = std::find(dead_instructions.begin(), dead_instructions.end(),
                         other_inst);
      if (i != dead_instructions.end()) {
        dead_instructions.erase(i);
      }
    });
  }
  return modified;
}

void DeadInsertElimPass::Initialize(ir::IRContext* c) {
  InitializeProcessing(c);

  // Initialize extension whitelist
  InitExtensions();
};

bool DeadInsertElimPass::AllExtensionsSupported() const {
  // If any extension not in whitelist, return false
  for (auto& ei : get_module()->extensions()) {
    const char* extName =
        reinterpret_cast<const char*>(&ei.GetInOperand(0).words[0]);
    if (extensions_whitelist_.find(extName) == extensions_whitelist_.end())
      return false;
  }
  return true;
}

Pass::Status DeadInsertElimPass::ProcessImpl() {
  // Do not process if any disallowed extensions are enabled
  if (!AllExtensionsSupported()) return Status::SuccessWithoutChange;
  // Process all entry point functions.
  ProcessFunction pfn = [this](ir::Function* fp) {
    return EliminateDeadInserts(fp);
  };
  bool modified = ProcessEntryPointCallTree(pfn, get_module());
  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

DeadInsertElimPass::DeadInsertElimPass() {}

Pass::Status DeadInsertElimPass::Process(ir::IRContext* c) {
  Initialize(c);
  return ProcessImpl();
}

void DeadInsertElimPass::InitExtensions() {
  extensions_whitelist_.clear();
  extensions_whitelist_.insert({
      "SPV_AMD_shader_explicit_vertex_parameter",
      "SPV_AMD_shader_trinary_minmax",
      "SPV_AMD_gcn_shader",
      "SPV_KHR_shader_ballot",
      "SPV_AMD_shader_ballot",
      "SPV_AMD_gpu_shader_half_float",
      "SPV_KHR_shader_draw_parameters",
      "SPV_KHR_subgroup_vote",
      "SPV_KHR_16bit_storage",
      "SPV_KHR_device_group",
      "SPV_KHR_multiview",
      "SPV_NVX_multiview_per_view_attributes",
      "SPV_NV_viewport_array2",
      "SPV_NV_stereo_view_rendering",
      "SPV_NV_sample_mask_override_coverage",
      "SPV_NV_geometry_shader_passthrough",
      "SPV_AMD_texture_gather_bias_lod",
      "SPV_KHR_storage_buffer_storage_class",
      "SPV_KHR_variable_pointers",
      "SPV_AMD_gpu_shader_int16",
      "SPV_KHR_post_depth_coverage",
      "SPV_KHR_shader_atomic_counter_ops",
  });
}

}  // namespace opt
}  // namespace spvtools
