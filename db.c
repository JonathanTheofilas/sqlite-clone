/*
 * db.c — SQLite clone engine implementation
 *
 * All output goes through db_output_append() rather than printf() so
 * that both the CLI and the GUI front-end can consume results the same
 * way without any change to the engine logic.
 */

/* Enable getline() on POSIX systems */
#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif

#include "db.h"

/* ------------------------------------------------------------------ */
/* Output buffer                                                        */
/* ------------------------------------------------------------------ */

char db_output_buf[DB_OUTPUT_BUF_SIZE];
int  db_output_len = 0;

void db_output_reset(void) {
    db_output_len    = 0;
    db_output_buf[0] = '\0';
}

void db_output_append(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int remaining = DB_OUTPUT_BUF_SIZE - db_output_len - 1;
    if (remaining > 0) {
        int written = vsnprintf(db_output_buf + db_output_len,
                                (size_t)remaining, fmt, args);
        if (written > 0) {
            db_output_len += (written < remaining) ? written : remaining;
            db_output_buf[db_output_len] = '\0';
        }
    }
    va_end(args);
}

/* ------------------------------------------------------------------ */
/* Row layout constants                                                 */
/* ------------------------------------------------------------------ */

#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0)->Attribute)

const uint32_t PAGE_SIZE       = 4096;

const uint32_t ID_SIZE         = size_of_attribute(Row, id);
const uint32_t USERNAME_SIZE   = size_of_attribute(Row, username);
const uint32_t EMAIL_SIZE      = size_of_attribute(Row, email);
const uint32_t ID_OFFSET       = 0;
const uint32_t USERNAME_OFFSET = size_of_attribute(Row, id);
const uint32_t EMAIL_OFFSET    = size_of_attribute(Row, id)
                               + size_of_attribute(Row, username);
const uint32_t ROW_SIZE        = size_of_attribute(Row, id)
                               + size_of_attribute(Row, username)
                               + size_of_attribute(Row, email);

/* ------------------------------------------------------------------ */
/* Node layout offsets and sizes (all expressed as byte values)        */
/* ------------------------------------------------------------------ */

/* Common header:  [node_type: u8][is_root: u8][parent_ptr: u32]      */
#define NODE_TYPE_OFFSET        0u
#define IS_ROOT_OFFSET          1u                          /* sizeof(u8)           */
#define PARENT_POINTER_OFFSET   2u                          /* sizeof(u8)*2         */
#define COMMON_NODE_HEADER_SIZE 6u                          /* u8 + u8 + u32        */

/* Internal node header:  common + [num_keys: u32][right_child: u32]  */
#define INTERNAL_NODE_NUM_KEYS_OFFSET    COMMON_NODE_HEADER_SIZE
#define INTERNAL_NODE_RIGHT_CHILD_OFFSET (COMMON_NODE_HEADER_SIZE + 4u)
#define INTERNAL_NODE_HEADER_SIZE        (COMMON_NODE_HEADER_SIZE + 8u)

/* Internal node cell:  [child_page: u32][key: u32]                   */
#define INTERNAL_NODE_CHILD_SIZE  4u
#define INTERNAL_NODE_KEY_SIZE    4u
#define INTERNAL_NODE_CELL_SIZE   8u

/* Leaf node header:  common + [num_cells: u32][next_leaf: u32]       */
#define LEAF_NODE_NUM_CELLS_OFFSET  COMMON_NODE_HEADER_SIZE
#define LEAF_NODE_NEXT_LEAF_OFFSET  (COMMON_NODE_HEADER_SIZE + 4u)
#define LEAF_NODE_HEADER_SIZE       (COMMON_NODE_HEADER_SIZE + 8u)

/* Leaf node cell:  [key: u32][row: ROW_SIZE bytes]                   */
#define LEAF_NODE_KEY_SIZE 4u
/* LEAF_NODE_VALUE_SIZE and LEAF_NODE_VALUE_OFFSET depend on ROW_SIZE,
   which is not a compile-time constant in C99 when using sizeof on
   struct fields — compute them as functions of ROW_SIZE at runtime.  */

/* These three are exported (declared in db.h) */
const uint32_t LEAF_NODE_MAX_CELLS =
    (4096 - (sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t)
           + sizeof(uint32_t) + sizeof(uint32_t)))
    / (sizeof(uint32_t)
     + size_of_attribute(Row, id)
     + size_of_attribute(Row, username)
     + size_of_attribute(Row, email));

