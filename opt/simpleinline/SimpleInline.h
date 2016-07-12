/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "InlineHelper.h"
#include "Pass.h"
#include "DexClass.h"
#include "Resolver.h"
#include "Transform.h"

#include <unordered_map>
#include <unordered_set>

class SimpleInlinePass : public Pass {
public:
  SimpleInlinePass() : Pass("SimpleInlinePass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("virtual", true, m_virtual_inline);
    pc.get("try_catch", false, m_inliner_config.try_catch_inline);
    pc.get("callee_invoke_direct",
           false,
           m_inliner_config.callee_direct_invoke_inline);
    pc.get("virtual_same_class",
           false,
           m_inliner_config.virtual_same_class_inline);
    pc.get("no_inline_annos", {}, m_no_inline_annos);
  }

  virtual void run_pass(DexClassesVector&, ConfigFiles&, PassManager&) override;

private:
  std::unordered_set<DexMethod*> gather_non_virtual_methods(
      Scope& scope, const std::unordered_set<DexType*>& no_inline);
  void select_single_called(
      Scope& scope, std::unordered_set<DexMethod*>& methods);

private:
  // count of instructions that define a method as inlinable always
  static const size_t SMALL_CODE_SIZE = 3;

  // inline virtual methods
  bool m_virtual_inline;

  // inline methods with try-catch
  MultiMethodInliner::Config m_inliner_config;

  // annotations indicating not to inline a function
  std::vector<std::string> m_no_inline_annos;

  // set of inlinable methods
  std::unordered_set<DexMethod*> inlinable;

  // keep a map from refs to defs or nullptr if no method was found
  MethodRefCache resolved_refs;
};
