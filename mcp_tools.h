#pragma once

#include <string>
#include <vector>
#include <map>

class Executor;

struct MCPRequest {
    std::string toolName;
    std::map<std::string, std::string> arguments;
};

struct MCPResponse {
    bool success;
    std::string result;
};

struct MCPToolDefinition {
    std::string name;
    std::string description;
};

class MCPTools {
private:
    Executor &executor;

    std::string formatSchema() const;
    std::string formatTableDescription(const std::string &tableName) const;
    std::string formatQueryResult(const struct QueryResult &result) const;

public:
    explicit MCPTools(Executor &executor);

    static std::vector<MCPToolDefinition> availableTools();

    MCPResponse execute(const MCPRequest &request);
};