const uint32_t LEAF_NODE_RIGHT_SPLIT_COUNT =
    ((4096 - (sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t)
             + sizeof(uint32_t) + sizeof(uint32_t)))
    / (sizeof(uint32_t)
     + size_of_attribute(Row, id)
     + size_of_attribute(Row, username)
     + size_of_attribute(Row, email))
    + 1) / 2;

const uint32_t LEAF_NODE_LEFT_SPLIT_COUNT =
    ((4096 - (sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t)
             + sizeof(uint32_t) + sizeof(uint32_t)))
    / (sizeof(uint32_t)
     + size_of_attribute(Row, id)
     + size_of_attribute(Row, username)
     + size_of_attribute(Row, email))
    + 1)
  - (((4096 - (sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t)
              + sizeof(uint32_t) + sizeof(uint32_t)))
    / (sizeof(uint32_t)
     + size_of_attribute(Row, id)
     + size_of_attribute(Row, username)
     + size_of_attribute(Row, email))
    + 1) / 2);

/* helper: leaf node cell size */
static uint32_t leaf_node_cell_size(void) {
    return LEAF_NODE_KEY_SIZE + ROW_SIZE;
}

/* ------------------------------------------------------------------ */
/* Row serialization                                                    */
/* ------------------------------------------------------------------ */

void serialize_row(Row* source, void* destination) {
    memcpy((char*)destination + ID_OFFSET,       &(source->id),       ID_SIZE);
    memcpy((char*)destination + USERNAME_OFFSET, &(source->username), USERNAME_SIZE);
    memcpy((char*)destination + EMAIL_OFFSET,    &(source->email),    EMAIL_SIZE);
}

void deserialize_row(void* source, Row* destination) {
    memcpy(&(destination->id),       (char*)source + ID_OFFSET,       ID_SIZE);
    memcpy(&(destination->username), (char*)source + USERNAME_OFFSET, USERNAME_SIZE);
    memcpy(&(destination->email),    (char*)source + EMAIL_OFFSET,    EMAIL_SIZE);
}

void print_row(Row* row) {
    db_output_append("(%d, %s, %s)\n", row->id, row->username, row->email);
}

/* ------------------------------------------------------------------ */
/* Page / pager                                                         */
/* ------------------------------------------------------------------ */

