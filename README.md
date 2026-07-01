# LeafDB

A custom relational database engine built from scratch in **C++**, featuring a hand-written SQL parser, B+ tree storage engine, write-ahead logging, snapshot-based transactions, secondary indexing, LLM integration, and a live web dashboard.

---

## Features

- **Hand-written SQL Parser** — tokenizer and recursive parser supporting `CREATE TABLE`, `INSERT`, `SELECT`, `UPDATE`, `DELETE`, `JOIN`, `WHERE`, `BETWEEN`, `BEGIN`, `COMMIT`, `ROLLBACK`
- **B+ Tree Storage Engine** — custom in-memory B+ tree with ordered key scans, prefix scans, range queries, and O(log n) point lookups
- **Write-Ahead Logging (WAL)** — all mutations are logged to disk before being applied; atomic checkpointing via temp-file-then-rename guarantees a consistent snapshot is always recoverable
- **Snapshot-based Transactions** — `BEGIN`/`COMMIT`/`ROLLBACK` with full atomicity; on rollback the pre-transaction snapshot is restored and indexes are rebuilt
- **Secondary Index** — automatic B+ tree index on `name` columns (TEXT + INT primary key tables) enabling fast equality and range lookups without a full table scan
- **LLM Integration** — connects to Google Gemini via libcurl; natural language queries are translated to MCP tool calls (execute_sql, list_tables, get_schema, describe_table) and executed against the database
- **MCP-style Tool Protocol** — custom tool-calling layer between the LLM and the query executor, routing AI responses to the correct database operation
- **Multi-threaded HTTP Server** — self-built TCP server serving a live web dashboard with a SQL query console, table browser, and AI chat interface
- **Crash Recovery** — on startup, replays any WAL entries not yet captured in the last checkpoint

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│                   Interfaces                    │
│        CLI (main.cpp)   Web UI (port 8080)      │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│              AI Interface Layer                 │
│   Natural language → MCP tool call → Executor  │
│         LLM Adapter (Google Gemini API)         │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│                  Executor                       │
│     SQL Parser → Query Planner → Executor       │
│   (CREATE / INSERT / SELECT / UPDATE / DELETE)  │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│              Storage Manager                    │
│   Catalog │ WAL │ Transactions │ Index          │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│                  KV Store                       │
│         B+ Tree (order 4, in-memory)            │
│     shared_mutex for concurrent read access     │
└─────────────────────────────────────────────────┘
```

---

## Build

**Requirements**
- C++17 compiler (g++ or clang++)
- CMake 3.14+
- libcurl (`sudo apt install libcurl4-openssl-dev`)

```bash
git clone https://github.com/Jayasurya1729/LeafDB.git
cd LeafDB
mkdir build && cd build
cmake .. && make
./leafdb
```

---

## Usage

### SQL Mode (default)

```sql
CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT)
INSERT INTO users VALUES (1, 'Alice', 30)
INSERT INTO users VALUES (2, 'Bob', 25)
SELECT * FROM users
SELECT * FROM users WHERE age > 25
UPDATE users SET age = 31 WHERE id = 1
DELETE FROM users WHERE id = 2

-- Transactions
BEGIN
INSERT INTO users VALUES (3, 'Charlie', 22)
ROLLBACK

-- Joins
SELECT * FROM users JOIN orders ON users.id = orders.user_id
SELECT users.name, orders.item FROM users JOIN orders ON users.id = orders.user_id WHERE users.id = 1
```

### AI Mode

```
mode ai
list tables
show all from users
show users where age > 25
describe users
```

### Web Dashboard

Open `http://localhost:8080` in your browser for the live dashboard with a SQL console, table browser, and AI assistant.

---

## LLM Setup

Set your Gemini API key before running:

```bash
export GEMINI_API_KEY=your_key_here
./leafdb
```

Without the key, SQL mode and the web dashboard work fully — only the AI assistant requires it.

---

## Project Structure

| File | Description |
|------|-------------|
| `btree.cpp / .h` | B+ tree implementation |
| `kvstore.cpp / .h` | Thread-safe KV store wrapping the B+ tree |
| `storage.cpp / .h` | Storage manager: WAL, transactions, indexing, persistence |
| `wal.cpp / .h` | Write-ahead log and checkpoint logic |
| `catalog.cpp / .h` | Table metadata and schema registry |
| `sql.cpp / .h` | SQL tokenizer and parser |
| `executor.cpp / .h` | Query executor for all statement types |
| `record.cpp / .h` | Record serialization and deserialization |
| `encode.cpp / .h` | Binary key encoding for B+ tree |
| `index.cpp / .h` | Secondary index on name columns |
| `table.cpp / .h` | Table scan and row-level operations |
| `ai_interface.cpp / .h` | Natural language → MCP → executor pipeline |
| `llm_adapter.cpp / .h` | Google Gemini API client via libcurl |
| `mcp_tools.cpp / .h` | MCP tool definitions and dispatch |
| `web_ui.cpp / .h` | Self-built HTTP server and dashboard |
| `main.cpp` | CLI entry point and signal handling |

---

## License

MIT