/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IODIMetadata.h"

#include <fstream>

#include "DexUtil.h"
#include "Show.h"
#include "StlUtil.h"
#include "Trace.h"

namespace {
// Returns com.foo.Bar. for the DexClass Lcom/foo/Bar;. Note the trailing
// '.'.
std::string pretty_prefix_for_cls(const DexClass* cls) {
  std::string pretty_name = java_names::internal_to_external(cls->str());
  // Include the . separator
  pretty_name.push_back('.');
  return pretty_name;
}
} // namespace

void IODIMetadata::mark_methods(DexStoresVector& scope) {
  // Calculates which methods won't collide with other methods when printed
  // in a stack trace (e.g. due to method overloading or templating).
  // Before IODI we disambiguated stack trace lines by using a proguard mapping
  // file (which relied on the line numbers in the stack trace to pick
  // which specific method should be symbolicated). Now, if we use instruction
  // offsets in stack traces, then we cannot leverage proguard mappings anymore,
  // so we must disable IODI for any methods whose stack trace may be ambiguous.
  //
  // We do this linearly for now because otherwise we need locks

  auto emplace_entry = [&](const std::string& str, DexMethod* m) {
    auto iter = m_iodi_methods.find(str);
    if (iter != m_iodi_methods.end()) {
      auto name_iter = m_method_to_name.find(iter->second);
      if (name_iter != m_method_to_name.end()) {
        m_method_to_name.erase(name_iter);
      }
      iter->second = nullptr;
    } else {
      m_iodi_methods.emplace(str, m);
      m_method_to_name.emplace(m, str);
    }
  };
  for (auto& store : scope) {
    for (auto& classes : store.get_dexen()) {
      for (auto& cls : classes) {
        auto pretty_prefix = pretty_prefix_for_cls(cls);
        // First we need to mark all entries...
        for (DexMethod* m : cls->get_dmethods()) {
          emplace_entry(pretty_prefix + m->str(), m);
        }
        for (DexMethod* m : cls->get_vmethods()) {
          emplace_entry(pretty_prefix + m->str(), m);
        }
      }
    }
  }
  std20::erase_if(m_iodi_methods, [](auto it) {
    if (it->second == nullptr) {
      TRACE(IODI, 3, "[IODI] Method cannot use IODI due to name collisions: %s",
            it->first.c_str());
      return true;
    }
    return false;
  });
}

void IODIMetadata::mark_method_huge(const DexMethod* method) {
  m_huge_methods.insert(method);
}

// Returns whether we can symbolicate using IODI for the given method.
bool IODIMetadata::can_safely_use_iodi(const DexMethod* method) const {
  // We can use IODI if we don't have a collision, if the method isn't virtual
  // and if it isn't too big.
  //
  // It turns out for some methods using IODI isn't beneficial. See
  // comment in emit_instruction_offset_debug_info for more info.
  if (m_huge_methods.count(method) > 0) {
    return false;
  }

  std::string pretty_name;
  {
    auto iter = m_method_to_name.find(method);
    if (iter == m_method_to_name.end()) {
      TRACE(IODI, 4, "[IODI] Warning: didn't find %s in pretty map in %s",
            SHOW(method), __PRETTY_FUNCTION__);
      auto cls = type_class(method->get_class());
      always_assert(cls);
      pretty_name = pretty_prefix_for_cls(cls);
      pretty_name += method->str();
    } else {
      pretty_name = iter->second;
    }
  }
  return m_iodi_methods.find(pretty_name) != m_iodi_methods.end();
}

void IODIMetadata::write(
    const std::string& iodi_metadata_filename,
    const std::unordered_map<DexMethod*, uint64_t>& method_to_id) {
  if (iodi_metadata_filename.empty()) {
    return;
  }
  std::ofstream ofs(iodi_metadata_filename.c_str(),
                    std::ofstream::out | std::ofstream::trunc);
  write(ofs, method_to_id);
}

void IODIMetadata::write(
    std::ostream& ofs,
    const std::unordered_map<DexMethod*, uint64_t>& method_to_id) {
  /*
   * Binary file format
   * {
   *  magic: uint32_t = 0xfaceb001
   *  version: uint32_t = 1
   *  count: uint32_t
   *  zero: uint32_t = 0
   *  single_entries: entry_t[count]
   * }
   * where
   * entry_t = {
   *  klen: uint16_t
   *  method_id: uint64_t
   *  key: char[klen]
   * }
   */
  struct __attribute__((__packed__)) Header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t zero;
  } header = {
      .magic = 0xfaceb001,
      .version = 1,
      .count = 0,
      .zero = 0,
  };
  ofs.write((const char*)&header, sizeof(Header));

  struct __attribute__((__packed__)) EntryHeader {
    uint16_t klen;
    uint64_t method_id;
  } entry_hdr;

  uint32_t count = 0;
  uint32_t huge_count = 0;

  for (const auto& it : m_iodi_methods) {
    if (!can_safely_use_iodi(it.second)) {
      // This will occur if at some point a method was marked as huge during
      // encoding.
      huge_count += 1;
      continue;
    }
    count += 1;
    always_assert_log(count != 0, "Too many entries found, overflowed");
    always_assert(it.first.size() < UINT16_MAX);
    entry_hdr.klen = it.first.size();
    entry_hdr.method_id = method_to_id.at(const_cast<DexMethod*>(it.second));
    ofs.write((const char*)&entry_hdr, sizeof(EntryHeader));
    ofs << it.first;
  }
  // Rewind and write the header now that we know single/dup counts
  ofs.seekp(0);
  header.count = count;
  ofs.write((const char*)&header, sizeof(Header));
  TRACE(IODI, 1,
        "[IODI] Emitted %u entries, %u ignored because they were too big.",
        count, huge_count);
}