void* get_page(Pager* pager, uint32_t page_num) {
    if (page_num > TABLE_MAX_PAGES) {
        fprintf(stderr, "Tried to fetch page number out of bounds. %d > %d\n",
                page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    if (pager->pages[page_num] == NULL) {
        void*    page      = malloc(PAGE_SIZE);
        uint32_t num_pages = pager->file_length / PAGE_SIZE;

        if (pager->file_length % PAGE_SIZE) {
            num_pages += 1;
        }

        if (page_num <= num_pages) {
#ifdef _WIN32
            _lseeki64(pager->file_descriptor, (long long)page_num * PAGE_SIZE, SEEK_SET);
            ssize_t bytes_read = _read(pager->file_descriptor, page, PAGE_SIZE);
#else
            lseek(pager->file_descriptor, (off_t)page_num * PAGE_SIZE, SEEK_SET);
            ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
#endif
            if (bytes_read == -1) {
                fprintf(stderr, "Error reading file: %d\n", errno);
                exit(EXIT_FAILURE);
            }
        }
        pager->pages[page_num] = page;

        if (page_num >= pager->num_pages) {
            pager->num_pages = page_num + 1;
        }
    }

    return pager->pages[page_num];
}

static void pager_flush(Pager* pager, uint32_t page_num) {
    if (pager->pages[page_num] == NULL) {
        fprintf(stderr, "Tried to flush null page\n");
        exit(EXIT_FAILURE);
    }

#ifdef _WIN32
    long long offset = _lseeki64(pager->file_descriptor,
                                 (long long)page_num * PAGE_SIZE, SEEK_SET);
#else
    off_t offset = lseek(pager->file_descriptor,
                         (off_t)page_num * PAGE_SIZE, SEEK_SET);
#endif
    if (offset == -1) {
        fprintf(stderr, "Error seeking: %d\n", errno);
        exit(EXIT_FAILURE);
    }

#ifdef _WIN32
    ssize_t bytes_written = _write(pager->file_descriptor,
                                   pager->pages[page_num], PAGE_SIZE);
#else
    ssize_t bytes_written = write(pager->file_descriptor,
                                  pager->pages[page_num], PAGE_SIZE);
#endif
    if (bytes_written == -1) {
        fprintf(stderr, "Error writing: %d\n", errno);
        exit(EXIT_FAILURE);
    }
}

static Pager* pager_open(const char* filename) {
#ifdef _WIN32
    int fd = _open(filename,
                   _O_RDWR | _O_CREAT | _O_BINARY,
                   _S_IWRITE | _S_IREAD);
#else
    int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
#endif
    if (fd == -1) {
        db_output_append("Unable to open file: %s\n", filename);
        return NULL;
    }

#ifdef _WIN32
    long long file_length = _lseeki64(fd, 0, SEEK_END);
#else
    off_t file_length = lseek(fd, 0, SEEK_END);
#endif
    if (file_length == -1) {
        fprintf(stderr, "Error seeking to end of file\n");
        exit(EXIT_FAILURE);
    }

    if (file_length % PAGE_SIZE != 0) {
        db_output_append("Db file is not a whole number of pages. Corrupt file.\n");
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return NULL;
    }

    Pager* pager = malloc(sizeof(Pager));
    pager->file_descriptor = fd;
    pager->file_length     = (uint32_t)file_length;
    pager->num_pages       = (uint32_t)(file_length / PAGE_SIZE);

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pager->pages[i] = NULL;
    }

    return pager;
}

/* ------------------------------------------------------------------ */
/* Table open / close                                                   */
/* ------------------------------------------------------------------ */

Table* db_open(const char* filename) {
    Pager* pager = pager_open(filename);
    if (pager == NULL) return NULL;

    Table* table = malloc(sizeof(Table));
    table->pager        = pager;
    table->root_page_num = 0;

    if (pager->num_pages == 0) {
        void* root_node = get_page(pager, 0);
        /* initialize_leaf_node */
        uint8_t* p = (uint8_t*)root_node;
        p[NODE_TYPE_OFFSET]  = (uint8_t)NODE_LEAF;
        p[IS_ROOT_OFFSET]    = (uint8_t)1;
        *(uint32_t*)(p + LEAF_NODE_NUM_CELLS_OFFSET) = 0;
        *(uint32_t*)(p + LEAF_NODE_NEXT_LEAF_OFFSET) = 0;
    }

    return table;
}

void db_close(Table* table) {
    Pager* pager = table->pager;

    for (uint32_t i = 0; i < pager->num_pages; i++) {
        if (pager->pages[i] == NULL) continue;
        pager_flush(pager, i);
        free(pager->pages[i]);
        pager->pages[i] = NULL;
    }

#ifdef _WIN32
    int result = _close(pager->file_descriptor);
#else
    int result = close(pager->file_descriptor);
#endif
    if (result == -1) {
        fprintf(stderr, "Error closing db file.\n");
        exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (pager->pages[i]) {
            free(pager->pages[i]);
            pager->pages[i] = NULL;
        }
    }
    free(pager);
    free(table);
}

/* ------------------------------------------------------------------ */
/* Node accessor helpers                                                */
/* ------------------------------------------------------------------ */

static uint32_t* leaf_node_num_cells(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NUM_CELLS_OFFSET);
}

static uint32_t* leaf_node_next_leaf(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NEXT_LEAF_OFFSET);
}

static void* leaf_node_cell(void* node, uint32_t cell_num) {
    return (char*)node + LEAF_NODE_HEADER_SIZE + cell_num * leaf_node_cell_size();
}

static uint32_t* leaf_node_key(void* node, uint32_t cell_num) {
    return (uint32_t*)leaf_node_cell(node, cell_num);
}

static void* leaf_node_value(void* node, uint32_t cell_num) {
    return (char*)leaf_node_cell(node, cell_num) + LEAF_NODE_KEY_SIZE;
}

static NodeType get_node_type(void* node) {
    return (NodeType)*(uint8_t*)((char*)node + NODE_TYPE_OFFSET);
}

static void set_node_type(void* node, NodeType type) {
    *(uint8_t*)((char*)node + NODE_TYPE_OFFSET) = (uint8_t)type;
}

static bool node_is_root(void* node) {
    return (bool)*(uint8_t*)((char*)node + IS_ROOT_OFFSET);
}

