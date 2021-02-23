/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "oneflow/core/graph/exec_graph.h"
#include "oneflow/core/graph/op_graph.h"

namespace oneflow {

void ExecNode::BindBnWithRegst(const std::string& bn, std::shared_ptr<RegstDesc> regst) {
  CHECK(bn_in_op2regst_.emplace(bn, regst).second);
}

void ExecNode::BindBnsWithRegst(const PbRpf<std::string>& (Operator::*bns_getter)() const,
                                std::shared_ptr<RegstDesc> regst) {
  for (const std::string& bn : (op_.get()->*bns_getter)()) { BindBnWithRegst(bn, regst); }
}

void ExecNode::AddBnToRegstAndBindIt(const PbRpf<std::string>& (Operator::*bns_getter)() const,
                                     std::shared_ptr<RegstDesc> regst) {
  for (const std::string& bn : (op_.get()->*bns_getter)()) { regst->AddLbi(op_->BnInOp2Lbi(bn)); }
  BindBnsWithRegst(bns_getter, regst);
}

bool ExecNode::TryBindBnWithOneOfTheRegsts(const std::string& bn,
                                           const std::list<std::shared_ptr<RegstDesc>>& regsts) {
  const LogicalBlobId& lbi = op()->BnInOp2Lbi(bn);
  bool has_binded = false;
  for (std::shared_ptr<RegstDesc> regst : regsts) {
    if (regst->GetBlobDesc(lbi) == nullptr) { continue; }
    BindBnWithRegst(bn, regst);
    has_binded = true;
    break;
  }
  return has_binded;
}

void ExecNode::BindBnWithOneOfTheRegsts(const std::string& bn,
                                        const std::list<std::shared_ptr<RegstDesc>>& regsts) {
  CHECK(TryBindBnWithOneOfTheRegsts(bn, regsts));
}

void ExecNode::UnbindBnWithEmptyRegst() {
  EraseIf<std::string, std::shared_ptr<RegstDesc>>(
      &bn_in_op2regst_, [](HashMap<std::string, std::shared_ptr<RegstDesc>>::iterator it) {
        return it->second->regst_desc_type().has_data_regst_desc() && it->second->NumOfLbi() == 0;
      });
}

void ExecNode::ToProto(const ParallelContext* parallel_ctx, ExecNodeProto* ret) const {
  const OpNode* op_node = Global<OpGraph>::Get()->OpNode4OpName(op_->op_name());
  const ParallelDesc* parallel_desc = op_node == nullptr ? nullptr : &op_node->parallel_desc();
  const SbpSignature* sbp_signature = op_node == nullptr ? nullptr : &op_node->sbp_signature();
  op_->GenKernelConf(GetBlobDesc4BnInOpFunc(), parallel_ctx, ret->mutable_kernel_conf(),
                     GetLogicalBlobDesc4BnInOpFunc(), parallel_desc, sbp_signature);
  for (const auto& bn_regst : bn_in_op2regst_) {
    const std::string& bn_in_op = bn_regst.first;
    auto regst = bn_regst.second;
    CHECK(regst);
    PbMapPair<std::string, int64_t> pair{bn_in_op, regst->regst_desc_id()};
    CHECK(ret->mutable_bn_in_op2regst_desc_id()->insert(pair).second);
  }
}

void ExecNode::InferBlobDescs(const ParallelContext* parallel_ctx) {
  auto GetBlobDesc4BnInOp = GetBlobDesc4BnInOpFunc();
  const OpNode* op_node = Global<OpGraph>::Get()->OpNode4OpName(op()->op_name());
  const SbpSignature* sbp_signature = nullptr;
  std::shared_ptr<const ParallelDesc> op_parallel_desc;
  if (op_node != nullptr) {
    sbp_signature = &op_node->sbp_signature();
    op_parallel_desc = CHECK_JUST(op()->GetOpParallelDesc());
  }
  if (op_node != nullptr && parallel_ctx->parallel_num() > 1 && sbp_signature != nullptr) {
    for (const auto& ibn : op()->input_bns()) {
      if (*CHECK_JUST(op()->GetParallelDesc4BnInOp(ibn)) == *op_parallel_desc) {
        CHECK_EQ(*CHECK_JUST(
                     GetPhysicalShape(CHECK_JUST(op()->GetLogicalBlobDesc4Ibn(ibn))->shape(),
                                      sbp_signature->bn_in_op2sbp_parallel().at(ibn),
                                      parallel_ctx->parallel_num(), parallel_ctx->parallel_id())),
                 GetBlobDesc4BnInOp(ibn)->shape());
      }
    }
  }
  CHECK_JUST(op_->InferBlobDescsIf(GetBlobDesc4BnInOp, parallel_ctx, sbp_signature));
  if (op_node != nullptr && parallel_ctx->parallel_num() > 1 && sbp_signature != nullptr) {
    for (const auto& obn : op()->output_bns()) {
      if (*CHECK_JUST(op()->GetParallelDesc4BnInOp(obn)) == *op_parallel_desc) {
        CHECK_EQ(*CHECK_JUST(
                     GetPhysicalShape(CHECK_JUST(op()->GetLogicalBlobDesc4Obn(obn))->shape(),
                                      sbp_signature->bn_in_op2sbp_parallel().at(obn),
                                      parallel_ctx->parallel_num(), parallel_ctx->parallel_id())),
                 GetBlobDesc4BnInOp(obn)->shape());
      }
    }
  }
  CHECK_JUST(op_->InferInplaceObn2IbnIf(&mut_inplace_obn2ibn_, &con_inplace_obn2ibn_,
                                        GetBlobDesc4BnInOp, parallel_ctx, sbp_signature));
}

std::function<const BlobDesc&(const std::string&)> ExecNode::GetLogicalBlobDesc4BnInOpFunc() const {
  const OpNode* op_node = Global<OpGraph>::Get()->OpNode4OpName(op()->op_name());
  if (op_node == nullptr) {
    return [](const std::string& bn_in_op) -> const BlobDesc& {
      UNIMPLEMENTED();
      return *(const BlobDesc*)nullptr;
    };
  }
  return [op_node, this](const std::string& bn_in_op) -> const BlobDesc& {
    return op_node->LogicalBlobDesc4Lbi(op()->BnInOp2Lbi(bn_in_op));
  };
}

std::function<BlobDesc*(const std::string&)> ExecNode::GetBlobDesc4BnInOpFunc() const {
  return [this](const std::string& bn_in_op) -> BlobDesc* {
    auto it = bn_in_op2regst_.find(bn_in_op);
    if (it == bn_in_op2regst_.end()) { return nullptr; }
    std::shared_ptr<RegstDesc> regst = it->second;
    CHECK(regst);
    return regst->MutBlobDesc(op()->BnInOp2Lbi(bn_in_op));
  };
}

void ExecGraph::ToExecSequence(const ParallelContext* parallel_ctx, ExecSequence* ret) const {
  TopoForEachNode([&](ExecNode* node) { node->ToProto(parallel_ctx, ret->add_exec_node()); });
}

}  // namespace oneflow
