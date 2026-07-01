#include "ai_interface.h"
#include "executor.h"
#include "sql.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace
{
    std::string trim(const std::string &value)
    {
        size_t start = 0;
        while (start < value.size() &&
               std::isspace(static_cast<unsigned char>(value[start])))
            start++;
        size_t end = value.size();
        while (end > start &&
               std::isspace(static_cast<unsigned char>(value[end - 1])))
            end--;
        return value.substr(start, end - start);
    }

    std::string lowerCopy(const std::string &value)
    {
        std::string result = value;
        for (char &ch : result)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return result;
    }

    // Tokenise natural-language input into lowercase words plus operator tokens.
    std::vector<std::string> tokenizeWords(const std::string &text,
                                           bool preserveCase = false)
    {
        std::vector<std::string> words;
        std::string current;

        for (size_t i = 0; i < text.size(); i++)
        {
            char ch = text[i];
            if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')
            {
                current.push_back(preserveCase
                    ? ch
                    : static_cast<char>(std::tolower(
                          static_cast<unsigned char>(ch))));
            }
            else
            {
                if (!current.empty()) { words.push_back(current); current.clear(); }

                if (ch == '=' || ch == '<' || ch == '>' || ch == '!')
                {
                    std::string op(1, ch);
                    if (i + 1 < text.size() && text[i + 1] == '=')
                    {
                        op.push_back('=');
                        i++;
                    }
                    else if (ch == '<' && i + 1 < text.size() && text[i + 1] == '>')
                    {
                        op.push_back('>');
                        i++;
                    }
                    words.push_back(op);
                }
            }
        }
        if (!current.empty()) words.push_back(current);
        return words;
    }

    bool startsWithKeyword(const std::string &input)
    {
        static const char *keywords[] = {
            "create", "insert", "select", "update", "delete",
            "begin", "commit", "rollback"
        };
        std::string lower = lowerCopy(trim(input));
        for (const char *kw : keywords)
        {
            size_t len = std::strlen(kw);
            if (lower.size() >= len && lower.compare(0, len, kw) == 0)
                if (lower.size() == len ||
                    std::isspace(static_cast<unsigned char>(lower[len])))
                    return true;
        }
        return false;
    }

    bool looksLikeToolJson(const std::string &input)
    {
        std::string t = trim(input);
        return t.size() >= 2 && t.front() == '{' && t.back() == '}';
    }

    // Minimal JSON string-value extractor (no dependency on a JSON library).
    std::string extractJsonStringValue(const std::string &json,
                                       const std::string &key)
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
            if (json[quoteEnd] == '"' && json[quoteEnd - 1] != '\\') break;
            quoteEnd++;
        }
        if (quoteEnd >= json.size()) return "";
        return json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
    }

    MCPRequest parseToolJson(const std::string &input)
    {
        MCPRequest req;
        req.toolName = extractJsonStringValue(input, "tool");
        if (req.toolName.empty())
            throw std::runtime_error("Tool JSON must include a \"tool\" field");

        std::string query = extractJsonStringValue(input, "query");
        if (!query.empty()) req.arguments["query"] = query;

        std::string table = extractJsonStringValue(input, "table");
        if (!table.empty()) req.arguments["table"] = table;

        return req;
    }

    bool containsPhrase(const std::vector<std::string> &words,
                        const std::vector<std::string> &phrase)
    {
        if (phrase.empty() || words.size() < phrase.size()) return false;
        for (size_t i = 0; i + phrase.size() <= words.size(); i++)
        {
            bool match = true;
            for (size_t j = 0; j < phrase.size(); j++)
                if (words[i + j] != phrase[j]) { match = false; break; }
            if (match) return true;
        }
        return false;
    }

    bool containsAllWords(const std::vector<std::string> &words,
                          const std::vector<std::string> &required)
    {
        for (const std::string &word : required)
        {
            bool found = false;
            for (const std::string &candidate : words)
                if (candidate == word) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }

    std::string findKnownTable(const std::vector<std::string> &words,
                               const std::vector<std::string> &tables)
    {
        for (auto it = words.rbegin(); it != words.rend(); ++it)
            for (const std::string &t : tables)
                if (*it == t) return t;
        return "";
    }

    std::string mapOperator(const std::string &word)
    {
        if (word == "equals" || word == "equal" || word == "is") return "=";
        if (word == "greater" || word == "more"  || word == "above") return ">";
        if (word == "less"    || word == "below" || word == "under") return "<";
        if (word == "at"      || word == "least") return ">=";
        if (word == "most")  return "<=";
        if (word == "not")   return "!=";
        return word;
    }

    std::string quoteIfNeeded(const std::string &value)
    {
        if (value.empty()) return "''";
        if (value.front() == '\'' && value.back() == '\'') return value;
        for (char ch : value)
            if (!std::isdigit(static_cast<unsigned char>(ch)) &&
                ch != '-' && ch != '.')
                return "'" + value + "'";
        return value;
    }

    std::string buildWhereClause(const std::vector<std::string> &words,
                                 const std::vector<std::string> &originalWords,
                                 size_t startIndex)
    {
        if (startIndex >= words.size()) return "";

        std::string column = words[startIndex];
        size_t i = startIndex + 1;
        std::string op;

        if (i < words.size() &&
            (words[i] == "=" || words[i] == "!=" || words[i] == "<>" ||
             words[i] == "<" || words[i] == ">"  ||
             words[i] == "<=" || words[i] == ">="))
        {
            op = words[i];
            i++;
        }
        else if (i + 1 < words.size())
        {
            std::string m = mapOperator(words[i]);
            if ((m == ">" || m == "<") &&
                i + 1 < words.size() && words[i + 1] == "than")
            {
                op = m; i += 2;
            }
            else if (m == "=") { op = "="; i++; }
        }

        if (op.empty() || i >= words.size()) return "";

        std::string rawValue = (i < originalWords.size())
                               ? originalWords[i] : words[i];
        std::ostringstream where;
        where << " WHERE " << column << " " << op << " "
              << quoteIfNeeded(rawValue);
        return where.str();
    }

    size_t findWordIndex(const std::vector<std::string> &words,
                         const std::string &target)
    {
        for (size_t i = 0; i < words.size(); i++)
            if (words[i] == target) return i;
        return words.size();
    }

    MCPRequest convertNaturalLanguage(const std::string &input,
                                      const std::vector<std::string> &tables)
    {
        std::vector<std::string> words         = tokenizeWords(input);
        std::vector<std::string> originalWords = tokenizeWords(input, true);

        // --- Meta queries ---
        if (containsPhrase(words, {"list", "tables"}) ||
            containsPhrase(words, {"show", "tables"}) ||
            containsPhrase(words, {"what", "tables"}) ||
            (words.size() == 1 && words[0] == "tables"))
            return {"list_tables", {}};

        if (containsPhrase(words, {"database", "schema"}) ||
            containsPhrase(words, {"show", "schema"})  ||
            containsPhrase(words, {"full", "schema"})  ||
            (words.size() == 1 && words[0] == "schema"))
            return {"get_schema", {}};

        if (containsPhrase(words, {"describe"})      ||
            containsPhrase(words, {"schema", "for"}) ||
            containsPhrase(words, {"columns", "in"}) ||
            containsPhrase(words, {"structure", "of"}))
        {
            std::string table = findKnownTable(words, tables);
            if (!table.empty())
                return {"describe_table", {{"table", table}}};
        }

        // --- Transaction keywords ---
        if (containsPhrase(words, {"begin", "transaction"}) ||
            words == std::vector<std::string>{"begin"})
            return {"execute_sql", {{"query", "BEGIN"}}};

        if (words.size() == 1 && words[0] == "commit")
            return {"execute_sql", {{"query", "COMMIT"}}};

        if (words.size() == 1 && words[0] == "rollback")
            return {"execute_sql", {{"query", "ROLLBACK"}}};

        // --- Resolve table name ---
        std::string table = findKnownTable(words, tables);
        size_t fromIndex  = findWordIndex(words, "from");
        if (fromIndex + 1 < words.size())
            table = words[fromIndex + 1];

        if (table.empty())
            throw std::runtime_error(
                "Could not infer a table name. "
                "Try: \"show all from <table>\" or \"list tables\".");

        // --- DELETE ---
        if (containsPhrase(words, {"delete", "from"}) ||
            (containsPhrase(words, {"delete"}) &&
             containsPhrase(words, {"from", table})))
        {
            size_t whereIndex = findWordIndex(words, "where");
            std::ostringstream sql;
            sql << "DELETE FROM " << table;
            if (whereIndex < words.size())
                sql << buildWhereClause(words, originalWords, whereIndex + 1);
            return {"execute_sql", {{"query", sql.str()}}};
        }

        // --- UPDATE ---
        if (containsPhrase(words, {"update"}))
        {
            size_t setIndex   = findWordIndex(words, "set");
            size_t whereIndex = findWordIndex(words, "where");
            if (setIndex + 2 < words.size())
            {
                std::ostringstream sql;
                sql << "UPDATE " << table
                    << " SET " << words[setIndex + 1] << " = "
                    << quoteIfNeeded(setIndex + 2 < originalWords.size()
                                     ? originalWords[setIndex + 2]
                                     : words[setIndex + 2]);
                if (whereIndex < words.size())
                    sql << buildWhereClause(words, originalWords, whereIndex + 1);
                return {"execute_sql", {{"query", sql.str()}}};
            }
        }

        // --- INSERT (not supported via natural language) ---
        if (containsPhrase(words, {"insert", "into"}) ||
            containsPhrase(words, {"add", "to"}))
            throw std::runtime_error(
                "For inserts use SQL directly, e.g.:\n"
                "  INSERT INTO users (id, name) VALUES (1, 'Alice')");

        // --- SELECT ---
        if (!words.empty() &&
            (containsPhrase(words, {"show", "all"})    ||
             containsPhrase(words, {"list", "all"})    ||
             containsPhrase(words, {"get",  "all"})    ||
             containsPhrase(words, {"find", "all"})    ||
             containsPhrase(words, {"display", "all"}) ||
             words[0] == "show" || words[0] == "list"  ||
             words[0] == "get"  || words[0] == "find"))
        {
            // Recognize "show all tables" and similar without a specific table name.
            if (containsAllWords(words, {"show", "all", "tables"}) ||
                containsAllWords(words, {"list", "tables"}))
            {
                return {"list_tables", {}};
            }

            size_t whereIndex = findWordIndex(words, "where");
            size_t withIndex  = findWordIndex(words, "with");
            std::ostringstream sql;
            sql << "SELECT * FROM " << table;
            if (whereIndex < words.size())
                sql << buildWhereClause(words, originalWords, whereIndex + 1);
            else if (withIndex + 1 < words.size())
                sql << buildWhereClause(words, originalWords, withIndex + 1);
            return {"execute_sql", {{"query", sql.str()}}};
        }

        throw std::runtime_error(
            "Unsupported natural language request. Examples:\n"
            "  list tables\n"
            "  show all from users\n"
            "  show users where age > 25\n"
            "  delete from users where id = 3\n"
            "  describe users");
    }
}

