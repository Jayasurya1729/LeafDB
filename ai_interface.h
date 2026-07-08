#pragma once

#include <string>
#include <vector>
#include "mcp_tools.h"
#include "executor.h"
#include "llm_adapter.h"

class AIInterface {
private:
    Executor &executor;
    MCPTools tools;
    LLMAdapter llm;

    MCPRequest resolveToolCall(const std::string &input);

public:
    explicit AIInterface(Executor &executor);

    std::string processUserInput(const std::string &input);
    std::string getSchemaContext() const;
    std::vector<std::string> listTables() const;
    QueryResult executeSqlStatement(const std::string &query, const std::string &sessionId = "") const;

    // Ask the configured LLM for a natural language response using schema context.
    std::string askLLM(const std::string &userPrompt);
};
