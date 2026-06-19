#include "RuntimeCoreCallbackQueue.hpp"

namespace SlicerLinuxRuntimeCore {

void CallbackQueue::push(RuntimeEvent event)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(std::move(event));
}

bool CallbackQueue::try_pop(RuntimeEvent& event)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty())
        return false;
    event = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

std::vector<RuntimeEvent> CallbackQueue::drain()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<RuntimeEvent> out;
    out.reserve(m_queue.size());
    while (!m_queue.empty()) {
        out.push_back(std::move(m_queue.front()));
        m_queue.pop_front();
    }
    return out;
}

std::size_t CallbackQueue::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

void CallbackQueue::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
}

}
