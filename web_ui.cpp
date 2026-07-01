#include "web_ui.h"
#include "ai_interface.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

WebUI::WebUI(AIInterface &ai, int port)
    : ai(ai), portNumber(port), running(false), serverSocket(-1)
{
}

WebUI::~WebUI()
{
    stop();
}

int WebUI::port() const
{
    return portNumber;
}

void WebUI::start()
{
    if (running.load())
        return;

    running.store(true);
    serverThread = std::thread(&WebUI::runServer, this);
}

void WebUI::stop()
{
    if (!running.load())
        return;

    running.store(false);
    if (serverSocket >= 0)
        close(serverSocket);
    if (serverThread.joinable())
        serverThread.join();
}

void WebUI::runServer()
{
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0)
    {
        std::cerr << "Failed to create server socket\n";
        running.store(false);
        return;
    }

    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(portNumber);

    if (bind(serverSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        std::cerr << "Failed to bind server socket\n";
        close(serverSocket);
        serverSocket = -1;
        running.store(false);
        return;
    }

    if (listen(serverSocket, 10) < 0)
    {
        std::cerr << "Failed to listen on server socket\n";
        close(serverSocket);
        serverSocket = -1;
        running.store(false);
        return;
    }

    while (running.load())
    {
        int client = accept(serverSocket, nullptr, nullptr);
        if (client < 0)
        {
            if (!running.load())
                break;
            continue;
        }

        handleClient(client);
        close(client);
    }
}

std::string WebUI::readRequest(int clientSocket)
{
    std::string request;
    char buffer[1024];
    ssize_t length = 0;
    while ((length = recv(clientSocket, buffer, sizeof(buffer), 0)) > 0)
    {
        request.append(buffer, static_cast<size_t>(length));
        if (request.find("\r\n\r\n") != std::string::npos)
            break;
    }
    return request;
}

void WebUI::sendResponse(int clientSocket, const std::string &status,
                         const std::string &contentType,
                         const std::string &body)
{
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n"
             << "Content-Type: " << contentType << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "\r\n"
             << body;
    std::string out = response.str();
    send(clientSocket, out.c_str(), out.size(), 0);
}

std::string WebUI::handleChatRequest(const std::string &body)
{
    std::string userMessage = extractJsonStringValue(body, "message");
    if (userMessage.empty())
        return "{\"success\":false,\"error\":\"Missing message\"}";

    std::string result;

    // 1. Check for explicit terminal-style raw pass-through prefix
    if (userMessage.rfind("llm ", 0) == 0)
    {
        // Strip the "llm " prefix (4 characters) and talk directly to your working askLLM adapter
        result = ai.askLLM(userMessage.substr(4));
    }
    // 2. Add safe check for general conversational words so the frontend won't break if no tables exist
    else if (userMessage == "hello" || userMessage == "hi" || userMessage == "help")
    {
        result = ai.askLLM(userMessage);
    }
    // 3. Otherwise, treat it as a database prompt and run via your MCP system
    else
    {
        result = ai.processUserInput(userMessage);
    }

    std::string escaped = escapeJsonString(result);
    std::ostringstream json;
    json << "{\"success\":true,\"response\":\"" << escaped << "\"}";
    return json.str();
}
std::string WebUI::handleTablesRequest() const
{
    auto tables = ai.listTables();
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < tables.size(); ++i)
    {
        if (i > 0) json << ",";
        json << "{\"name\":\"" << escapeJsonString(tables[i]) << "\"}";
    }
    json << "]";
    return json.str();
}

std::string WebUI::handleStatsRequest() const
{
    auto tables = ai.listTables();
    std::ostringstream json;
    json << "{"
         << "\"tableCount\":" << tables.size() << ","
         << "\"recordCount\":0"
         << "}";
    return json.str();
}

