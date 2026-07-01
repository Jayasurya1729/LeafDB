#include "sql.h"
#include <cctype>
#include <stdexcept>

using namespace std;

namespace
{
    string upperCopy(const string &value)
    {
        string result = value;
        for (char &ch : result)
            ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
        return result;
    }

    string lowerCopy(const string &value)
    {
        string result = value;
        for (char &ch : result)
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        return result;
    }

    bool isIdentifierStart(char ch)
    {
        return isalpha(static_cast<unsigned char>(ch)) || ch == '_';
    }

    bool isIdentifierBody(char ch)
    {
        return isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.';
    }

    bool isNumberToken(const string &token)
    {
        if (token.empty()) return false;
        size_t index = 0;
        if (token[0] == '-')
        {
            if (token.size() == 1) return false;
            index = 1;
        }
        for (; index < token.size(); index++)
            if (!isdigit(static_cast<unsigned char>(token[index])))
                return false;
        return true;
    }

    string stripTrailingSemicolon(const string &sql)
    {
        size_t end = sql.size();
        while (end > 0 && isspace(static_cast<unsigned char>(sql[end - 1])))
            end--;
        if (end > 0 && sql[end - 1] == ';')
            end--;
        while (end > 0 && isspace(static_cast<unsigned char>(sql[end - 1])))
            end--;
        return sql.substr(0, end);
    }

    vector<string> makeTokens(const string &sql)
    {
        vector<string> tokens;
        size_t i = 0;

        while (i < sql.size())
        {
            char ch = sql[i];

            if (isspace(static_cast<unsigned char>(ch))) { i++; continue; }

            if (ch == '(' || ch == ')' || ch == ',' || ch == '*' ||
                ch == '=' || ch == ';')
            {
                tokens.push_back(string(1, ch));
                i++;
                continue;
            }

            if (ch == '!' || ch == '<' || ch == '>')
            {
                if (i + 1 < sql.size() && sql[i + 1] == '=')
                {
                    tokens.push_back(sql.substr(i, 2));
                    i += 2;
                    continue;
                }
                if (ch == '<' && i + 1 < sql.size() && sql[i + 1] == '>')
                {
                    tokens.push_back("<>");
                    i += 2;
                    continue;
                }
                if (ch == '<' || ch == '>')
                {
                    tokens.push_back(string(1, ch));
                    i++;
                    continue;
                }
                throw runtime_error("Unexpected character in SQL: " + string(1, ch));
            }

            if (ch == '\'')
            {
                string value;
                bool closed = false;
                i++;
                while (i < sql.size())
                {
                    // Escaped single quote: ''
                    if (sql[i] == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'')
                    {
                        value.push_back('\'');
                        i += 2;
                        continue;
                    }
                    if (sql[i] == '\'') { i++; closed = true; break; }
                    value.push_back(sql[i]);
                    i++;
                }
                if (!closed)
                    throw runtime_error("Unterminated string literal");
                tokens.push_back("'" + value + "'");
                continue;
            }

            // Negative numbers and plain digits
            if (ch == '-' || isdigit(static_cast<unsigned char>(ch)))
            {
                size_t start = i;
                i++;
                while (i < sql.size() && isdigit(static_cast<unsigned char>(sql[i])))
                    i++;
                tokens.push_back(sql.substr(start, i - start));
                continue;
            }

            if (isIdentifierStart(ch))
            {
                size_t start = i;
                i++;
                while (i < sql.size() && isIdentifierBody(sql[i]))
                    i++;
                tokens.push_back(sql.substr(start, i - start));
                continue;
            }

            throw runtime_error("Unexpected character in SQL: " + string(1, ch));
        }

        return tokens;
    }

    struct Parser
    {
        vector<string> tokens;
        size_t pos = 0;

        explicit Parser(vector<string> values) : tokens(move(values)) {}

        bool eof() const { return pos >= tokens.size(); }

        const string &peek() const
        {
            if (eof()) throw runtime_error("Unexpected end of SQL statement");
            return tokens[pos];
        }