static void set_is_root(void* node, bool is_root_val) {
    *(uint8_t*)((char*)node + IS_ROOT_OFFSET) = (uint8_t)is_root_val;
}

static void initialize_leaf_node(void* node) {
    set_node_type(node, NODE_LEAF);
    set_is_root(node, false);
    *leaf_node_num_cells(node) = 0;
    *leaf_node_next_leaf(node) = 0;
}

static void initialize_internal_node(void* node) {
    set_node_type(node, NODE_INTERNAL);
    set_is_root(node, false);
}

static uint32_t* internal_node_num_keys(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_NUM_KEYS_OFFSET);
}

static uint32_t* internal_node_right_child(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}

static uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_HEADER_SIZE
                       + cell_num * INTERNAL_NODE_CELL_SIZE);
}

static uint32_t* internal_node_child(void* node, uint32_t child_num) {
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_num > num_keys) {
        fprintf(stderr, "Tried to access child_num %d > num_keys %d\n",
                child_num, num_keys);
        exit(EXIT_FAILURE);
    } else if (child_num == num_keys) {
        return internal_node_right_child(node);
    } else {
        return internal_node_cell(node, child_num);
    }
}

static uint32_t* internal_node_key(void* node, uint32_t key_num) {
    return (uint32_t*)((char*)internal_node_cell(node, key_num)
                       + INTERNAL_NODE_CHILD_SIZE);
}

static uint32_t get_node_max_key(void* node) {
    switch (get_node_type(node)) {
        case NODE_INTERNAL:
            return *internal_node_key(node, *internal_node_num_keys(node) - 1);
        case NODE_LEAF:
            return *leaf_node_key(node, *leaf_node_num_cells(node) - 1);
    }
    return 0; /* unreachable */
}

/* ------------------------------------------------------------------ */
/* Cursor                                                               */
/* ------------------------------------------------------------------ */

void* cursor_value(Cursor* cursor) {
    void* page = get_page(cursor->table->pager, cursor->page_num);
    return leaf_node_value(page, cursor->cell_num);
}

void cursor_advance(Cursor* cursor) {
    void* node = get_page(cursor->table->pager, cursor->page_num);
    cursor->cell_num += 1;
    if (cursor->cell_num >= *leaf_node_num_cells(node)) {
        uint32_t next_page_num = *leaf_node_next_leaf(node);
        if (next_page_num == 0) {
            cursor->end_of_table = true;
        } else {
            cursor->page_num  = next_page_num;
            cursor->cell_num  = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Table search / start                                                 */
/* ------------------------------------------------------------------ */

static Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void*    node      = get_page(table->pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    Cursor* cursor  = malloc(sizeof(Cursor));
    cursor->table   = table;
    cursor->page_num = page_num;

    uint32_t min_index       = 0;
    uint32_t one_past_max    = num_cells;
    while (one_past_max != min_index) {
        uint32_t index         = (min_index + one_past_max) / 2;
        uint32_t key_at_index  = *leaf_node_key(node, index);
        if (key == key_at_index) {
            cursor->cell_num = index;
            return cursor;
        }
        if (key < key_at_index) {
            one_past_max = index;
        } else {
            min_index = index + 1;
        }
    }
    cursor->cell_num = min_index;
    return cursor;
}

static Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key);

Cursor* table_find(Table* table, uint32_t key) {
    void* root_node = get_page(table->pager, table->root_page_num);
    if (get_node_type(root_node) == NODE_LEAF) {
        return leaf_node_find(table, table->root_page_num, key);
    } else {
        return internal_node_find(table, table->root_page_num, key);
    }
}

static Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void*    node     = get_page(table->pager, page_num);
    uint32_t num_keys = *internal_node_num_keys(node);

    uint32_t min_index = 0;
    uint32_t max_index = num_keys;

    while (min_index != max_index) {
        uint32_t index        = (min_index + max_index) / 2;
        uint32_t key_to_right = *internal_node_key(node, index);
        if (key_to_right >= key) {
            max_index = index;
        } else {
            min_index = index + 1;
        }
    }

    uint32_t child_num = *internal_node_child(node, min_index);
    void*    child     = get_page(table->pager, child_num);
    switch (get_node_type(child)) {
        case NODE_LEAF:
            return leaf_node_find(table, child_num, key);
        case NODE_INTERNAL:
            return internal_node_find(table, child_num, key);
    }
    return NULL; /* unreachable */
}

Cursor* table_start(Table* table) {
    uint32_t page_num = table->root_page_num;
    void*    node     = get_page(table->pager, page_num);

    while (get_node_type(node) != NODE_LEAF) {
        page_num = *internal_node_child(node, 0);
        node     = get_page(table->pager, page_num);
    }

    Cursor* cursor      = malloc(sizeof(Cursor));
    cursor->table       = table;
    cursor->page_num    = page_num;
    cursor->cell_num    = 0;
    cursor->end_of_table = (*leaf_node_num_cells(node) == 0);

    return cursor;
}

/* ------------------------------------------------------------------ */
/* B-tree splits                                                        */
/* ------------------------------------------------------------------ */

static uint32_t get_unused_page_num(Pager* pager) { return pager->num_pages; }

static void create_new_root(Table* table, uint32_t right_child_page_num) {
    void*    root                = get_page(table->pager, table->root_page_num);
    uint32_t left_child_page_num = get_unused_page_num(table->pager);
    void*    left_child          = get_page(table->pager, left_child_page_num);

    memcpy(left_child, root, PAGE_SIZE);
    set_is_root(left_child, false);

    initialize_internal_node(root);
    set_is_root(root, true);
    *internal_node_num_keys(root)  = 1;
    *internal_node_child(root, 0)  = left_child_page_num;
    uint32_t left_child_max_key    = get_node_max_key(left_child);
    *internal_node_key(root, 0)    = left_child_max_key;
    *internal_node_right_child(root) = right_child_page_num;
}

static void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value);

