#include <iostream>
#include <csignal>
#include "sql.h"
#include "executor.h"
#include "ai_interface.h"
#include "web_ui.h"

using namespace std;

static Executor *g_executor = nullptr;

// Signal handler: save data cleanly on Ctrl+C or kill.
static void handleShutdown(int)
{
    if (g_executor)
        g_executor->shutdown();
    std::_Exit(0);
}

static void printValue(const Value &v)
{
    if (v.type == TYPE_INT)    cout << v.i;
    else if (v.type == TYPE_STRING) cout << v.s;
}

static void printRecord(const Record &rec)
{
    for (size_t i = 0; i < rec.vals.size(); i++)
    {
        if (i > 0) cout << " | ";
        printValue(rec.vals[i]);
    }
    cout << "\n";
}

static void printSqlResult(const QueryResult &result)
{
    if (result.success)
    {
        cout << "OK: " << result.message << "\n";
        if (!result.rows.empty())
        {
            cout << "Results:\n";
            if (!result.columns.empty())
            {
                cout << "  ";
                for (size_t i = 0; i < result.columns.size(); i++)
                {
                    if (i > 0) cout << " | ";
                    cout << result.columns[i];
                }
                cout << "\n";
            }
            for (const auto &row : result.rows)
            {
                cout << "  ";
                printRecord(row);
            }
        }
    }
    else
    {
        cout << "ERROR: " << result.message << "\n";
    }
}

static void printHelp()
{
    cout <<
        "Commands:\n"
        "  exit          Quit the program\n"
        "  mode sql      Switch to SQL mode (default)\n"
        "  mode ai       Switch to natural-language / MCP mode\n"
        "  schema        Show database schema\n"
        "  tools         List available MCP tools\n"
        "  help          Show this help\n"
        "\n"
        "SQL mode examples:\n"
        "  CREATE TABLE users (id INT PRIMARY KEY, name TEXT)\n"
        "  INSERT INTO users VALUES (1, 'Alice')\n"
        "  INSERT INTO users (id, name) VALUES (2, 'Bob')\n"
        "  SELECT * FROM users\n"
        "  SELECT * FROM users WHERE id = 1\n"
        "  UPDATE users SET name = 'Charlie' WHERE id = 1\n"
        "  DELETE FROM users WHERE id = 1\n"
        "  BEGIN\n"
        "  COMMIT\n"
        "  ROLLBACK\n"
        "\n"
        "AI mode examples:\n"
        "  list tables\n"
        "  show all from users\n"
        "  show users where id = 1\n"
        "  describe users\n"
        "  {\"tool\":\"execute_sql\",\"query\":\"SELECT * FROM users\"}\n";
}

int main()
{
    Executor executor;
    g_executor = &executor;
    std::signal(SIGINT,  handleShutdown);
    std::signal(SIGTERM, handleShutdown);

    AIInterface ai(executor);
    WebUI web(ai, 8080);
    web.start();
    string mode = "sql";

    cout << "CoreDB interactive shell\n"
         << "Type 'help' for commands, 'exit' to quit.\n"
         << "Current mode: sql\n"
         << "Web chat UI available at http://localhost:" << web.port() << "\n\n";

    string query;
    while (true)
    {
        cout << (mode == "ai" ? "AI> " : "SQL> ") << flush;
        if (!getline(cin, query))
            break;

        // Trim trailing whitespace / carriage returns
        while (!query.empty() && (query.back() == '\r' || query.back() == ' '))
            query.pop_back();

        if (query == "exit") { executor.shutdown(); break; }
        if (query.empty())   continue;

        if (query == "help")
        {
            printHelp();
            cout << "\n";
            continue;
        }

        if (query == "schema")
        {
            cout << ai.getSchemaContext() << "\n\n";
            continue;
        }

        if (query == "tools")
        {
            for (const auto &tool : MCPTools::availableTools())
                cout << "  " << tool.name << " - " << tool.description << "\n";
            cout << "\n";
            continue;
        }

        if (query == "mode sql" || query == "mode ai")
        {
            mode = query.substr(5);
            cout << "Switched to " << mode << " mode.\n\n";
            continue;
        }

        try
        {
            if (mode == "ai")
            {
                if (query.rfind("llm ", 0) == 0)
                {
                    string resp = ai.askLLM(query.substr(4));
                    cout << resp << "\n";
                }
                else
                {
                    string result = ai.processUserInput(query);
                    cout << result << "\n";
                }
            }
            else
            {
                SqlStatement statement = parseSql(query);
                QueryResult  result    = executor.execute(statement);
                printSqlResult(result);
            }
        }
        catch (const exception &ex)
        {
            cout << "ERROR: " << ex.what() << "\n";
        }

        cout << "\n";
    }

    web.stop();
    executor.shutdown();
    return 0;
}