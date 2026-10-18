#ifndef RELIABLE_HTTP_SERVER_H
#define RELIABLE_HTTP_SERVER_H

// Include after TLS-configured httplib.h. The listener's stop() does not cancel
// accepted request reads. Keep a duplicate descriptor per serving connection so
// shutdown can wake stalled/trickling bodies without racing descriptor reuse.
#include <mutex>
#include <set>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <utility>

template<class Base>
class ReliableHttpServer : public Base {
public:
    template<class... Args>
    explicit ReliableHttpServer(Args&&... args) : Base(std::forward<Args>(args)...) {}
    void cancel_connections() {
        std::lock_guard<std::mutex> guard(m_lock);
        m_stopping = true;
        for (int descriptor : m_connections) ::shutdown(descriptor, SHUT_RDWR);
    }
private:
    bool process_and_close_socket(socket_t socket) override {
        const int duplicate = ::fcntl(socket, F_DUPFD_CLOEXEC, 0);
        // Fail closed under descriptor pressure: an untracked trickling request
        // could otherwise outlive shutdown despite the per-read timeout.
        if (duplicate < 0) {
            ::shutdown(socket, SHUT_RDWR);
            ::close(socket);
            return false;
        }
        {
            std::lock_guard<std::mutex> guard(m_lock);
            if (duplicate >= 0) m_connections.insert(duplicate);
            if (m_stopping) ::shutdown(socket, SHUT_RDWR);
        }
        const auto cleanup = [this, duplicate] {
            if (duplicate < 0) return;
            std::lock_guard<std::mutex> guard(m_lock);
            m_connections.erase(duplicate);
            ::close(duplicate);
        };
        try {
            const bool result = Base::process_and_close_socket(socket);
            cleanup();
            return result;
        } catch (...) {
            cleanup();
            throw;
        }
    }
    std::mutex m_lock;
    std::set<int> m_connections;
    bool m_stopping = false;
};
#endif