std::string WebUI::handleQueryRequest(const std::string &body)
{
    std::string query = extractJsonStringValue(body, "query");
    if (query.empty())
        return "{\"success\":false,\"error\":\"Missing query\"}";

    try
    {
        QueryResult result = ai.executeSqlStatement(query);
        std::ostringstream json;
        json << "{\"success\":true,\"message\":\"" << escapeJsonString(result.message) << "\"";
        if (!result.columns.empty())
        {
            json << ",\"columns\":[";
            for (size_t i = 0; i < result.columns.size(); ++i)
            {
                if (i > 0) json << ",";
                json << "\"" << escapeJsonString(result.columns[i]) << "\"";
            }
            json << "]";
        }
        if (!result.rows.empty())
        {
            json << ",\"rows\": [";
            for (size_t i = 0; i < result.rows.size(); ++i)
            {
                if (i > 0) json << ",";
                json << "[";
                for (size_t j = 0; j < result.rows[i].vals.size(); ++j)
                {
                    if (j > 0) json << ",";
                    json << "\"" << escapeJsonString(result.rows[i].vals[j].type == TYPE_INT
                        ? std::to_string(result.rows[i].vals[j].i)
                        : result.rows[i].vals[j].s) << "\"";
                }
                json << "]";
            }
            json << "]";
        }
        json << "}";
        return json.str();
    }
    catch (const std::exception &ex)
    {
        std::ostringstream json;
        json << "{\"success\":false,\"error\":\"" << escapeJsonString(ex.what()) << "\"}";
        return json.str();
    }
}

std::string WebUI::extractJsonStringValue(const std::string &json,
                                          const std::string &key) const
{
    std::string pattern = "\"" + key + "\"";
    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos + pattern.size());
    if (colonPos == std::string::npos) return "";

    size_t quoteStart = json.find('"', colonPos + 1);
    if (quoteStart == std::string::npos) return "";

    size_t quoteEnd = quoteStart + 1;
    while (quoteEnd < json.size())
    {
        if (json[quoteEnd] == '"' && json[quoteEnd - 1] != '\\')
            break;
        quoteEnd++;
    }
    if (quoteEnd >= json.size()) return "";
    return json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
}

std::string WebUI::escapeJsonString(const std::string &value) const
{
    std::ostringstream escaped;
    for (char ch : value)
    {
        switch (ch)
        {
            case '"': escaped << "\\\""; break;
            case '\\': escaped << "\\\\"; break;
            case '\b': escaped << "\\b"; break;
            case '\f': escaped << "\\f"; break;
            case '\n': escaped << "\\n"; break;
            case '\r': escaped << "\\r"; break;
            case '\t': escaped << "\\t"; break;
            default: escaped << ch; break;
        }
    }
    return escaped.str();
}

void WebUI::handleClient(int clientSocket)
{
    std::string request = readRequest(clientSocket);
    if (request.empty())
        return;

    bool isPostChat = request.rfind("POST /chat ", 0) == 0;
    bool isPostQuery = request.rfind("POST /query ", 0) == 0;
    bool isGetTables = request.rfind("GET /tables ", 0) == 0;
    bool isGetStats = request.rfind("GET /stats ", 0) == 0;
    bool isGetRoot = request.rfind("GET / ", 0) == 0;
    bool isGetIndex = request.rfind("GET /index.html ", 0) == 0;

    if (isPostChat || isPostQuery)
    {
        size_t bodyPos = request.find("\r\n\r\n");
        std::string body;
        if (bodyPos != std::string::npos)
            body = request.substr(bodyPos + 4);

        std::string response = isPostChat ? handleChatRequest(body)
                                          : handleQueryRequest(body);
        sendResponse(clientSocket, "200 OK", "application/json", response);
    }
    else if (isGetTables)
    {
        std::string response = handleTablesRequest();
        sendResponse(clientSocket, "200 OK", "application/json", response);
    }
    else if (isGetStats)
    {
        std::string response = handleStatsRequest();
        sendResponse(clientSocket, "200 OK", "application/json", response);
    }
    else if (isGetRoot || isGetIndex)
    {
        std::string page = getIndexPage();
        sendResponse(clientSocket, "200 OK", "text/html", page);
    }
    else
    {
        sendResponse(clientSocket, "404 Not Found", "text/plain", "Not found");
    }
}

