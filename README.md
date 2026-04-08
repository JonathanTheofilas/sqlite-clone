# SQLite Clone

A hand-rolled SQLite clone written from scratch in C. Stores data in a B-tree, persists it to disk, and ships with both a classic console REPL and a native Win32 GUI.

## Features

- **B-tree storage** — data is kept in a balanced B-tree for O(log n) inserts and lookups.
- **Persistent pages** — the database is stored as fixed-size 4 KB pages on disk and loaded back on startup.
- **SQL operations**
  - `insert <id> <username> <email>` — insert a row.
  - `select` — retrieve all rows.
- **Meta-commands**
  - `.exit` — close the database and quit.
  - `.tables` — list tables (currently: `users`).
  - `.btree` — print the B-tree structure for debugging.
- **Two front-ends** — a headless CLI and a resizable Win32 GUI window, both built from the same engine.

## Project structure

```
sqlite-clone/
├── db.h                  # Engine public API — types, constants, function declarations
├── db.c                  # B-tree engine (pager, cursor, insert/select, meta-commands)
├── main.c                # CLI front-end  →  sqlite_clone_cli.exe
├── gui.c                 # Win32 GUI front-end  →  sqlite_clone.exe
├── app.rc                # Windows resource file (version info, optional icon)
└── Makefile              # build targets: cli / gui / all / clean
```

### Engine / UI split

`db.c` never calls `printf` for results. Instead every function writes into a global 64 KB buffer (`db_output_buf`) via `db_output_append()`. After each command the caller reads the buffer and displays it however it likes — `fputs` to stdout in the CLI, `EM_REPLACESEL` into an Edit control in the GUI.

The convenience function `db_run_command(table, cmd)` runs a full command string through the pipeline (meta-command → prepare → execute) and returns `META_COMMAND_EXIT` when the application should shut down.

## Building

Requires [MinGW-w64](https://www.mingw-w64.org/) (available via [MSYS2](https://www.msys2.org/)):

```bash
# Both targets
make all

# Console REPL only
make cli        # → sqlite_clone_cli.exe

# Win32 GUI only
make gui        # → sqlite_clone.exe
```

To embed a custom application icon, place `app.ico` in the project root and pass `-DHAVE_ICON` to `windres`:

```makefile
app.res: app.rc
	windres -DHAVE_ICON app.rc -O coff -o app.res
```

## CLI usage

```
sqlite_clone_cli.exe mydb.db
```

```
db > insert 1 alice alice@example.com
Executed.
db > insert 2 bob bob@example.com
Executed.
db > select
(1, alice, alice@example.com)
(2, bob, bob@example.com)
Executed.
db > .tables
users
db > .btree
Tree:
- leaf (size 2)
  - 1
  - 2
db > .exit
```

## GUI usage

```
sqlite_clone.exe [optional_path.db]
```

On launch a **File → Open Database** dialog appears (skipped if a path is given on the command line). The window has two zones:

- **Output pane** (top) — scrollable read-only area showing all command output.
- **Input bar** (bottom) — type a command and press **Enter** or click **Run**.

The window title updates to show the current database filename. Use **File → New Database** to create a fresh `.db` file, **File → Close Database** to detach without quitting, or **File → Exit** to close everything.

## File format

The database file is a sequence of 4 KB pages:

| Page type | Contents |
|-----------|----------|
| Leaf node | Header (node type, is-root flag, parent pointer, cell count, next-leaf pointer) + array of `[key: u32, row: serialized bytes]` cells |
| Internal node | Header (node type, is-root flag, parent pointer, key count, right-child pointer) + array of `[child_page: u32, key: u32]` cells |

Row layout within each cell:

| Field | Offset | Size |
|-------|--------|------|
| id | 0 | 4 bytes |
| username | 4 | 33 bytes |
| email | 37 | 256 bytes |

## Development

This is a learning project. Contributions and issues are welcome — open a PR or file an issue on GitHub.
