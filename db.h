/*
 * db.h — SQLite clone engine: public API
 *
 * All B-tree, pager, row, cursor, and statement logic lives in db.c.
 * Both the CLI (main.c) and the Win32 GUI (gui.c) include this header
 * and link against db.c.
 *
 * Output model
 * ============
 * Instead of printing directly with printf, every engine function that
 * produces visible output writes into the global db_output_buf[] array
 * via db_output_append().  Callers:
 *   1. Call db_output_reset() before dispatching a command.
 *   2. Dispatch the command (do_meta_command / prepare_statement /
 *      execute_statement).
 *   3. Read db_output_buf to get the text produced by the command.
 */

#ifndef DB_H
#define DB_H

#ifdef _WIN32
#  include <io.h>          /* _open, _read, _write, _lseek, _close */
#  include <fcntl.h>
#  include <sys/stat.h>
#  ifndef O_BINARY
#    define O_BINARY _O_BINARY
#  endif
/* ssize_t — MinGW64 provides it via corecrt.h; only shim on older toolchains */
#  if !defined(ssize_t) && !defined(_SSIZE_T_DEFINED) && !defined(_SSIZE_T_)
     typedef long long ssize_t;
#    define _SSIZE_T_DEFINED
#  endif
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  define O_BINARY 0
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Output buffer                                                        */
/* ------------------------------------------------------------------ */

#define DB_OUTPUT_BUF_SIZE 65536

extern char db_output_buf[DB_OUTPUT_BUF_SIZE];
extern int  db_output_len;

/* Reset the output buffer (call before each command). */
void db_output_reset(void);

/* Append formatted text to the output buffer (used internally by db.c). */
void db_output_append(const char* fmt, ...);

/* ------------------------------------------------------------------ */
/* Result enumerations                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    EXECUTE_SUCCESS,
    EXECUTE_DUPLICATE_KEY,
    EXECUTE_TABLE_FULL
} ExecuteResult;

typedef enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_EXIT,           /* .exit — caller should close db & quit */
    META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum {
    PREPARE_SUCCESS,
    PREPARE_NEGATIVE_ID,
    PREPARE_STRING_TOO_LONG,
    PREPARE_SYNTAX_ERROR,
    PREPARE_UNRECOGNIZED_STATEMENT
} PrepareResult;

/* ------------------------------------------------------------------ */
/* Node types                                                           */
/* ------------------------------------------------------------------ */

typedef enum { NODE_INTERNAL, NODE_LEAF } NodeType;

/* ------------------------------------------------------------------ */
/* Row / Schema                                                         */
/* ------------------------------------------------------------------ */

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE    255

typedef struct {
    uint32_t id;
    char username[COLUMN_USERNAME_SIZE + 1];
    char email[COLUMN_EMAIL_SIZE + 1];
} Row;

/* ------------------------------------------------------------------ */
/* Statement                                                            */
/* ------------------------------------------------------------------ */

typedef enum { STATEMENT_INSERT, STATEMENT_SELECT } StatementType;

typedef struct {
    StatementType type;
    Row row_to_insert;   /* only used by insert */
} Statement;

/* ------------------------------------------------------------------ */
/* Storage constants                                                    */
/* ------------------------------------------------------------------ */

#define TABLE_MAX_PAGES 1000

extern const uint32_t PAGE_SIZE;

/* Row layout */
extern const uint32_t ID_SIZE;
extern const uint32_t USERNAME_SIZE;
extern const uint32_t EMAIL_SIZE;
extern const uint32_t ID_OFFSET;
extern const uint32_t USERNAME_OFFSET;
extern const uint32_t EMAIL_OFFSET;
extern const uint32_t ROW_SIZE;

/* Leaf node layout */
extern const uint32_t LEAF_NODE_MAX_CELLS;
extern const uint32_t LEAF_NODE_LEFT_SPLIT_COUNT;
extern const uint32_t LEAF_NODE_RIGHT_SPLIT_COUNT;

/* ------------------------------------------------------------------ */
/* Storage structures                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int      file_descriptor;
    uint32_t file_length;
    uint32_t num_pages;
    void*    pages[TABLE_MAX_PAGES];
} Pager;

typedef struct {
    uint32_t root_page_num;
    Pager*   pager;
} Table;

typedef struct {
    Table*   table;
    uint32_t page_num;
    uint32_t cell_num;
    bool     end_of_table;
} Cursor;

/* ------------------------------------------------------------------ */
/* Input buffer (used by CLI; GUI has its own input handling)          */
/* ------------------------------------------------------------------ */

typedef struct {
    char*  buffer;
    size_t buffer_length;
    size_t input_length;
} InputBuffer;

InputBuffer* new_input_buffer(void);
void         read_input(InputBuffer* input_buffer);
void         close_input_buffer(InputBuffer* input_buffer);

/* ------------------------------------------------------------------ */
/* Database open / close                                                */
/* ------------------------------------------------------------------ */

Table* db_open(const char* filename);
void   db_close(Table* table);

/* ------------------------------------------------------------------ */
/* Command dispatch                                                     */
/* ------------------------------------------------------------------ */

MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table);
PrepareResult     prepare_statement(InputBuffer* input_buffer, Statement* statement);
ExecuteResult     execute_statement(Statement* statement, Table* table);

/*
 * Convenience: run a NUL-terminated command string through the full
 * pipeline (meta-command → prepare → execute) and write all output
 * into db_output_buf.  Returns META_COMMAND_EXIT when the caller
 * should shut down.
 *
 * On entry, cmd must NOT include a trailing newline.
 */
MetaCommandResult db_run_command(Table* table, const char* cmd);

/* ------------------------------------------------------------------ */
/* Internal helpers exposed for debugging / GUI use                    */
/* ------------------------------------------------------------------ */

void   print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level);
void   print_row(Row* row);
void   serialize_row(Row* source, void* destination);
void   deserialize_row(void* source, Row* destination);
void*  get_page(Pager* pager, uint32_t page_num);
Cursor* table_start(Table* table);
Cursor* table_find(Table* table, uint32_t key);
void*  cursor_value(Cursor* cursor);
void   cursor_advance(Cursor* cursor);

#endif /* DB_H */