        string consume()
        {
            string value = peek();
            pos++;
            return value;
        }

        bool match(const string &expected)
        {
            if (!eof() && upperCopy(peek()) == upperCopy(expected))
            {
                pos++;
                return true;
            }
            return false;
        }

        void expect(const string &expected)
        {
            if (!match(expected))
                throw runtime_error("Expected '" + expected + "', got '" +
                                    (eof() ? "<end>" : peek()) + "'");
        }

        string expectIdentifier()
        {
            if (eof()) throw runtime_error("Expected identifier, got end of input");
            const string &t = peek();
            if (t == "(" || t == ")" || t == "," || t == "=" ||
                t == "*" || t == ";")
                throw runtime_error("Expected identifier, got '" + t + "'");
            return consume();
        }

        SqlLiteral parseLiteral()
        {
            string token = consume();
            SqlLiteral literal;

            if (token.size() >= 2 && token.front() == '\'' && token.back() == '\'')
            {
                literal.isString    = true;
                literal.stringValue = token.substr(1, token.size() - 2);
                return literal;
            }

            if (isNumberToken(token))
            {
                literal.isString  = false;
                literal.intValue  = stoi(token);
                return literal;
            }

            throw runtime_error("Expected a string or integer literal, got '" + token + "'");
        }

        vector<string> parseIdentifierList()
        {
            vector<string> result;
            if (match("*")) { result.push_back("*"); return result; }
            result.push_back(expectIdentifier());
            while (match(","))
                result.push_back(expectIdentifier());
            return result;
        }

        SqlWhereClause parseWhereClause()
        {
            SqlWhereClause where;
            if (!match("WHERE")) return where;

            where.enabled = true;
            string columnToken = lowerCopy(expectIdentifier());
            size_t dotPos = columnToken.find('.');
            if (dotPos != string::npos)
            {
                where.tableName = columnToken.substr(0, dotPos);
                where.column = columnToken.substr(dotPos + 1);
            }
            else
            {
                where.column = columnToken;
            }
            where.op      = upperCopy(consume());

            if (where.op != "=" && where.op != "!=" && where.op != "<>" &&
                where.op != "<" && where.op != "<=" &&
                where.op != ">" && where.op != ">=" && where.op != "BETWEEN")
            {
                throw runtime_error("Unsupported WHERE operator: " + where.op);
            }

            where.value = parseLiteral();

            if (where.op == "BETWEEN")
            {
                expect("AND");
                where.secondValue    = parseLiteral();
                where.hasSecondValue = true;
            }

            return where;
        }
    };

    SqlStatement parseCreateTable(Parser &parser)
    {
        SqlStatement stmt;
        stmt.type = SqlStatementType::CreateTable;
        parser.expect("TABLE");
        stmt.tableName = lowerCopy(parser.expectIdentifier());
        parser.expect("(");

        do
        {
            SqlColumnDefinition col;
            col.name = lowerCopy(parser.expectIdentifier());
            col.type = upperCopy(parser.expectIdentifier());
            if (col.type == "STRING") col.type = "TEXT";

            while (!parser.eof() &&
                   upperCopy(parser.peek()) != "," && parser.peek() != ")")
            {
                if (parser.match("PRIMARY"))
                {
                    parser.expect("KEY");
                    col.isPrimaryKey = true;
                    col.isNotNull    = true;
                    continue;
                }
                if (parser.match("NOT"))
                {
                    parser.expect("NULL");
                    col.isNotNull = true;
                    continue;
                }
                throw runtime_error("Unexpected column constraint: " + parser.peek());
            }
            stmt.columns.push_back(col);
        }
        while (parser.match(","));

        parser.expect(")");
        return stmt;
    }

