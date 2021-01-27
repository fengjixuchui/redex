/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/noncopyable.hpp>
#include <boost/optional.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "androidfw/ResourceTypes.h"

namespace boost {
namespace iostreams {
class mapped_file;
} // namespace iostreams
} // namespace boost

struct RedexMappedFile {
  std::unique_ptr<boost::iostreams::mapped_file> file;
  std::string filename;
  bool read_only;

  RedexMappedFile(std::unique_ptr<boost::iostreams::mapped_file> in_file,
                  std::string in_filename,
                  bool read_only);
  ~RedexMappedFile();

  RedexMappedFile(RedexMappedFile&& other) noexcept;
  RedexMappedFile& operator=(RedexMappedFile&& rhs) noexcept;

  RedexMappedFile(const RedexMappedFile&) = delete;
  RedexMappedFile& operator=(const RedexMappedFile&) = delete;

  const char* const_data() const;
  char* data() const;
  size_t size() const;
};

const char* const ONCLICK_ATTRIBUTE = "android:onClick";

std::string read_entire_file(const std::string& filename);
void write_entire_file(const std::string& filename,
                       const std::string& contents);
RedexMappedFile map_file(const char* path, bool mode_write = false);
size_t write_serialized_data(const android::Vector<char>& cVec,
                             RedexMappedFile f);
void unmap_and_close(RedexMappedFile map);

std::string get_string_attribute_value(const android::ResXMLTree& parser,
                                       const android::String16& attribute_name);
bool has_raw_attribute_value(const android::ResXMLTree& parser,
                             const android::String16& attribute_name,
                             android::Res_value& out_value);

boost::optional<int32_t> get_min_sdk(const std::string& manifest_filename);

int get_int_attribute_or_default_value(const android::ResXMLTree& parser,
                                       const android::String16& attribute_name,
                                       int32_t default_value);

bool has_bool_attribute(const android::ResXMLTree& parser,
                        const android::String16& attribute_name);

bool get_bool_attribute_value(const android::ResXMLTree& parser,
                              const android::String16& attribute_name,
                              bool default_value);

/*
 * These are all the components which may contain references to Java classes in
 * their attributes.
 */
enum class ComponentTag {
  Activity,
  ActivityAlias,
  Provider,
  Receiver,
  Service,
};

/**
 * Indicate the value of the "exported" attribute of a component.
 */
enum class BooleanXMLAttribute {
  True,
  False,
  Undefined,
};

struct ComponentTagInfo {
  ComponentTag tag;
  std::string classname;
  BooleanXMLAttribute is_exported;
  std::string permission;
  std::string protection_level;
  // Not defined on <provider>
  bool has_intent_filters{false};
  // Only defined on <provider>
  std::unordered_set<std::string> authority_classes;

  ComponentTagInfo(ComponentTag tag,
                   const std::string& classname,
                   BooleanXMLAttribute is_exported,
                   std::string permission,
                   std::string protection_level)
      : tag(tag),
        classname(classname),
        is_exported(is_exported),
        permission(std::move(permission)),
        protection_level(std::move(protection_level)) {}
};

struct ManifestClassInfo {
  std::unordered_set<std::string> application_classes;
  std::unordered_set<std::string> instrumentation_classes;
  std::vector<ComponentTagInfo> component_tags;
};

ManifestClassInfo get_manifest_class_info(const std::string& filename);

// For testing only!
std::unordered_set<std::string> extract_classes_from_native_lib(
    const std::string& lib_contents);

std::unordered_set<std::string> get_native_classes(
    const std::string& apk_directory);

std::unordered_set<std::string> get_layout_classes(
    const std::string& apk_directory);

std::unordered_set<std::string> get_xml_files(const std::string& directory);
std::unordered_set<uint32_t> get_xml_reference_attributes(
    const std::string& filename);