std::string WebUI::getIndexPage() const
{
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>CoreDB Dashboard</title>
  <title>CoreDB Dashboard</title>
  <style>
    * {
      margin: 0;
      padding: 0;
      box-sizing: border-box;
    }

    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif;
      background: #f5f7fa;
      display: flex;
      height: 100vh;
      overflow: hidden;
    }

    /* Sidebar Navigation */
    .sidebar {
      width: 250px;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      color: white;
      display: flex;
      flex-direction: column;
      box-shadow: 2px 0 8px rgba(0,0,0,0.1);
      overflow-y: auto;
    }

    .logo {
      padding: 24px;
      border-bottom: 1px solid rgba(255,255,255,0.1);
      font-size: 20px;
      font-weight: 700;
      text-align: center;
      letter-spacing: 1px;
    }

    .nav-items {
      flex: 1;
      padding: 16px 0;
    }

    .nav-item {
      padding: 16px 20px;
      cursor: pointer;
      transition: all 0.3s;
      border-left: 3px solid transparent;
      display: flex;
      align-items: center;
      gap: 12px;
      font-size: 15px;
      user-select: none;
    }

    .nav-item:hover {
      background: rgba(255,255,255,0.1);
    }

    .nav-item.active {
      background: rgba(255,255,255,0.2);
      border-left-color: #fff;
    }

    .nav-icon {
      font-size: 18px;
    }

    /* Main Content */
    .main-content {
      flex: 1;
      display: flex;
      flex-direction: column;
      overflow: hidden;
    }

    .header {
      background: white;
      padding: 24px;
      border-bottom: 1px solid #e0e0e0;
      box-shadow: 0 2px 4px rgba(0,0,0,0.05);
    }

    .header h1 {
      font-size: 28px;
      color: #333;
      margin-bottom: 8px;
    }

    .header p {
      color: #999;
      font-size: 14px;
    }

    .content {
      flex: 1;
      overflow-y: auto;
      padding: 24px;
    }

    .section {
      display: none;
    }

    .section.active {
      display: block;
      animation: fadeIn 0.3s ease-in;
    }

    @keyframes fadeIn {
      from { opacity: 0; }
      to { opacity: 1; }
    }

    /* Dashboard Cards */
    .stats-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
      gap: 20px;
      margin-bottom: 32px;
    }

    .stat-card {
      background: white;
      padding: 24px;
      border-radius: 12px;
      box-shadow: 0 2px 8px rgba(0,0,0,0.08);
      border-left: 4px solid #667eea;
    }

    .stat-card.blue { border-left-color: #667eea; }
    .stat-card.green { border-left-color: #48bb78; }
    .stat-card.orange { border-left-color: #ed8936; }
    .stat-card.red { border-left-color: #f56565; }

    .stat-label {
      font-size: 14px;
      color: #999;
      margin-bottom: 8px;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }

    .stat-value {
      font-size: 32px;
      font-weight: 700;
      color: #333;
    }

    /* Tables Section */
    .table-list {
      display: grid;
      gap: 16px;
    }

    .table-item {
      background: white;
      padding: 20px;
      border-radius: 12px;
      box-shadow: 0 2px 8px rgba(0,0,0,0.08);
      display: flex;
      justify-content: space-between;
      align-items: center;
    }

    .table-info h3 {
      color: #333;
      margin-bottom: 4px;
    }

    .table-info p {
      color: #999;
      font-size: 14px;
    }

    .table-actions {
      display: flex;
      gap: 8px;
    }

    .btn {
      padding: 8px 16px;
      border: none;
      border-radius: 8px;
      font-size: 14px;
      cursor: pointer;
      transition: all 0.3s;
      font-weight: 600;
    }

    .btn-primary {
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      color: white;
    }

    .btn-primary:hover {
      transform: translateY(-2px);
      box-shadow: 0 4px 12px rgba(102, 126, 234, 0.4);
    }

    .btn-secondary {
      background: #f0f0f0;
      color: #333;
    }

    .btn-secondary:hover {
      background: #e0e0e0;
    }

    /* Query Console */
    .query-editor {
      background: white;
      border-radius: 12px;
      box-shadow: 0 2px 8px rgba(0,0,0,0.08);
      overflow: hidden;
      display: flex;
      flex-direction: column;
      height: calc(100vh - 240px);
    }

    .editor-toolbar {
      background: #f8f8f8;
      padding: 16px;
      border-bottom: 1px solid #e0e0e0;
      display: flex;
      gap: 8px;
    }

    .editor-input {
      flex: 1;
      padding: 16px;
      border: none;
      font-family: 'Monaco', 'Menlo', 'Ubuntu Mono', monospace;
      font-size: 14px;
      resize: none;
      outline: none;
    }

    .query-results {
      flex: 1;
      padding: 16px;
      overflow-y: auto;
      background: #fafafa;
      border-top: 1px solid #e0e0e0;
      display: none;
    }

    .query-results.show {
      display: block;
    }

    .results-table {
      width: 100%;
      border-collapse: collapse;
      background: white;
    }

    .results-table th {
      background: #f0f0f0;
      padding: 12px;
      text-align: left;
      font-weight: 600;
      border-bottom: 2px solid #e0e0e0;
    }

    .results-table td {
      padding: 12px;
      border-bottom: 1px solid #e0e0e0;
    }

    /* Chat Section */
    .chat-container {
      display: flex;
      flex-direction: column;
      height: calc(100vh - 200px);
      background: white;
      border-radius: 12px;
      box-shadow: 0 2px 8px rgba(0,0,0,0.08);
      overflow: hidden;
    }

    .chat-messages {
      flex: 1;
      overflow-y: auto;
      padding: 24px;
      display: flex;
      flex-direction: column;
      gap: 12px;
    }

    .chat-message {
      display: flex;
      animation: slideIn 0.3s ease-out;
      max-width: 85%;
    }

    @keyframes slideIn {
      from {
        opacity: 0;
        transform: translateY(10px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }

    .chat-message.user {
      justify-content: flex-end;
      align-self: flex-end;
    }

    .chat-message.bot {
      justify-content: flex-start;
      align-self: flex-start;
    }

    .chat-bubble {
      padding: 12px 16px;
      border-radius: 12px;
      word-wrap: break-word;
      line-height: 1.5;
      font-size: 15px;
    }

    .chat-message.user .chat-bubble {
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      color: white;
      border-bottom-right-radius: 4px;
    }

    .chat-message.bot .chat-bubble {
      background: #f0f0f0;
      color: #333;
      border-bottom-left-radius: 4px;
    }

    .chat-input-area {
      border-top: 1px solid #e0e0e0;
      padding: 16px;
      background: #fafafa;
      display: flex;
      gap: 8px;
    }

    .chat-input {
      flex: 1;
      border: 1px solid #d0d0d0;
      border-radius: 24px;
      padding: 12px 16px;
      font-size: 15px;
      font-family: inherit;
      resize: none;
      max-height: 100px;
    }

    .chat-input:focus {
      outline: none;
      border-color: #667eea;
      box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.1);
    }

    /* Empty State */
    .empty-state {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      height: 100%;
      color: #999;
    }

    .empty-state-icon {
      font-size: 48px;
      margin-bottom: 16px;
    }

    .empty-state p {
      font-size: 16px;
      margin-bottom: 8px;
    }

    .empty-state-hint {
      font-size: 14px;
      color: #ccc;
    }

    @media (max-width: 1024px) {
      .sidebar {
        width: 200px;
      }

      .stats-grid {
        grid-template-columns: repeat(2, 1fr);
      }
    }

    @media (max-width: 768px) {
      body {
        flex-direction: column;
      }

      .sidebar {
        width: 100%;
        flex-direction: row;
        height: auto;
      }

      .nav-items {
        display: flex;
        flex: 1;
      }

      .nav-item {
        flex: 1;
        justify-content: center;
        padding: 12px;
      }

      .main-content {
        height: calc(100vh - 60px);
      }

      .stats-grid {
        grid-template-columns: 1fr;
      }
    }
  </style>
</head>
<body>
  <div class="sidebar">
    <div class="logo">🗄️ CoreDB</div>
    <div class="nav-items">
      <div class="nav-item active" onclick="switchSection('dashboard')">
        <span class="nav-icon">📊</span> Dashboard
      </div>
      <div class="nav-item" onclick="switchSection('tables')">
        <span class="nav-icon">📋</span> Tables
      </div>
      <div class="nav-item" onclick="switchSection('query')">
        <span class="nav-icon">⚡</span> Query
      </div>
      <div class="nav-item" onclick="switchSection('chat')">
        <span class="nav-icon">💬</span> Assistant
      </div>
    </div>
  </div>

  <div class="main-content">
    <div class="header">
      <h1 id="section-title">Dashboard</h1>
      <p id="section-desc">Overview of your database</p>
    </div>

    <div class="content">
      <!-- Dashboard Section -->
      <div id="dashboard" class="section active">
        <div class="stats-grid">
          <div class="stat-card blue">
            <div class="stat-label">Database Size</div>
            <div class="stat-value">128 KB</div>
          </div>
          <div class="stat-card green">
            <div class="stat-label">Tables</div>
            <div class="stat-value" id="table-count">0</div>
          </div>
          <div class="stat-card orange">
            <div class="stat-label">Total Records</div>
            <div class="stat-value" id="record-count">0</div>
          </div>
          <div class="stat-card red">
            <div class="stat-label">Uptime</div>
            <div class="stat-value">42h</div>
          </div>
        </div>
        <div style="background: white; padding: 24px; border-radius: 12px; box-shadow: 0 2px 8px rgba(0,0,0,0.08);">
          <h3 style="margin-bottom: 16px; color: #333;">Features</h3>
          <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 16px;">
            <div style="padding: 16px; background: #f8f8f8; border-radius: 8px; text-align: center;">
              <div style="font-size: 24px; margin-bottom: 8px;">⚙️</div>
              <div style="font-weight: 600; color: #333;">Transaction Support</div>
              <div style="font-size: 12px; color: #999; margin-top: 4px;">ACID semantics</div>
            </div>
            <div style="padding: 16px; background: #f8f8f8; border-radius: 8px; text-align: center;">
              <div style="font-size: 24px; margin-bottom: 8px;">🤖</div>
              <div style="font-weight: 600; color: #333;">AI Assistant</div>
              <div style="font-size: 12px; color: #999; margin-top: 4px;">Natural language</div>
            </div>
            <div style="padding: 16px; background: #f8f8f8; border-radius: 8px; text-align: center;">
              <div style="font-size: 24px; margin-bottom: 8px;">💾</div>
              <div style="font-weight: 600; color: #333;">Write-Ahead Log</div>
              <div style="font-size: 12px; color: #999; margin-top: 4px;">Durability guarantee</div>
            </div>
            <div style="padding: 16px; background: #f8f8f8; border-radius: 8px; text-align: center;">
              <div style="font-size: 24px; margin-bottom: 8px;">🌳</div>
              <div style="font-weight: 600; color: #333;">B-Tree Indexes</div>
              <div style="font-size: 12px; color: #999; margin-top: 4px;">Fast lookups</div>
            </div>
          </div>
        </div>
      </div>

      <!-- Tables Section -->
      <div id="tables" class="section">
        <div class="table-list" id="tables-list">
          <div class="empty-state">
            <div class="empty-state-icon">📭</div>
            <p>No tables yet</p>
            <div class="empty-state-hint">Create a table using the Query console</div>
          </div>
        </div>
      </div>

      <!-- Query Console Section -->
      <div id="query" class="section">
        <div class="query-editor">
          <div class="editor-toolbar">
            <button class="btn btn-primary" onclick="executeQuery()">▶ Execute</button>
            <button class="btn btn-secondary" onclick="clearQuery()">Clear</button>
            <span style="margin-left: auto; color: #999; font-size: 14px;" id="query-status"></span>
          </div>
          <textarea class="editor-input" id="query-input" placeholder="SELECT * FROM users;&#10;CREATE TABLE products (id INT, name TEXT);&#10;INSERT INTO orders VALUES (1, 'pending');"></textarea>
          <div class="query-results" id="query-results"></div>
        </div>
      </div>

      <!-- Chat Section -->
      <div id="chat" class="section">
        <div class="chat-container">
          <div class="chat-messages" id="chat-messages">
            <div class="empty-state">
              <div class="empty-state-icon">💬</div>
              <p>Start chatting with CoreDB AI Assistant</p>
              <div class="empty-state-hint">Ask about your database or get SQL help</div>
            </div>
          </div>
          <div class="chat-input-area">
            <textarea class="chat-input" id="chat-input" placeholder="Ask me anything..." rows="1"></textarea>
            <button class="btn btn-primary" onclick="sendChatMessage()" style="min-width: 80px;">Send</button>
          </div>
        </div>
      </div>
    </div>
  </div>

  <script>
    let firstChatMessage = true;

    function switchSection(section) {
      // Hide all sections
      document.querySelectorAll('.section').forEach(s => s.classList.remove('active'));
      document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));

      // Show selected section
      document.getElementById(section).classList.add('active');
      event.target.closest('.nav-item').classList.add('active');

      // Update header
      const titles = {
        dashboard: 'Dashboard',
        tables: 'Tables',
        query: 'Query Console',
        chat: 'AI Assistant'
      };
      const descs = {
        dashboard: 'Overview of your database',
        tables: 'Browse your database tables',
        query: 'Execute SQL queries',
        chat: 'Chat with CoreDB AI Assistant'
      };
      document.getElementById('section-title').textContent = titles[section];
      document.getElementById('section-desc').textContent = descs[section];
    }

    function executeQuery() {
      const query = document.getElementById('query-input').value.trim();
      if (!query) return;

      const status = document.getElementById('query-status');
      const results = document.getElementById('query-results');
      status.textContent = 'Executing...';
      results.classList.remove('show');
      results.innerHTML = '';

      fetch('/query', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ query })
      })
        .then(res => res.json())
        .then(data => {
          if (!data.success) {
            status.textContent = 'Error executing query';
            results.innerHTML = '<div class="empty-state"><p style="color:#c33;">' + escapeHtml(data.error) + '</p></div>';
            results.classList.add('show');
            return;
          }

          status.textContent = 'Query executed successfully';
          if (data.columns && data.rows) {
            let html = '<table class="results-table"><tr>';
            data.columns.forEach(col => {
              html += '<th>' + escapeHtml(col) + '</th>';
            });
            html += '</tr>';
            data.rows.forEach(row => {
              html += '<tr>';
              row.forEach(cell => {
                html += '<td>' + escapeHtml(cell) + '</td>';
              });
              html += '</tr>';
            });
            html += '</table>';
            results.innerHTML = html;
          } else {
            results.innerHTML = '<div class="empty-state"><p>' + escapeHtml(data.message) + '</p></div>';
          }
          results.classList.add('show');
        })
        .catch(err => {
          status.textContent = 'Execution failed';
          results.innerHTML = '<div class="empty-state"><p style="color:#c33;">Network error</p></div>';
          results.classList.add('show');
        });
    }

    function clearQuery() {
      document.getElementById('query-input').value = '';
      document.getElementById('query-results').classList.remove('show');
      document.getElementById('query-status').textContent = '';
    }

    async function sendChatMessage() {
      const input = document.getElementById('chat-input');
      const text = input.value.trim();
      if (!text) return;

      const messagesDiv = document.getElementById('chat-messages');

      if (firstChatMessage) {
        messagesDiv.innerHTML = '';
        firstChatMessage = false;
      }

      const userMsg = document.createElement('div');
      userMsg.className = 'chat-message user';
      userMsg.innerHTML = '<div class="chat-bubble">' + escapeHtml(text) + '</div>';
      messagesDiv.appendChild(userMsg);

      input.value = '';
      input.style.height = 'auto';
      input.focus();

      const botMsg = document.createElement('div');
      botMsg.className = 'chat-message bot';
      botMsg.innerHTML = '<div class="chat-bubble">Thinking...</div>';
      messagesDiv.appendChild(botMsg);

      messagesDiv.scrollTop = messagesDiv.scrollHeight;

      try {
        const res = await fetch('/chat', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ message: text })
        });

        const data = await res.json();

        if (data.success) {
          botMsg.innerHTML = '<div class="chat-bubble">' + escapeHtml(data.response) + '</div>';
        } else {
          botMsg.innerHTML = '<div class="chat-bubble" style="background:#fee; color:#c33;">Error: ' + escapeHtml(data.error || 'Unknown error') + '</div>';
        }
      } catch (err) {
        botMsg.innerHTML = '<div class="chat-bubble" style="background:#fee; color:#c33;">Error: Network request failed</div>';
      }

      messagesDiv.scrollTop = messagesDiv.scrollHeight;
    }

    function escapeHtml(text) {
      const div = document.createElement('div');
      div.textContent = text;
      return div.innerHTML;
    }

    function loadDashboardData() {
      fetch('/stats')
        .then(res => res.json())
        .then(data => {
          document.getElementById('table-count').textContent = data.tableCount || 0;
          document.getElementById('record-count').textContent = data.recordCount || 0;
        })
        .catch(() => {
          document.getElementById('table-count').textContent = '-';
          document.getElementById('record-count').textContent = '-';
        });

      fetch('/tables')
        .then(res => res.json())
        .then(data => {
          const list = document.getElementById('tables-list');
          if (!Array.isArray(data) || data.length === 0) {
            list.innerHTML = '<div class="empty-state"><div class="empty-state-icon">📭</div><p>No tables yet</p><div class="empty-state-hint">Create a table using the Query console</div></div>';
            return;
          }

          let html = '';
          data.forEach(table => {
            html += '<div class="table-item"><div class="table-info"><h3>' + escapeHtml(table.name) + '</h3><p>' + escapeHtml(table.name) + ' is ready to query</p></div><div class="table-actions"><button class="btn btn-secondary" onclick="selectTable(\'' + escapeHtml(table.name) + '\')">Open</button></div></div>';
          });
          list.innerHTML = html;
        })
        .catch(() => {
          document.getElementById('tables-list').innerHTML = '<div class="empty-state"><p style="color:#c33;">Could not load tables</p></div>';
        });
    }

    function selectTable(tableName) {
      switchSection('query');
      const queryInput = document.getElementById('query-input');
      queryInput.value = 'SELECT * FROM ' + tableName + ';';
      queryInput.focus();
    }

    // Chat input auto-resize
    const chatInput = document.getElementById('chat-input');
    chatInput.addEventListener('input', function() {
      this.style.height = 'auto';
      this.style.height = Math.min(this.scrollHeight, 100) + 'px';
    });

    chatInput.addEventListener('keypress', function(e) {
      if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        sendChatMessage();
      }
    });

    document.addEventListener('DOMContentLoaded', loadDashboardData);
  </script>
</body>
</html>)HTML";
}