static void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, Row* value) {
    void*    old_node    = get_page(cursor->table->pager, cursor->page_num);
    uint32_t new_page_num = get_unused_page_num(cursor->table->pager);
    void*    new_node    = get_page(cursor->table->pager, new_page_num);
    initialize_leaf_node(new_node);
    *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
    *leaf_node_next_leaf(old_node) = new_page_num;

    for (int32_t i = (int32_t)LEAF_NODE_MAX_CELLS; i >= 0; i--) {
        void*    destination_node;
        if (i >= (int32_t)LEAF_NODE_LEFT_SPLIT_COUNT) {
            destination_node = new_node;
        } else {
            destination_node = old_node;
        }
        uint32_t index_within_node = (uint32_t)i % LEAF_NODE_LEFT_SPLIT_COUNT;
        void*    destination = leaf_node_cell(destination_node, index_within_node);

        if (i == (int32_t)cursor->cell_num) {
            serialize_row(value,
                leaf_node_value(destination_node, index_within_node));
            *leaf_node_key(destination_node, index_within_node) = key;
        } else if (i > (int32_t)cursor->cell_num) {
            memcpy(destination,
                   leaf_node_cell(old_node, (uint32_t)i - 1),
                   leaf_node_cell_size());
        } else {
            memcpy(destination,
                   leaf_node_cell(old_node, (uint32_t)i),
                   leaf_node_cell_size());
        }
    }

    *leaf_node_num_cells(old_node) = LEAF_NODE_LEFT_SPLIT_COUNT;
    *leaf_node_num_cells(new_node) = LEAF_NODE_RIGHT_SPLIT_COUNT;

    if (node_is_root(old_node)) {
        create_new_root(cursor->table, new_page_num);
    } else {
        fprintf(stderr, "Need to implement updating parent after split\n");
        exit(EXIT_FAILURE);
    }
}

static void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value) {
    void*    node     = get_page(cursor->table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (num_cells >= LEAF_NODE_MAX_CELLS) {
        leaf_node_split_and_insert(cursor, key, value);
        return;
    }

    if (cursor->cell_num < num_cells) {
        for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
            memcpy(leaf_node_cell(node, i),
                   leaf_node_cell(node, i - 1),
                   leaf_node_cell_size());
        }
    }

    *leaf_node_num_cells(node) += 1;
    *leaf_node_key(node, cursor->cell_num) = key;
    serialize_row(value, leaf_node_value(node, cursor->cell_num));
}

/* ------------------------------------------------------------------ */
/* Execute statements                                                   */
/* ------------------------------------------------------------------ */