AIInterface::AIInterface(Executor &exec) : executor(exec), tools(exec), llm() {}

std::string AIInterface::getSchemaContext() const
{
    MCPTools schemaTools(executor);
    return schemaTools.execute({"get_schema", {}}).result;
}

QueryResult AIInterface::executeSqlStatement(const std::string &query) const
{
    SqlStatement statement = parseSql(query);
    return executor.execute(statement);
}

std::vector<std::string> AIInterface::listTables() const
{
    return executor.listTables();
}

MCPRequest AIInterface::resolveToolCall(const std::string &input)
{
    std::string trimmed = trim(input);
    if (trimmed.empty())
        throw std::runtime_error("Empty input");

    if (looksLikeToolJson(trimmed))     return parseToolJson(trimmed);
    if (startsWithKeyword(trimmed))     return {"execute_sql", {{"query", trimmed}}};
    return convertNaturalLanguage(trimmed, executor.listTables());
}

std::string AIInterface::processUserInput(const std::string &input)
{
    try
    {
        // 1. Get the schema context so the AI knows your table names
        std::string schemaContext = getSchemaContext();

        // 2. Build the strict prompt
        std::ostringstream prompt;
        prompt << "You are an AI database router. Translate the user's request into an MCP tool call JSON structure.\n\n";
        prompt << "Available Tools:\n";
        prompt << "- execute_sql (arguments: {\"query\": \"SQL_STATEMENT\"})\n";
        prompt << "- list_tables\n";
        prompt << "- get_schema\n\n";
        prompt << "Current Database Schema:\n" << schemaContext << "\n\n";
        prompt << "User request: \"" << input << "\"\n\n";
        prompt << "Respond ONLY with a valid JSON object using the fields \"tool\" and \"arguments\".\n";
        prompt << "Example:\n";
        prompt << "{\n  \"tool\": \"execute_sql\",\n  \"arguments\": {\n    \"query\": \"SELECT * FROM person;\"\n  }\n}";

        // 3. Generate response using your LLM adapter
        std::string resp = llm.generate(prompt.str());

        // 4. Extract the text value from the outer Gemini API envelope
        std::string keyPattern = "\"text\"";
        size_t keyPos = resp.find(keyPattern);
        std::string cleanJson = resp;

        if (keyPos != std::string::npos) {
            size_t colonPos = resp.find(':', keyPos + keyPattern.size());
            if (colonPos != std::string::npos) {
                size_t quoteStart = resp.find('"', colonPos + 1);
                if (quoteStart != std::string::npos) {
                    size_t quoteEnd = quoteStart + 1;
                    while (quoteEnd < resp.size()) {
                        if (resp[quoteEnd] == '"' && resp[quoteEnd - 1] != '\\')
                            break;
                        quoteEnd++;
                    }
                    if (quoteEnd < resp.size()) {
                        cleanJson = resp.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                    }
                }
            }
        }

        // 5. Unescape JSON strings inside the extracted content
        size_t pos = 0;
        while ((pos = cleanJson.find("\\n", pos)) != std::string::npos) {
            cleanJson.replace(pos, 2, "\n");
            pos += 1;
        }
        pos = 0;
        while ((pos = cleanJson.find("\\\"", pos)) != std::string::npos) {
            cleanJson.replace(pos, 2, "\"");
            pos += 1;
        }
        
        // Clean up escaped mathematical signs if any exist
        pos = 0;
        while ((pos = cleanJson.find("\\>", pos)) != std::string::npos) {
            cleanJson.replace(pos, 2, ">");
            pos += 1;
        }
        pos = 0;
        while ((pos = cleanJson.find("\\<", pos)) != std::string::npos) {
            cleanJson.replace(pos, 2, "<");
            pos += 1;
        }

        // 6. NEW BRACE-BASED ISOLATION LAYER
        // This ensures we drop any conversational junk or markdown backticks completely
        size_t startBrace = cleanJson.find('{');
        size_t endBrace = cleanJson.rfind('}');
        
        if (startBrace != std::string::npos && endBrace != std::string::npos && endBrace > startBrace) {
            cleanJson = cleanJson.substr(startBrace, endBrace - startBrace + 1);
        }

        // 7. Pass the absolute clean tool JSON to your parser and execute it!
        MCPRequest  request  = resolveToolCall(cleanJson);
        MCPResponse response = tools.execute(request);
        
        if (!response.success)
        {
            // Conversational fallback in case it's a general question
            if (response.result.find("Unsupported SQL statement") != std::string::npos) {
                return askLLM(input); 
            }
            return "ERROR: " + response.result;
        }
            
        return response.result;
    }
    catch (const std::exception &ex)
    {
        return "ERROR: " + std::string(ex.what());
    }
}
std::string AIInterface::askLLM(const std::string &userPrompt)
{
    try
    {
        std::string schema = getSchemaContext();
        std::ostringstream prompt;
        prompt << "You are a helpful SQL assistant.\n";
        prompt << "Database schema:\n" << schema << "\n";
        prompt << "User question:\n" << userPrompt << "\n";
        prompt << "Provide a concise, actionable answer or an example SQL statement.";

        std::string resp = llm.generate(prompt.str());

        // --- NEW JSON PARSING LOGIC FOR GEMINI ---
        std::string keyPattern = "\"text\"";
        size_t keyPos = resp.find(keyPattern);
        if (keyPos == std::string::npos) return resp; // Fallback to raw response if not found

        size_t colonPos = resp.find(':', keyPos + keyPattern.size());
        if (colonPos == std::string::npos) return resp;

        size_t quoteStart = resp.find('"', colonPos + 1);
        if (quoteStart == std::string::npos) return resp;

        size_t quoteEnd = quoteStart + 1;
        while (quoteEnd < resp.size())
        {
            if (resp[quoteEnd] == '"' && resp[quoteEnd - 1] != '\\')
                break;
            quoteEnd++;
        }
        if (quoteEnd >= resp.size()) return resp;

        // Extract the clean text block
        std::string cleanText = resp.substr(quoteStart + 1, quoteEnd - quoteStart - 1);

        // Process escaped characters like JSON newlines (\n) so they render beautifully
        size_t pos = 0;
        while ((pos = cleanText.find("\\n", pos)) != std::string::npos) {
            cleanText.replace(pos, 2, "\n");
            pos += 1;
        }
        
        // Process escaped tabs (\t) if there are any
        pos = 0;
        while ((pos = cleanText.find("\\t", pos)) != std::string::npos) {
            cleanText.replace(pos, 2, "\t");
            pos += 1;
        }

        // Process escaped quotes (\") 
        pos = 0;
        while ((pos = cleanText.find("\\\"", pos)) != std::string::npos) {
            cleanText.replace(pos, 2, "\"");
            pos += 1;
        }

        return cleanText;
        // ----------------------------------------
    }
    catch (const std::exception &ex)
    {
        return std::string("ERROR: ") + ex.what();
    }
}