    SqlStatement parseInsert(Parser &parser)
    {
        SqlStatement stmt;
        stmt.type      = SqlStatementType::Insert;
        parser.expect("INTO");
        stmt.tableName = lowerCopy(parser.expectIdentifier());

        if (parser.match("("))
        {
            stmt.insertColumns.push_back(lowerCopy(parser.expectIdentifier()));
            while (parser.match(","))
                stmt.insertColumns.push_back(lowerCopy(parser.expectIdentifier()));
            parser.expect(")");
        }

        parser.expect("VALUES");
        parser.expect("(");
        stmt.insertValues.push_back(parser.parseLiteral());
        while (parser.match(","))
            stmt.insertValues.push_back(parser.parseLiteral());
        parser.expect(")");
        return stmt;
    }

    SqlStatement parseSelect(Parser &parser)
    {
        SqlStatement stmt;
        stmt.type          = SqlStatementType::Select;
        stmt.selectColumns = parser.parseIdentifierList();

        if (stmt.selectColumns.size() == 1 && stmt.selectColumns[0] == "*")
        {
            stmt.selectAll = true;
            stmt.selectColumns.clear();
        }
        else
        {
            for (string &c : stmt.selectColumns) c = lowerCopy(c);
        }

        parser.expect("FROM");
        stmt.tableName = lowerCopy(parser.expectIdentifier());

        if (parser.match("JOIN"))
        {
            stmt.hasJoin = true;
            stmt.joinTable = lowerCopy(parser.expectIdentifier());
            parser.expect("ON");
            stmt.joinLeftColumn = lowerCopy(parser.expectIdentifier());
            parser.expect("=");
            stmt.joinRightColumn = lowerCopy(parser.expectIdentifier());
        }

        stmt.where = parser.parseWhereClause();
        return stmt;
    }

    SqlStatement parseUpdate(Parser &parser)
    {
        SqlStatement stmt;
        stmt.type      = SqlStatementType::Update;
        stmt.tableName = lowerCopy(parser.expectIdentifier());
        parser.expect("SET");
        stmt.updateAssignments.push_back(lowerCopy(parser.expectIdentifier()));
        parser.expect("=");
        stmt.updateValue          = parser.parseLiteral();
        stmt.hasUpdateAssignment  = true;
        stmt.where                = parser.parseWhereClause();
        return stmt;
    }

    SqlStatement parseDelete(Parser &parser)
    {
        SqlStatement stmt;
        stmt.type      = SqlStatementType::Delete;
        parser.expect("FROM");
        stmt.tableName = lowerCopy(parser.expectIdentifier());

        if (parser.match("JOIN"))
        {
            stmt.hasJoin = true;
            stmt.joinTable = lowerCopy(parser.expectIdentifier());
            parser.expect("ON");
            stmt.joinLeftColumn = lowerCopy(parser.expectIdentifier());
            parser.expect("=");
            stmt.joinRightColumn = lowerCopy(parser.expectIdentifier());
        }

        stmt.where = parser.parseWhereClause();
        return stmt;
    }
}

vector<string> tokenizeSql(const string &sql)
{
    return makeTokens(stripTrailingSemicolon(sql));
}

SqlStatement parseSql(const string &sql)
{
    Parser parser(tokenizeSql(sql));
    if (parser.eof())
        throw runtime_error("Empty SQL statement");

    string keyword = upperCopy(parser.consume());
    SqlStatement stmt;

    if      (keyword == "CREATE")   stmt = parseCreateTable(parser);
    else if (keyword == "INSERT")   stmt = parseInsert(parser);
    else if (keyword == "SELECT")   stmt = parseSelect(parser);
    else if (keyword == "UPDATE")   stmt = parseUpdate(parser);
    else if (keyword == "DELETE")   stmt = parseDelete(parser);
    else if (keyword == "BEGIN")    stmt.type = SqlStatementType::Begin;
    else if (keyword == "COMMIT")   stmt.type = SqlStatementType::Commit;
    else if (keyword == "ROLLBACK") stmt.type = SqlStatementType::Rollback;
    else
        throw runtime_error("Unsupported SQL statement: " + keyword);

    if (!parser.eof())
        throw runtime_error("Unexpected token after statement: '" + parser.peek() + "'");

    return stmt;
}