static ExecuteResult execute_insert(Statement* statement, Table* table) {
    void*    node      = get_page(table->pager, table->root_page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    Row*     row_to_insert  = &(statement->row_to_insert);
    uint32_t key_to_insert  = row_to_insert->id;
    Cursor*  cursor         = table_find(table, key_to_insert);

    if (cursor->cell_num < num_cells) {
        uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num);
        if (key_at_index == key_to_insert) {
            free(cursor);
            return EXECUTE_DUPLICATE_KEY;
        }
    }

    leaf_node_insert(cursor, row_to_insert->id, row_to_insert);
    free(cursor);
    return EXECUTE_SUCCESS;
}

static ExecuteResult execute_select(Statement* statement, Table* table) {
    (void)statement;
    Cursor* cursor = table_start(table);
    Row row;
    while (!(cursor->end_of_table)) {
        deserialize_row(cursor_value(cursor), &row);
        print_row(&row);
        cursor_advance(cursor);
    }
    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement* statement, Table* table) {
    switch (statement->type) {
        case STATEMENT_INSERT: return execute_insert(statement, table);
        case STATEMENT_SELECT: return execute_select(statement, table);
    }
    return EXECUTE_SUCCESS; /* unreachable */
}

/* ------------------------------------------------------------------ */
/* Print tree (debug)                                                   */
/* ------------------------------------------------------------------ */

static void indent(uint32_t level) {
    for (uint32_t i = 0; i < level; i++) {
        db_output_append("  ");
    }
}

void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level) {
    void*    node     = get_page(pager, page_num);
    uint32_t num_keys, child;

    switch (get_node_type(node)) {
        case NODE_LEAF:
            num_keys = *leaf_node_num_cells(node);
            indent(indentation_level);
            db_output_append("- leaf (size %d)\n", num_keys);
            for (uint32_t i = 0; i < num_keys; i++) {
                indent(indentation_level + 1);
                db_output_append("- %d\n", *leaf_node_key(node, i));
            }
            break;
        case NODE_INTERNAL:
            num_keys = *internal_node_num_keys(node);
            indent(indentation_level);
            db_output_append("- internal (size %d)\n", num_keys);
            for (uint32_t i = 0; i < num_keys; i++) {
                child = *internal_node_child(node, i);
                print_tree(pager, child, indentation_level + 1);
                indent(indentation_level + 1);
                db_output_append("- key %d\n", *internal_node_key(node, i));
            }
            child = *internal_node_right_child(node);
            print_tree(pager, child, indentation_level + 1);
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Meta commands                                                        */
/* ------------------------------------------------------------------ */

MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table) {
    if (strcmp(input_buffer->buffer, ".exit") == 0) {
        /* Signal caller to close db and quit; we do NOT call exit() here. */
        return META_COMMAND_EXIT;
    } else if (strcmp(input_buffer->buffer, ".btree") == 0) {
        db_output_append("Tree:\n");
        print_tree(table->pager, 0, 0);
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".tables") == 0) {
        /* For now there is one hard-coded table. */
        db_output_append("users\n");
        return META_COMMAND_SUCCESS;
    } else {
        return META_COMMAND_UNRECOGNIZED_COMMAND;
    }
}

/* ------------------------------------------------------------------ */
/* Statement preparation                                                */
/* ------------------------------------------------------------------ */

