#pragma once

#include "RuntimeCoreTypes.hpp"

#include <deque>

namespace SlicerLinuxRuntimeCore {

class CallbackQueue {
public:
    void push(RuntimeEvent event);
    bool try_pop(RuntimeEvent& event);
    std::vector<RuntimeEvent> drain();
    std::size_t size() const;
    void clear();

private:
    mutable std::mutex m_mutex;
    std::deque<RuntimeEvent> m_queue;
};

}