// Checks if the file is in a res/raw folder. Such a file won't be considered
// for resource remapping, class name extraction, etc. These files don't follow
// binary XML format, and thus are out of scope for many optimizations.
bool is_raw_resource(const std::string& filename);
int inline_xml_reference_attributes(
    const std::string& filename,
    const std::map<uint32_t, android::Res_value>& id_to_inline_value);
void remap_xml_reference_attributes(
    const std::string& filename,
    const std::map<uint32_t, uint32_t>& kept_to_remapped_ids);

// Iterates through all layouts in the given directory. Adds all class names to
// the output set, and allows for any specified attribute values to be returned
// as well. Attribute names should specify their namespace, if any (so
// android:onClick instead of just onClick)
void collect_layout_classes_and_attributes(
    const std::string& apk_directory,
    const std::unordered_set<std::string>& attributes_to_read,
    std::unordered_set<std::string>& out_classes,
    std::unordered_multimap<std::string, std::string>& out_attributes);

// Same as above, for single file.
void collect_layout_classes_and_attributes_for_file(
    const std::string& file_path,
    const std::unordered_set<std::string>& attributes_to_read,
    std::unordered_set<std::string>& out_classes,
    std::unordered_multimap<std::string, std::string>& out_attributes);

// Convenience method for copying values in a multimap to a set, for a
// particular key.
std::set<std::string> multimap_values_to_set(
    const std::unordered_multimap<std::string, std::string>& map,
    const std::string& key);

// Given the bytes of a binary XML file, replace the entries (if any) in the
// ResStringPool. Writes result to the given Vector output param.
// Returns android::NO_ERROR (0) on success, or one of the corresponding
// android:: error codes for failure conditions/bad input data.
int replace_in_xml_string_pool(
    const void* data,
    const size_t len,
    const std::map<std::string, std::string>& shortened_names,
    android::Vector<char>* out_data,
    size_t* out_num_renamed);

// Replaces all strings in the ResStringPool for the given file with their
// replacements. Writes all changes to disk, clobbering the given file.
// Same return codes as replace_in_xml_string_pool.
int rename_classes_in_layout(
    const std::string& file_path,
    const std::map<std::string, std::string>& shortened_names,
    size_t* out_num_renamed,
    ssize_t* out_size_delta);

/**
 * Follows the reference links for a resource for all configurations.
 * Outputs all the nodes visited, as well as all the string values seen.
 */
void walk_references_for_resource(
    const android::ResTable& table,
    uint32_t resID,
    std::unordered_set<uint32_t>* nodes_visited,
    std::unordered_set<std::string>* leaf_string_values);

std::unordered_set<uint32_t> get_js_resources(
    const std::string& directory,
    const std::vector<std::string>& js_assets_lists,
    const std::map<std::string, std::vector<uint32_t>>& name_to_ids);

std::unordered_set<uint32_t> get_resources_by_name_prefix(
    const std::vector<std::string>& prefixes,
    const std::map<std::string, std::vector<uint32_t>>& name_to_ids);

const int TYPE_INDEX_BIT_SHIFT = 16;

class ResourcesArscFile {
 public:
  ResourcesArscFile(const ResourcesArscFile&) = delete;
  ResourcesArscFile& operator=(const ResourcesArscFile&) = delete;

  android::ResTable res_table;
  android::SortedVector<uint32_t> sorted_res_ids;
  std::map<uint32_t, std::string> id_to_name;
  std::map<std::string, std::vector<uint32_t>> name_to_ids;

  explicit ResourcesArscFile(const std::string& path);
  std::vector<std::string> get_resource_strings_by_name(
      const std::string& res_name);
  void remap_ids(const std::map<uint32_t, uint32_t>& old_to_remapped_ids);
  std::unordered_set<uint32_t> get_types_by_name(
      const std::unordered_set<std::string>& type_names);
  size_t serialize();
  ~ResourcesArscFile();

  size_t get_length() const;

 private:
  RedexMappedFile m_f;
  size_t m_arsc_len;
  bool m_file_closed = false;
};
