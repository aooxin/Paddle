/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserve.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include "paddle/framework/backward.h"

#include <list>
#include <memory>

#include "paddle/framework/op_registry.h"
#include "paddle/operators/net_op.h"
#include "paddle/operators/recurrent_op.h"

namespace paddle {
namespace framework {

template <typename Map, typename T>
static void ForEachVarName(const Map& names, T callback) {
  for (auto& name : names) {
    for (auto& n : name.second) {
      if (callback(n)) return;
    }
  }
}

// return whether all the names + suffixes in the set
static bool AllInSet(
    const std::map<std::string, std::vector<std::string>>& names,
    const std::string& suffix, const std::unordered_set<std::string>& set) {
  bool all_in_set = true;
  ForEachVarName(names, [&all_in_set, &set, &suffix](const std::string& n) {
    all_in_set = set.find(n + suffix) != set.end();
    return !all_in_set;
  });
  return all_in_set;
}

static std::unique_ptr<OperatorBase> NOP() {
  auto net_op = new operators::NetOp();
  net_op->SetType("@NOP@");
  net_op->CompleteAddOp();
  return std::unique_ptr<OperatorBase>(net_op);
}

//  Get backward operator from a forward operator, a recursive implementation.
//
//  no_grad_names the gradient variable names without gradient calculating.
//
//  uniq_id is a unique index used inside recursively calling
//  BackwardRecursive. use `uid = uniq_id++;` to get the unique index, and
//  pass `uniq_id` through recursive calling.
//
//  returns The backward operator. In a simple situation, it may be a simple
//  operator, in a complex situation, it maybe a NetOp.
//
//  See Backward.h for details
static std::unique_ptr<OperatorBase> BackwardRecursive(
    const OperatorBase& forwardOp,
    std::unordered_set<std::string>& no_grad_names, size_t& uniq_id) {
  //  If all input gradients of forwarding operator do not need to calculate,
  //  just return an NOP. Not return null ptr because NOP does not take
  //  too much time for calculation, but it is useful for simplifying logic.
  if (AllInSet(forwardOp.Inputs() /*names*/, kGradVarSuffix /*suffix*/,
               no_grad_names /*set*/)) {
    return NOP();
  }

  //  All output gradients of forwarding operator do not need to calculate.
  //  Then all input gradients cannot be computed at all, and we put them into
  //  `no_grad_names` set. Return an NOP.
  if (AllInSet(forwardOp.Outputs() /*names*/, kGradVarSuffix /*suffix*/,
               no_grad_names /*set*/)) {
    ForEachVarName(forwardOp.Inputs(),
                   [&no_grad_names](const std::string& name) -> bool {
                     no_grad_names.insert(GradVarName(name));
                     return false;
                   });
    return NOP();
  }

  // Returned gradient network
  auto net = std::unique_ptr<operators::NetOp>(new operators::NetOp());

  if (forwardOp.IsNetOp()) {
    // Because forwardOp is a net op, it can static_cast.
    auto& forwardNet = static_cast<const operators::NetOp&>(forwardOp);

    // Map from output gradient variable name to operator's indices in
    // backward net's ops_. That operator generates that variable.
    std::unordered_map<std::string, std::vector<size_t>> dup_output_ops;

    size_t local_op_id = 0;
    // reversely travel forwardNet and collect all duplicate outputs.
    for (auto it = forwardNet.ops_.rbegin(); it != forwardNet.ops_.rend();
         ++it, ++local_op_id) {
      auto& fwd = *it;
      auto bwd = BackwardRecursive(*fwd, no_grad_names, uniq_id);
      ForEachVarName(bwd->Outputs(),
                     [&dup_output_ops, local_op_id](const std::string& out) {
                       dup_output_ops[out].emplace_back(local_op_id);
                       return false;
                     });
      net->AppendOp(std::move(bwd));
    }
    // Get unique ID for this method.
    auto uid = uniq_id++;
    // TODO(dzh): more comment
    // multiple operators which have the same output (y for example) may
    // overwrite the same y variable when backward, special operations are token
    // to handle this case. For each duplicate output, rename it to an alias
    // (original name with a offset), append an `add` op for its operator,
    // and finally sum all the alias variable to the final output variable y.
    using Pos = std::pair<size_t, std::unique_ptr<OperatorBase>>;
    std::list<Pos> insert_position;
    for (auto& dup_output_op : dup_output_ops) {
      const std::string& name = dup_output_op.first;
      // duplicate @Empty@ don't need to be added
      if (name == kEmptyVarName) continue;

      auto& dup_op = dup_output_op.second;
      // no duplicate output
      if (dup_op.size() == 1) continue;

      // process the duplicate outputs
      std::vector<std::string> dup_outputs;
      for (size_t i = 0; i < dup_op.size(); ++i) {
        // rename each duplicate output to an alias
        auto op_offset = dup_op[i];
        dup_outputs.push_back(name + "@RENAME@" + std::to_string(uid) + "@" +
                              std::to_string(i));
        net->ops_[op_offset]->Rename(name, dup_outputs.back());
      }
      // collect all the offset to append `add` op for each alias
      insert_position.push_back(
          {dup_op.back(), OpRegistry::CreateOp("add", {{"X", {dup_outputs}}},
                                               {{"Out", {name}}}, {})});
    }

    // make sure the inserted `add` ops follow the BFS order.
    insert_position.sort(
        [](const Pos& l, const Pos& r) { return l.first > r.first; });

    for (auto& pos : insert_position) {
      net->InsertOp(pos.first + 1, std::move(pos.second));
    }
  } else {
    std::unique_ptr<OperatorBase> grad_op(OpRegistry::CreateGradOp(forwardOp));

    ForEachVarName(grad_op->Inputs(), [&no_grad_names, &net, &grad_op](
                                          const std::string& grad_input) {
      if (no_grad_names.count(grad_input)) {
        // +1 for \0
        std::string prefix = grad_input.substr(
            0, grad_input.size() - sizeof(kGradVarSuffix) / sizeof(char) + 1);
        grad_op->Rename(grad_input, prefix + kZeroVarSuffix);

        // If part of input gradient of that operator is not calculated, fill
        // zero variables to that input gradient.
        net->AppendOp(OpRegistry::CreateOp("fill_zeros_like", {{"X", {prefix}}},
                                           {{"Y", {grad_input}}}, {}));
      }
      return false;
    });

    ForEachVarName(grad_op->Outputs(),
                   [&no_grad_names, &grad_op](const std::string& grad_output) {
                     if (no_grad_names.count(grad_output)) {
                       grad_op->Rename(grad_output, kEmptyVarName);
                     }
                     return false;
                   });

    // process recurrent gradient op as a special operator.
    if (forwardOp.Type() == "recurrent") {
      // NOTE clean up cycle call somewhere (RNN's stepnet constains itself), or
      // this will result in infinite loop.
      const auto& rnnop =
          *static_cast<const operators::RecurrentOp*>(&forwardOp);
      auto rnn_grad_op =
          static_cast<operators::RecurrentGradientOp*>(grad_op.get());
      const auto& stepnet_op =
          *static_cast<const OperatorBase*>(&rnnop.stepnet());
      // create stepnet's gradient op
      rnn_grad_op->set_stepnet(
          BackwardRecursive(stepnet_op, no_grad_names, uniq_id));
    }

    if (net->ops_.empty()) {  // Current no aux op is added to network
      return grad_op;
    }
    net->AppendOp(std::move(grad_op));
  }
  net->SetType("@GENERATED_BACKWARD@");
  net->CompleteAddOp();
  return std::unique_ptr<OperatorBase>(
      static_cast<OperatorBase*>(net.release()));
}

// See header for comments
std::unique_ptr<OperatorBase> Backward(
    const OperatorBase& forwardOp,
    const std::unordered_set<std::string>& no_grad_vars) {
  std::unordered_set<std::string> no_grad_names;
  no_grad_names.reserve(no_grad_vars.size() + 1);

  no_grad_names.insert(std::string(kEmptyVarName) + kGradVarSuffix);

  for (auto& name : no_grad_vars) {
    no_grad_names.insert(name + kGradVarSuffix);
  }
  size_t uid = 0;
  return BackwardRecursive(forwardOp, no_grad_names, uid);
}

// ====================================  //

static bool AllGradInSet(const std::vector<std::string>& names,
                         const std::unordered_set<std::string>& set) {
  for (const std::string& name : names) {
    if (!set.count(GradVarName(name))) {
      return false;
    }
  }
  return true;
}

std::vector<OpDescBind> CreatBackwardOps(
    const std::unique_ptr<OpDescBind>& op_desc_ptr,
    unordered_map<std::string>& no_grad_vars) {
  const OpDescBind& op_desc = *op_desc_ptr;
  std::vector<OpDescBind> grad_op_descs;
  // All input gradients of forwarding operator do not need to calculat.
  if (AllGradInSet(op_desc_.InputArgumentNames(), kGradVarSuffix,
                   no_grad_vars)) {
    return grad_op_descs;  // empty vector
  }
  // All output gradients of forwarding operator do not need to calculate.
  const std::vector<std::string>& outputs = op_desc_.OutputArugumentNames();
  if (AllGradInSet(outputs, kGradVarSuffix, no_grad_vars)) {
    for (const std::string& name : outputs) {
      no_grad_vars.insert(GradVarName(name));
    }
    return grad_op_descs;  // empty vector
  }

  grad_op_descs = OpRegistry::CreateGradOpDescs(op_desc);

  std::vector<OpDescBind> fill_zeros_ops;
  for (OpDescBind& desc : grad_op_descs) {
    for (const std::string& in_name : desc.InputArgumentNames()) {
      if (no_grad_vars.count(in_name)) {
        std::string prefix = in_name.substr(
            0, in_name.size() - sizeof(kGradVarSuffix) / sizeof(char) + 1);
        std::string new_name = prefix + kZeroVarSuffix;
        desc.Rename(in_name, new_name);
        OpDescBind op_desc_bind(
            {"fill_zeros_like", {{"X", {prefix}}}, {{"Y", {new_name}}}, {}});
        fill_zeros_ops.push_back(op_desc_bind);
      }
    }
    for (const std::string& out_name : desc.OutputName()) {
      if (no_grad_vars.count(out_name)) {
        desc.Rename(out_name, kEmptyVarName);
      }
    }
  }
  grad_op_descs.insert(grad_op_descs.begin(), fill_zeros_ops.begin(),
                       fill_zeros_ops.end());

  // TODO (fengjiayi): RNN op
  return grad_op_descs;
}

void AppendBackwardOps(BlockDescBind& block_desc,
                       const std::unordered_set<std::string>& no_grad_vars) {
  std::unordered_map<std::string, std::vector<size_t>> dup_out_ops;
  size_t grad_desc_idx = 0;
  std::deque<std::unique_ptr<OpDescBind>> op_descs = block_desc.ops_;
  std::vector<std::unique_ptr<OpDescBind>> grad_op_descs;
  for (auto it = op_descs.rbegin(); it != op_descs.rend(); ++it) {
    std::vector<OpDescBind> op_grads = CreatBackwardOps(*it, no_grad_vars);
    for (const OpDescBind& desc : op_grads) {
      for (const std::string& out_name : desc.OutputArugumentNames()) {
        dup_out_ops[out_name].emplace_back(grad_desc_idx);
      }
      ++grad_desc_idx;
    }
    grad_op_descs.insert(grad_op_descs.end(), op_grads.begin(), op_grads.end());
  }
  // Check whether some variables are written more than once
  std::list<std::pair<size_t, OpDescBind>> pending_sum_ops;
  for (const auto& dup : dup_out_ops) {
    const std::string& out_name = dup.first;
    const std::vector<size_t> dup_op = dup.second;
    if (out_name != kEmptyVarName && dup_op.size() > 1) {
      std::vector<std::string> sum_op_inputs;
      for (size_t i = 0; i < dup_op.size(); ++i) {
        std::string new_name = out_name + "@RENAME@" + std::to_string(i);
        grad_op_descs[dup_op[i]].Rename(out_name, new_name);
        sum_op_inputs.emplace_back(new_name);
      }
      pending_sum_ops.push_back(
          {dup_op.back(),
           OpDescBind(
               {"sum", {{"X", {sum_op_inputs}}}, {{"Out", {out_name}}}, {}})});
    }
  }
  pending_sum_ops.sort(
      [](const std::pair<size_t, OpDescBind>& a,
         const std::pair<size_t, OpDescBind>& b) { return a.first > b.first; });
  for (auto& p : pending_sum_ops) {
    grad_op_descs.insert(grad_op_descs.begin() + p.first + 1,
                         std::move(p.second));
  }
  // Append grad_op_descs to BlockDescBind::ops_
  for () {
  }
}

}  // namespace framework
}  // namespace paddle
