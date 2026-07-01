#include "mcp_tools.h"
#include "sql.h"
#include "executor.h"
#include "catalog.h"

#include <sstream>
#include <stdexcept>

namespace
{
    std::string valueToString(const Value &v)
    {
        if (v.type == TYPE_INT)
            return std::to_string(v.i);
        return v.s;
    }
}

MCPTools::MCPTools(Executor &exec) : executor(exec) {}

std::vector<MCPToolDefinition> MCPTools::availableTools()
{
    return {
        {"execute_sql", "Run a SQL statement against the database"},
        {"get_schema", "Return the full database schema"},
        {"list_tables", "List all table names"},
        {"describe_table", "Describe columns for a single table (argument: table)"},
    };
}

std::string MCPTools::formatSchema() const
{
    std::ostringstream out;
    auto tables = executor.listTables();

    if (tables.empty())
        return "No tables defined.";

    for (const auto &tableName : tables)
        out << formatTableDescription(tableName) << "\n";

    std::string result = out.str();
    if (!result.empty() && result.back() == '\n')
        result.pop_back();
    return result;
}

std::string MCPTools::formatTableDescription(const std::string &tableName) const
{
    TableMetadata tm = executor.getCatalog().getTable(tableName);
    std::ostringstream out;
    out << "TABLE " << tm.name << " (";

    for (size_t i = 0; i < tm.columns.size(); i++)
    {
        if (i > 0)
            out << ", ";
        out << tm.columns[i].name << " " << tm.columns[i].type;
        if (tm.columns[i].isPrimaryKey)
            out << " PRIMARY KEY";
        if (tm.columns[i].isNotNull)
            out << " NOT NULL";
    }

    out << ")";
    return out.str();
}

std::string MCPTools::formatQueryResult(const QueryResult &result) const
{
    std::ostringstream out;
    out << result.message;

    if (!result.rows.empty())
    {
        out << "\n";
        if (!result.columns.empty())
        {
            out << "  ";
            for (size_t j = 0; j < result.columns.size(); j++)
            {
                if (j > 0)
                    out << " | ";
                out << result.columns[j];
            }
            out << "\n";
        }

        for (size_t i = 0; i < result.rows.size(); i++)
        {
            const Record &row = result.rows[i];
            out << "  ";
            for (size_t j = 0; j < row.vals.size(); j++)
            {
                if (j > 0)
                    out << " | ";
                out << valueToString(row.vals[j]);
            }
            if (i + 1 < result.rows.size())
                out << "\n";
        }
    }

    return out.str();
}

MCPResponse MCPTools::execute(const MCPRequest &request)
{
    try
    {
        if (request.toolName == "execute_sql")
        {
            auto it = request.arguments.find("query");
            if (it == request.arguments.end() || it->second.empty())
                return {false, "Missing query argument"};

            SqlStatement stmt = parseSql(it->second);
            QueryResult res = executor.execute(stmt);
            return {res.success, formatQueryResult(res)};
        }

        if (request.toolName == "get_schema")
            return {true, formatSchema()};

        if (request.toolName == "list_tables")
        {
            auto tables = executor.listTables();
            if (tables.empty())
                return {true, "No tables."};

            std::ostringstream out;
            for (size_t i = 0; i < tables.size(); i++)
            {
                if (i > 0)
                    out << ", ";
                out << tables[i];
            }
            return {true, out.str()};
        }

        if (request.toolName == "describe_table")
        {
            auto it = request.arguments.find("table");
            if (it == request.arguments.end() || it->second.empty())
                return {false, "Missing table argument"};

            return {true, formatTableDescription(it->second)};
        }

        return {false, "Unknown tool: " + request.toolName};
    }
    catch (const std::exception &ex)
    {
        return {false, std::string("Tool error: ") + ex.what()};
    }
}
