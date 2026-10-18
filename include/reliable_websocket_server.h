#ifndef RELIABLE_WEBSOCKET_SERVER_H
#define RELIABLE_WEBSOCKET_SERVER_H

#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXSocket.h>
#include <chrono>
#include <condition_variable>
#include <mutex>

// IX's normal stop snapshots clients before joining acceptance. A connection
// worker may not have registered its WebSocket yet. Gate late workers and keep
// closing newly registered sockets until all accepted workers have returned.
// Only the application lifecycle owner may call shutdown()/start().
class ReliableWebSocketServer : public ix::WebSocketServer {
public:
    ReliableWebSocketServer(int port, const std::string& host)
        : ix::WebSocketServer(port, host, 128, 64, 1,
                              ix::SocketServer::kDefaultAddressFamily, -1, 1) {}

    void shutdown() {
        {
            std::lock_guard<std::mutex> guard(m_lock);
            m_stopping = true;
        }
        stopAcceptingConnections();
        for (;;) {
            for (const auto& client : getClients()) client->close();
            std::unique_lock<std::mutex> guard(m_lock);
            if (m_workers == 0) break;
            m_changed.wait_for(guard, std::chrono::milliseconds(5));
        }
        // Late accepted threads see m_stopping before entering handleUpgrade.
        ix::WebSocketServer::stop();
    }

private:
    void handleConnection(std::unique_ptr<ix::Socket> socket,
                          std::shared_ptr<ix::ConnectionState> state) override {
        {
            std::lock_guard<std::mutex> guard(m_lock);
            if (m_stopping) {
                socket->close();
                state->setTerminated();
                return;
            }
            ++m_workers;
        }
        try {
            handleUpgrade(std::move(socket), state);
        } catch (const std::exception& error) {
            logError(std::string("Connection terminated: ") + error.what());
        } catch (...) {
            logError("Connection terminated by an exception");
        }
        state->setTerminated();
        {
            std::lock_guard<std::mutex> guard(m_lock);
            --m_workers;
        }
        m_changed.notify_all();
    }
    std::mutex m_lock;
    std::condition_variable m_changed;
    bool m_stopping = false;
    unsigned m_workers = 0;
};
#endif
