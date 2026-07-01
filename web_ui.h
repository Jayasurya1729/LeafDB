#pragma once

#include <atomic>
#include <thread>

class AIInterface;

class WebUI {
public:
    explicit WebUI(AIInterface &ai, int port = 8080);
    ~WebUI();

    void start();
    void stop();
    int port() const;

private:
    void runServer();
    void handleClient(int clientSocket);
    std::string readRequest(int clientSocket);
    void sendResponse(int clientSocket, const std::string &status,
                      const std::string &contentType,
                      const std::string &body);
    std::string handleChatRequest(const std::string &body);
    std::string handleTablesRequest() const;
    std::string handleStatsRequest() const;
    std::string handleQueryRequest(const std::string &body);
    std::string extractJsonStringValue(const std::string &json,
                                       const std::string &key) const;
    std::string escapeJsonString(const std::string &value) const;
    std::string getIndexPage() const;

    AIInterface &ai;
    int portNumber;
    std::thread serverThread;
    std::atomic<bool> running;
    int serverSocket;
};
