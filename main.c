/*
 * main.c — CLI front-end for the SQLite clone engine.
 *
 * Compile with:
 *   make cli
 * or manually:
 *   gcc -std=c99 -Wall -o sqlite_clone_cli.exe db.c main.c
 *
 * Usage:
 *   sqlite_clone_cli.exe mydb.db
 */

#include <stdio.h>
#include <stdlib.h>
#include "db.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <database_file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char* filename = argv[1];
    Table* table = db_open(filename);
    if (table == NULL) {
        /* db_open wrote an error into db_output_buf */
        fprintf(stderr, "%s", db_output_buf);
        exit(EXIT_FAILURE);
    }

    InputBuffer* input_buffer = new_input_buffer();

    while (1) {
        /* Show prompt */
        printf("db > ");
        fflush(stdout);

        read_input(input_buffer);

        MetaCommandResult res = db_run_command(table, input_buffer->buffer);

        /* Print whatever the engine produced */
        if (db_output_len > 0) {
            fputs(db_output_buf, stdout);
        }

        if (res == META_COMMAND_EXIT) {
            db_close(table);
            close_input_buffer(input_buffer);
            exit(EXIT_SUCCESS);
        }
    }
}
