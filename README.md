# SQLite Clone

This project is a simple clone of SQLite written from scratch in C. It's a learning project to understand how databases work internally.

## Features

*   **B-Tree Data Structure**: Data is stored in a B-Tree to allow for efficient insertion and retrieval.
*   **Basic SQL Operations**:
    *   `insert <id> <username> <email>`: Inserts a new record into the database.
    *   `select`: Retrieves all records from the database.
*   **Meta-commands**:
    *   `.exit`: Exits the program.
    *   `.btree`: Prints the B-Tree structure.
*   **Persistence**: The database is stored in a file and is loaded back into memory when the program is started.

## Usage

To use the SQLite clone, you first need to compile the `main.c` file.

### Compilation

```bash
gcc main.c -o sqlite_clone
```

### Running

To run the program, provide a filename as a command-line argument. If the file doesn't exist, it will be created.

```bash
./sqlite_clone test.db
```

Once the program is running, you can enter SQL commands or meta-commands at the `db >` prompt.

```
db > insert 1 user1 user1@example.com
Executed.
db > select
(1, user1, user1@example.com)
Executed.
db > .exit
```

## File Format

The database file is organized into pages. The first page is the root of the B-Tree. Each page can be either a leaf node or an internal node.

*   **Leaf Node**: Contains a list of key-value pairs. The key is the `id` of the record, and the value is the serialized record data.
*   **Internal Node**: Contains a list of keys and pointers to child nodes.

## Development

This project is for educational purposes. Contributions are welcome! If you want to contribute, please open an issue or a pull request.