static PrepareResult prepare_insert(InputBuffer* input_buffer, Statement* statement) {
    statement->type = STATEMENT_INSERT;

    /* strtok modifies the buffer, so we work in a local copy */
    char buf[512];
    strncpy(buf, input_buffer->buffer, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    strtok(buf, " ");                   /* "insert" */
    char* id_string = strtok(NULL, " ");
    char* username  = strtok(NULL, " ");
    char* email     = strtok(NULL, " ");

    if (id_string == NULL || username == NULL || email == NULL) {
        return PREPARE_SYNTAX_ERROR;
    }

    int id = atoi(id_string);
    if (id < 0) return PREPARE_NEGATIVE_ID;

    if (strlen(username) > COLUMN_USERNAME_SIZE) return PREPARE_STRING_TOO_LONG;
    if (strlen(email)    > COLUMN_EMAIL_SIZE)    return PREPARE_STRING_TOO_LONG;

    statement->row_to_insert.id = (uint32_t)id;
    strcpy(statement->row_to_insert.username, username);
    strcpy(statement->row_to_insert.email, email);

    return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement) {
    if (strncmp(input_buffer->buffer, "insert", 6) == 0) {
        return prepare_insert(input_buffer, statement);
    }
    if (strcmp(input_buffer->buffer, "select") == 0) {
        statement->type = STATEMENT_SELECT;
        return PREPARE_SUCCESS;
    }
    return PREPARE_UNRECOGNIZED_STATEMENT;
}

/* ------------------------------------------------------------------ */
/* Input buffer (CLI helper)                                            */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
/* MinGW does not always provide getline(); supply a simple version. */
static ssize_t win_getline(char** lineptr, size_t* n, FILE* stream) {
    size_t pos;
    int c;

    if (!lineptr || !stream || !n) { errno = EINVAL; return -1; }

    c = getc(stream);
    if (c == EOF) return -1;

    if (*lineptr == NULL) {
        *lineptr = (char*)malloc(128);
        if (!*lineptr) return -1;
        *n = 128;
    }

    pos = 0;
    while (c != EOF) {
        if (pos + 1 >= *n) {
            size_t new_size = *n + (*n >> 2);
            if (new_size < 128) new_size = 128;
            char* new_ptr = (char*)realloc(*lineptr, new_size);
            if (!new_ptr) return -1;
            *n = new_size;
            *lineptr = new_ptr;
        }
        ((unsigned char*)(*lineptr))[pos++] = (unsigned char)c;
        if (c == '\n') break;
        c = getc(stream);
    }
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}
#define getline win_getline
#endif

InputBuffer* new_input_buffer(void) {
    InputBuffer* ib  = malloc(sizeof(InputBuffer));
    ib->buffer        = NULL;
    ib->buffer_length = 0;
    ib->input_length  = 0;
    return ib;
}

void read_input(InputBuffer* input_buffer) {
    ssize_t bytes_read = getline(&(input_buffer->buffer),
                                 &(input_buffer->buffer_length), stdin);
    if (bytes_read <= 0) {
        fprintf(stderr, "Error reading input\n");
        exit(EXIT_FAILURE);
    }
    /* Strip trailing newline */
    input_buffer->input_length = (size_t)bytes_read - 1;
    input_buffer->buffer[bytes_read - 1] = '\0';
}

void close_input_buffer(InputBuffer* input_buffer) {
    free(input_buffer->buffer);
    free(input_buffer);
}

/* ------------------------------------------------------------------ */
/* Convenience: run a single command string end-to-end                 */
/* ------------------------------------------------------------------ */

MetaCommandResult db_run_command(Table* table, const char* cmd) {
    db_output_reset();

    /* Wrap in a temporary InputBuffer */
    InputBuffer ib;
    size_t len    = strlen(cmd);
    char*  buf    = malloc(len + 1);
    memcpy(buf, cmd, len + 1);
    ib.buffer        = buf;
    ib.buffer_length = len + 1;
    ib.input_length  = len;

    MetaCommandResult result = META_COMMAND_SUCCESS;

    if (ib.buffer[0] == '.') {
        result = do_meta_command(&ib, table);
        switch (result) {
            case META_COMMAND_SUCCESS:
            case META_COMMAND_EXIT:
                break;
            case META_COMMAND_UNRECOGNIZED_COMMAND:
                db_output_append("Unrecognized command '%s'\n", cmd);
                break;
        }
    } else {
        Statement statement;
        switch (prepare_statement(&ib, &statement)) {
            case PREPARE_SUCCESS:
                break;
            case PREPARE_NEGATIVE_ID:
                db_output_append("ID must be positive.\n");
                goto done;
            case PREPARE_STRING_TOO_LONG:
                db_output_append("String is too long.\n");
                goto done;
            case PREPARE_SYNTAX_ERROR:
                db_output_append("Syntax error. Could not parse statement.\n");
                goto done;
            case PREPARE_UNRECOGNIZED_STATEMENT:
                db_output_append("Unrecognized keyword at start of '%s'.\n", cmd);
                goto done;
        }

        switch (execute_statement(&statement, table)) {
            case EXECUTE_SUCCESS:
                db_output_append("Executed.\n");
                break;
            case EXECUTE_DUPLICATE_KEY:
                db_output_append("Error: Duplicate key.\n");
                break;
            case EXECUTE_TABLE_FULL:
                db_output_append("Error: Table full.\n");
                break;
        }
    }

done:
    free(buf);
    return result;
}
