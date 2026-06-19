#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <boost/filesystem/path.hpp>

namespace Slic3r::SlicerLinuxRuntime {

bool enabled();
bool use_linux_runtime();
bool source_module_uses_linux_runtime();
bool should_select_linux_component_package(const std::string& component_name);
const char* linux_component_package_os_type();

std::string runtime_module_stem();
std::string runtime_module_file_name();
std::string runtime_library_path(const std::filesystem::path& component_folder);
std::string runtime_library_path(const boost::filesystem::path& component_folder);

std::string linux_component_library_name();
std::string linux_source_library_name();
std::string host_executable_file_name();
std::string mac_host_wrapper_file_name();
std::string mac_lima_instance_file_name();
std::string mac_runtime_install_script_file_name();
std::string mac_runtime_verify_script_file_name();
std::string windows_wsl_distro_file_name();
std::string windows_wsl_import_script_file_name();
std::string windows_wsl_validate_script_file_name();
std::string windows_wsl_bootstrap_script_file_name();
std::string windows_wsl_rootfs_file_name();
std::string windows_component_cache_subdir_file_name();

bool is_linux_component_package_filename(const std::string& file_name);
bool is_overlay_runtime_filename(const std::string& file_name);
bool validate_linux_so_binary(const std::string& file_path, std::string* reason = nullptr);
std::string linux_component_manifest_file_name();
std::string linux_component_manifest_path(const std::filesystem::path& component_folder);
std::string linux_component_manifest_path(const boost::filesystem::path& component_folder);
std::string sha256_file_hex(const std::string& file_path, std::string* reason = nullptr);
std::string expected_component_abi_version();
bool validate_linux_component_file(const std::string& file_path, std::string* reason = nullptr);
bool validate_linux_component_file_against_manifest(const std::string& file_path, const std::string& manifest_path, std::string* reason = nullptr);
bool validate_linux_component_set_against_manifest(const std::filesystem::path& component_folder, std::string* reason = nullptr);
bool validate_linux_component_set_against_manifest(const boost::filesystem::path& component_folder, std::string* reason = nullptr);
bool abi_version_matches_expected(const std::string& actual_version, std::string* reason = nullptr);
std::vector<std::string> ota_copy_extensions();

}
