//   Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/fluid/framework/ir/fuse_optimizer_ops_pass/fuse_optimizer_op_pass.h"
#include "paddle/fluid/framework/ir/graph_helper.h"
#include "paddle/fluid/framework/operator.h"
#include "paddle/phi/core/kernel_factory.h"

namespace paddle {
namespace framework {
namespace ir {

void FuseOptimizerOpPass::ApplyImpl(ir::Graph *graph) const {
  ir::Graph &result = *graph;

  const std::string fuse_op_type = GetOpType();
  std::vector<std::string> aux_var_names = GetAuxiliaryVarNames();
  aux_var_names.emplace_back(kParam);
  aux_var_names.emplace_back(kGrad);

  // Step 1: Get the specified op and auxiliary variables.
  std::vector<ir::Node *> topo_nodes = ir::TopologySortOperations(result);
  auto vars_info = GetVarInfo(result);
  std::vector<ir::Node *> opt_nodes;
  size_t opt_ops_num = 0;
  // Note: Only take care about the dense gradients.
  for (auto &node : topo_nodes) {
    if (node->Op()->Type() == fuse_op_type) {
      auto grad_name = node->Op()->Input(kGrad);
      PADDLE_ENFORCE_EQ(
          grad_name.size(), static_cast<size_t>(1),
          platform::errors::InvalidArgument(
              "The %s operator has multiple gradient input. Expected "
              "it to only have one gradient input.",
              fuse_op_type));
      if (IsLoDTensorType(GetTypeOfVar(vars_info, grad_name[0]))) {
        opt_nodes.emplace_back(node);
      }
      ++opt_ops_num;
    }
  }

  VLOG(6) << "Find " << fuse_op_type << " operators : " << opt_ops_num
          << ", and " << opt_nodes.size() << " for dense gradients.";

  if (opt_nodes.size() <= 1) return;
  if (result.Has(details::kFusedOptType)) {
    auto &opt_type = result.Get<details::FusedOptType>(details::kFusedOptType);
    VLOG(6) << "Currently only support fusing one type of optimizer op, "
            << opt_type << " has been fused.";
    return;
  }

  // There should not have no-ctr-var between the opt_nodes that link the
  // op_node
  // of opt_nodes.
  if (HasVarDepsBetweenOps(topo_nodes, opt_nodes)) {
    VLOG(6) << "There are interdependent variables among these optimization "
               "operators, which can not be handled well at present.";
    return;
  }

  result.Set(details::kFusedOptType, new details::FusedOptType);
  result.Get<details::FusedOptType>(details::kFusedOptType) = fuse_op_type;
  if (!result.Has(details::kStartupProgramDescs)) {
    result.Set(details::kStartupProgramDescs, new details::ProgramDescs);
  }
  if (!result.Has(details::kProgramDescs)) {
    result.Set(details::kProgramDescs, new details::ProgramDescs);
  }

  // Step 2: Insert fused_var_name to FusedVars, and the FusedVars need be
  // initialized in scopes before execution.
  if (!result.Has(details::kFusedVars)) {
    result.Set(details::kFusedVars, new details::FusedVars);
  }
  std::unordered_map<std::string, std::vector<std::string>> aux_var_map;
  GetFusingVarNamesMap(aux_var_names, opt_nodes, &aux_var_map);
  std::unordered_map<std::string, std::string> fused_vars_name;
  fused_vars_name.reserve(aux_var_names.size());
  auto &fused_var_set = result.Get<details::FusedVars>(details::kFusedVars);
  const std::string prefix(details::kFusedVarNamePrefix);
  for (auto &var_name : aux_var_names) {
    // NOTE: the fused_var_name should be unique.
    auto fused_var_name = prefix + "_" + fuse_op_type + "_" + var_name + "_" +
                          aux_var_map[var_name][0];
    VLOG(6) << var_name << ": " << fused_var_name;
    PADDLE_ENFORCE_EQ(
        fused_var_set.count(fused_var_name), 0,
        platform::errors::AlreadyExists(
            "The fused variable(%s) already exists.", fused_var_name));
    // FIXME(wangxi). update persistable
    details::VariableInfo var_info;
    var_info.name_ = fused_var_name;
    var_info.type_ = proto::VarType::LOD_TENSOR;
    var_info.persistable_ = false;
    fused_var_set.insert({fused_var_name, var_info});
    fused_vars_name.emplace(var_name, fused_var_name);
  }

  // Step 3: Get the fused Gradient's name
  bool grad_fused = false;
  if (result.Has(details::kParamsAndDenseGrads)) {
    // NOTE: kParamsAndDenseGrads is generated by
    // alloc_continue_space_for_grad_pass
    auto &params_and_dense_grads =
        result.Get<details::ParamsAndGrads>(details::kParamsAndDenseGrads);
    PADDLE_ENFORCE_LE(
        params_and_dense_grads.size(), aux_var_map.at(kGrad).size(),
        platform::errors::InvalidArgument(
            "The number of dense gradients(%d) should be "
            "little than optimizer ops(%d).",
            params_and_dense_grads.size(), aux_var_map.at(kGrad).size()));

    std::unordered_set<std::string> opt_grad_set(aux_var_map.at(kGrad).size());
    for (auto &p_g : params_and_dense_grads) {
      opt_grad_set.insert(p_g.second);
    }
    std::vector<size_t> new_grad_idx;
    for (size_t idx = 0; idx < aux_var_map.at(kGrad).size(); ++idx) {
      auto &grad = aux_var_map.at(kGrad).at(idx);
      if (!opt_grad_set.count(grad)) {
        new_grad_idx.emplace_back(idx);
      }
    }

    // NOTE(zcd): the gradient of kParamsAndDenseGrads may be different
    // with the kGrad. The gradients of kParamsAndDenseGrads is
    // collected during backward stage, but in optimization state, the
    // some gradient's name maybe changed.
    if (new_grad_idx.size() == 0) {
      if (!result.Has(details::kFusedGrads)) {
        PADDLE_THROW(platform::errors::PreconditionNotMet(
            "The coalesce_grad_tensor_pass should "
            "be called before this pass."));
      }
      auto &fused_grad = result.Get<details::FusedGrads>(details::kFusedGrads);
      PADDLE_ENFORCE_NE(fused_grad.size(), 0,
                        platform::errors::NotFound(
                            "The fused gradient should not be empty."));
      if (fused_grad.size() > 1) {
        // Note(chenweihang): Because the dtype of those gradients is not
        //   unified,so the number of fused gradients is more than one,
        //   but it is not supported currently.
        return;
      }
      auto &fused_vars = result.Get<details::FusedVars>(details::kFusedVars);

      auto iter = fused_vars.find(fused_grad.front());
      PADDLE_ENFORCE_EQ(
          iter != fused_vars.end(), true,
          platform::errors::NotFound("Not found the fused gradient variable."));
      fused_vars_name[kGrad] = fused_grad.front();

      // Sort the parameters and auxiliary variables according
      // to parameters' name to make variables' name correspond correctly.
      SortParametersAndAuxVars(params_and_dense_grads, &aux_var_map,
                               &opt_nodes);
      grad_fused = true;
    } else {
      VLOG(6) << "The number of new gradients is " << new_grad_idx.size();
      if (new_grad_idx.size() == 1) return;
      // NOTE(zcd): If the gradients of backward stage and optimization stage
      // have diff, Only take care of the the gradient of optimization stage.
      GradientsFilter(new_grad_idx, &opt_nodes, &aux_var_map);
    }
  }

  // Pass pre-condition check: check dtype of fusing vars
  auto fusing_var_dtype =
      GetDtypeOfVar(vars_info, aux_var_map.at(kParam).front());
  for (auto vars : aux_var_map) {
    for (auto &var_name : vars.second) {
      if (fusing_var_dtype != GetDtypeOfVar(vars_info, var_name)) {
        // Note(chenweihang): Currently the fuse_optimizer_ops strategy
        //   in mixed precision scenarios is not yet supported.
        return;
      }
    }
  }

  // Pass pre-condition check: gradients generated op kernel
  auto fusing_grad_var_names = aux_var_map.at(kGrad);
  for (auto grad_var_name : fusing_grad_var_names) {
    if (!GradGeneratedOpKernelCheck(vars_info, grad_var_name)) {
      // Note(chenweihang): Currently the fuse_optimizer_ops strategy is risky
      //   when gradient generated operator with kernel just support CPU or
      //   GPU device, so close it.
      return;
    }
  }

  LOG(WARNING) << "Find " << fuse_op_type << " operators : " << opt_ops_num
               << ", and " << opt_nodes.size() << " for dense gradients. "
               << "To make the speed faster, those optimization are fused "
                  "during training.";

  // Step 4: Alloc continuous space for Parameters and AuxiliaryVar(e.g.
  // Moment1, Moment2, Beta1Pow, Beta2Pow) of all the optimizer ops
  // separately.
  if (!grad_fused) {
    FuseGradientsToContinuousSpace(
        aux_var_map.at(kParam), aux_var_map.at(kGrad),
        fused_vars_name.at(kGrad), fusing_var_dtype, &result);
  }
  aux_var_names.pop_back();
  FuseVarsToContinuousSpace(aux_var_names, aux_var_map, fused_vars_name,
                            fusing_var_dtype, &result);

  // Step 5: Fuse optimizer Ops and Scale Ops
  auto *fused_opt_node =
      FuseOptimizerOps(aux_var_map, fused_vars_name, opt_nodes, &result);

  InsertInputAndOutputForFusedOpNode(opt_nodes, graph, fused_opt_node);
  // Step 6: Remove optimizer Ops
  for (auto &opt_op : opt_nodes) {
    graph->RemoveNode(opt_op);
  }
}

bool FuseOptimizerOpPass::HasVarDepsBetweenOps(
    const std::vector<Node *> &topo_nodes,
    const std::vector<Node *> &opt_nodes) const {
  std::unordered_map<Node *, std::unordered_set<Node *>> preceding_ops;
  std::unordered_map<Node *, std::unordered_set<Node *>> pending_ops;
  for (auto &op : topo_nodes) {
    preceding_ops[op];
    pending_ops[op];
    for (auto &var : op->outputs) {
      if (var->IsCtrlVar()) continue;
      for (auto &pending_op : var->outputs) {
        preceding_ops[pending_op].insert(op);
        pending_ops[op].insert(pending_op);
      }
    }
  }

  std::unordered_set<Node *> opt_node_set(opt_nodes.begin(), opt_nodes.end());
  auto has_var_deps = [](const std::unordered_set<Node *> &op_set1,
                         const std::unordered_set<Node *> &op_set2) -> bool {
    std::set<Node *> intersect_ops;
    set_intersection(op_set1.begin(), op_set1.end(), op_set2.begin(),
                     op_set2.end(),
                     inserter(intersect_ops, intersect_ops.begin()));
    return !intersect_ops.empty();
  };

  for (auto opt_node : opt_node_set) {
    if (has_var_deps(preceding_ops.at(opt_node), opt_node_set)) {
      return true;
    }
    if (has_var_deps(pending_ops.at(opt_node), opt_node_set)) {
      return true;
    }
  }
  return false;
}

bool FuseOptimizerOpPass::OpWithKernelSupportCPUAndGPU(
    const std::string &op_type) const {
  if (op_type == "c_sync_calc_stream" || op_type == "c_sync_comm_stream") {
    return true;
  }
  bool support_cpu = false;
  bool support_gpu = false;
  auto &kernel_factory = phi::KernelFactory::Instance();
  auto kernel_key_map =
      kernel_factory.SelectKernelMap(phi::TransToPhiKernelName(op_type));
  bool has_op_kernel = kernel_key_map.size() > 0 ? true : false;
  for (auto &kernel : kernel_key_map) {
    if (platform::is_gpu_place(phi::TransToPhiPlace(kernel.first.backend()))) {
      support_gpu = true;
    } else if (platform::is_cpu_place(
                   phi::TransToPhiPlace(kernel.first.backend()))) {
      support_cpu = true;
    }
  }

  if (!support_cpu || !support_gpu) {
    auto &all_kernels = OperatorWithKernel::AllOpKernels();
    auto it = all_kernels.find(op_type);
    // skip op not has kernel
    if (it != all_kernels.end()) {
      has_op_kernel = true;
      for (auto &kernel_pair : it->second) {
        if (platform::is_cpu_place(kernel_pair.first.place_)) {
          support_cpu = true;
        } else if (platform::is_gpu_place(kernel_pair.first.place_)) {
          support_gpu = true;
        }
      }
    }
  }

  VLOG(6) << "Op check: " << op_type << ", support CPU: " << support_cpu
          << ", support GPU: " << support_gpu;
  return has_op_kernel ? (support_cpu && support_gpu) : true;
}

bool FuseOptimizerOpPass::GradGeneratedOpKernelCheck(
    const std::unordered_map<std::string, std::vector<ir::Node *>> &vars_info,
    const std::string &grad_var_name) const {
  auto grad_var_nodes = vars_info.at(grad_var_name);
  std::unordered_set<std::string> check_op_set;
  for (auto var_node : grad_var_nodes) {
    for (auto in_node : var_node->inputs) {
      if (in_node->IsOp() && in_node->Op()) {
        check_op_set.emplace(in_node->Op()->Type());
      }
    }
  }
  for (auto op_type : check_op_set) {
    if (!OpWithKernelSupportCPUAndGPU(op_type)) {
      return false;
    }
  }
  return true;
}

void FuseOptimizerOpPass::GradientsFilter(
    const std::vector<size_t> &new_grad_idx, std::vector<Node *> *opt_nodes,
    std::unordered_map<std::string, std::vector<std::string>> *aux_var_map)
    const {
  for (auto &aux_vars : *aux_var_map) {
    std::vector<std::string> sorted_vars;
    sorted_vars.reserve(aux_vars.second.size());
    for (size_t i : new_grad_idx) {
      sorted_vars.emplace_back(aux_vars.second.at(i));
    }
    std::swap(aux_vars.second, sorted_vars);
    if (VLOG_IS_ON(6)) {
      std::stringstream out;
      for (auto &var_name : aux_vars.second) {
        out << var_name << " ";
      }
      VLOG(6) << aux_vars.first << ": " << out.str();
    }
  }
  std::vector<Node *> sorted_ops;
  for (size_t i : new_grad_idx) {
    sorted_ops.emplace_back(opt_nodes->at(i));
  }
  std::swap(*opt_nodes, sorted_ops);
}

void FuseOptimizerOpPass::FuseGradientsToContinuousSpace(
    const std::vector<std::string> &params,
    const std::vector<std::string> &grads, const std::string &fused_grad_name,
    const proto::VarType::Type &dtype, ir::Graph *result) const {
  auto &pinned_var_set =
      result->GetOrInit<details::PinnedVars>(details::kPinnedVars);

  auto vars_info = GetVarInfo(*result);
  // The Gradients should not be reused during memory optimization.
  for (auto &grad_var_name : grads) {
    auto iter = vars_info.find(grad_var_name);
    PADDLE_ENFORCE_EQ(
        iter != vars_info.end(), true,
        platform::errors::NotFound("The gradient variable %s is not found.",
                                   grad_var_name));
    PADDLE_ENFORCE_EQ(
        !iter->second.empty(), true,
        platform::errors::NotFound("The gradient var node %s is not found.",
                                   grad_var_name));
    PADDLE_ENFORCE_NOT_NULL(
        iter->second.front()->Var(),
        platform::errors::InvalidArgument("The gradient var(%s) node is null.",
                                          grad_var_name));
    PADDLE_ENFORCE_EQ(
        IsLoDTensorType(iter->second.front()->Var()->GetType()), true,
        platform::errors::InvalidArgument(
            "Currently the gradient(%s) type only should be LoDTensor when "
            "fusing optimizer ops.",
            grad_var_name));
    for (auto var : iter->second) {
      pinned_var_set.insert(var->Var()->Name());
    }
  }

  // Define Ops
  result->Get<details::ProgramDescs>(details::kProgramDescs).emplace_back();
  ProgramDesc &program_desc =
      result->Get<details::ProgramDescs>(details::kProgramDescs).back();
  auto *global_block = program_desc.MutableBlock(0);
  AppendCoalesceTensorOp(params, grads, fused_grad_name, dtype, global_block,
                         false, false);
}

std::unordered_map<std::string, std::vector<Node *>>
FuseOptimizerOpPass::GetVarInfo(const Graph &result) const {
  std::unordered_map<std::string, std::vector<Node *>> vars;
  for (Node *node : result.Nodes()) {
    if (node->IsVar() && node->Var()) {
      // Note: The graph may have the same name node. For example, parameter
      // is the input of optimizer and it also is the output of optimizer;
      vars[node->Var()->Name()].emplace_back(node);
    }
  }
  return vars;
}

bool FuseOptimizerOpPass::IsLoDTensorType(
    const proto::VarType::Type &type) const {
  // Current only support LOD_TENSOR.
  return type == proto::VarType::LOD_TENSOR;
}

const VarDesc *FuseOptimizerOpPass::GetVarDescFromVarsInfo(
    const std::unordered_map<std::string, std::vector<Node *>> &vars_info,
    const std::string &var_name) const {
  auto grad_iter = vars_info.find(var_name);
  PADDLE_ENFORCE_EQ(grad_iter != vars_info.end(), true,
                    platform::errors::NotFound(
                        "The gradient variable %s is not found.", var_name));
  PADDLE_ENFORCE_EQ(!grad_iter->second.empty(), true,
                    platform::errors::NotFound(
                        "The gradient var node %s is not found.", var_name));
  PADDLE_ENFORCE_NOT_NULL(grad_iter->second.front()->Var(),
                          platform::errors::InvalidArgument(
                              "The gradient var(%s) node is null.", var_name));
  return grad_iter->second.front()->Var();
}

proto::VarType::Type FuseOptimizerOpPass::GetDtypeOfVar(
    const std::unordered_map<std::string, std::vector<ir::Node *>> &vars_info,
    const std::string &name) const {
  auto var_desc = GetVarDescFromVarsInfo(vars_info, name);
  return var_desc->GetDataType();
}

proto::VarType::Type FuseOptimizerOpPass::GetTypeOfVar(
    const std::unordered_map<std::string, std::vector<Node *>> &vars_info,
    const std::string &name) const {
  auto var_desc = GetVarDescFromVarsInfo(vars_info, name);
  return var_desc->GetType();
}

void FuseOptimizerOpPass::FuseVarsToContinuousSpace(
    const std::vector<std::string> &aux_var_names,
    const std::unordered_map<std::string, std::vector<std::string>>
        &aux_var_map,
    const std::unordered_map<std::string, std::string> &fused_vars_name,
    const proto::VarType::Type &dtype, ir::Graph *result) const {
  // Define Ops
  result->Get<details::ProgramDescs>(details::kProgramDescs).emplace_back();
  ProgramDesc &program_desc =
      result->Get<details::ProgramDescs>(details::kProgramDescs).back();
  auto *global_block = program_desc.MutableBlock(0);
  for (auto &var_name : aux_var_names) {
    VLOG(6) << "aux_var_names : " << var_name
            << ". fused_vars_name: " << fused_vars_name.at(var_name);
    AppendCoalesceTensorOp(aux_var_map.at(var_name), aux_var_map.at(var_name),
                           fused_vars_name.at(var_name), dtype, global_block,
                           true);
  }
}

void FuseOptimizerOpPass::SortParametersAndAuxVars(
    const std::vector<std::pair<std::string, std::string>> &params_grads,
    std::unordered_map<std::string, std::vector<std::string>> *aux_var_map,
    std::vector<ir::Node *> *ops) const {
  PADDLE_ENFORCE_NE(
      aux_var_map->count(kGrad), static_cast<size_t>(0),
      platform::errors::NotFound("The gradient variable doesn‘t exist."));
  auto &grad_vec = aux_var_map->at(kGrad);

  std::vector<size_t> grad_sort_idx;
  grad_sort_idx.reserve(grad_vec.size());

  for (auto &p_g : params_grads) {
    auto iter = std::find(grad_vec.begin(), grad_vec.end(), p_g.second);
    PADDLE_ENFORCE_EQ(
        iter != grad_vec.end(), true,
        platform::errors::NotFound(
            "Parameter@Grad(%s) is not found in gradient vector.", p_g.second));
    auto idx = std::distance(grad_vec.begin(), iter);
    grad_sort_idx.emplace_back(idx);
  }

  for (auto &aux_vars : *aux_var_map) {
    std::vector<std::string> sorted_vars;
    sorted_vars.reserve(aux_vars.second.size());
    for (size_t i = 0; i < aux_vars.second.size(); ++i) {
      sorted_vars.emplace_back(aux_vars.second.at(grad_sort_idx[i]));
    }
    std::swap(aux_vars.second, sorted_vars);

    if (VLOG_IS_ON(6)) {
      std::stringstream out;
      for (auto &var_name : aux_vars.second) {
        out << var_name << " ";
      }
      VLOG(6) << aux_vars.first << ": " << out.str();
    }
  }

  std::vector<ir::Node *> sorted_ops;
  sorted_ops.reserve(ops->size());
  for (size_t i = 0; i < ops->size(); ++i) {
    sorted_ops.emplace_back(ops->at(grad_sort_idx[i]));
  }
  std::swap(*ops, sorted_ops);
}

void FuseOptimizerOpPass::GetFusingVarNamesMap(
    const std::vector<std::string> &aux_vars_name,
    const std::vector<ir::Node *> &opt_nodes,
    std::unordered_map<std::string, std::vector<std::string>> *aux_args_name)
    const {
  for (auto &node : opt_nodes) {
    for (auto &var_n : aux_vars_name) {
      auto arg_names = node->Op()->Input(var_n);
      PADDLE_ENFORCE_EQ(arg_names.size(), static_cast<size_t>(1),
                        platform::errors::InvalidArgument(
                            "The input variable of optimizer to be fused is "
                            "invalid. Excepted %s only has one %s input.",
                            node->Op()->Type(), var_n));
      (*aux_args_name)[var_n].emplace_back(arg_names[0]);
    }
  }
}

void FuseOptimizerOpPass::AppendCoalesceTensorOp(
    const std::vector<std::string> &in_args,
    const std::vector<std::string> &out_args, const std::string &fused_out_arg,
    const proto::VarType::Type &dtype, BlockDesc *global_block, bool copy_data,
    bool check_name) const {
  auto op_desc = global_block->AppendOp();
  op_desc->SetType("coalesce_tensor");
  op_desc->SetInput("Input", in_args);
  op_desc->SetOutput("Output", out_args);
  op_desc->SetOutput("FusedOutput", {fused_out_arg});
  op_desc->SetAttr("copy_data", copy_data);
  op_desc->SetAttr("check_name", check_name);
  op_desc->SetAttr("dtype", static_cast<int>(dtype));
}

void FuseOptimizerOpPass::InsertInputAndOutputForFusedOpNode(
    const std::vector<ir::Node *> &op_nodes, ir::Graph *graph,
    ir::Node *fused_opt_node) const {
  std::unordered_set<ir::Node *> inputs;
  std::unordered_set<ir::Node *> outputs;
  for (auto opt_op : op_nodes) {
    inputs.insert(opt_op->inputs.begin(), opt_op->inputs.end());
    for (auto &input : opt_op->inputs) {
      replace(input->outputs.begin(), input->outputs.end(), opt_op,
              fused_opt_node);
    }
    outputs.insert(opt_op->outputs.begin(), opt_op->outputs.end());
    for (auto &output : opt_op->outputs) {
      replace(output->inputs.begin(), output->inputs.end(), opt_op,
              fused_opt_node);
    }
  }

  // Remove the dependence vars between op_nodes.
  std::unordered_set<ir::Node *> out_dep_vars;
  std::unordered_set<ir::Node *> not_useful_vars;

  auto deal_with_ctrl_vars = [&out_dep_vars, &not_useful_vars,
                              &fused_opt_node](ir::Node *ctr_var_node) {
    PADDLE_ENFORCE_EQ(ctr_var_node->inputs.size(), 1,
                      platform::errors::InvalidArgument(
                          "The control var(%s) node has multiple inputs.",
                          ctr_var_node->Name()));
    if (ctr_var_node->inputs.front() == fused_opt_node) {
      PADDLE_ENFORCE_GT(
          ctr_var_node->outputs.size(), 0,
          platform::errors::InvalidArgument(
              "The control var(%s) node has no output.", ctr_var_node->Name()));
      auto output_ops = ctr_var_node->outputs;
      output_ops.erase(std::remove_if(output_ops.begin(), output_ops.end(),
                                      [&fused_opt_node](const ir::Node *node) {
                                        return node == fused_opt_node;
                                      }),
                       output_ops.end());
      if (!output_ops.empty()) {
        out_dep_vars.insert(ctr_var_node);
      }
      not_useful_vars.insert(ctr_var_node);
    }
  };

  for (auto *in_node : inputs) {
    if (in_node->IsCtrlVar()) {
      deal_with_ctrl_vars(in_node);
    }
  }

  for (auto *out_node : outputs) {
    if (out_node->IsCtrlVar()) {
      deal_with_ctrl_vars(out_node);
    }
  }

  for (auto &node : not_useful_vars) {
    if (inputs.count(node)) {
      inputs.erase(node);
    }
    if (outputs.count(node)) {
      outputs.erase(node);
    }
  }

  for (auto &dep_var : out_dep_vars) {
    if (not_useful_vars.count(dep_var)) {
      not_useful_vars.erase(dep_var);
    }
    dep_var->inputs.clear();
    dep_var->inputs.emplace_back(fused_opt_node);
  }

  outputs.insert(out_dep_vars.begin(), out_dep_vars.end());

  auto nodes_to_string =
      [](std::unordered_set<ir::Node *> nodes) -> std::string {
    std::stringstream ss;
    for (auto n : nodes) {
      if (n->IsVar()) {
        ss << n->Name() << " ";
      }
    }
    return ss.str();
  };

  VLOG(4) << "add inputs to " << fused_opt_node->Op()->Type() << ": "
          << nodes_to_string(inputs);
  VLOG(4) << "add outputs to " << fused_opt_node->Op()->Type() << ": "
          << nodes_to_string(outputs);

  fused_opt_node->inputs.insert(fused_opt_node->inputs.begin(), inputs.begin(),
                                inputs.end());
  fused_opt_node->outputs.insert(fused_opt_node->outputs.begin(),
                                 outputs.begin(), outputs.end());

  for (auto &ctrl_var_node : not_useful_vars) {
    graph->RemoveNode(ctrl_var_node);
  }
}
}  // namespace ir
}  // namespace framework
}  // namespace paddle
