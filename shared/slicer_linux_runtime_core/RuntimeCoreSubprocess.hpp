#pragma once

#include "RuntimeCoreTypes.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace SlicerLinuxRuntimeCore {

class RuntimeSubprocess {
public:
    RuntimeSubprocess();
    ~RuntimeSubprocess();

    bool start(const LaunchSpec& spec);
    bool running() const;
    void stop();

    nlohmann::json request(const std::string& method, const nlohmann::json& payload);
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    mutable std::mutex m_mutex;
    std::string m_last_error;
    std::uint64_t m_next_id{1};
};

}
