#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <io.h>
#define popen _popen
#define pclose _pclose
#endif

// Helper function for testing
void test_commands(char* commands[], int num_commands, char* expected_output) {
    FILE* fp;
    char path[1024];
    char command_str[2048] = "";
    char temp_file_name[] = "temp_commands.txt";

    // Write commands to a temporary file
    FILE* temp_file = fopen(temp_file_name, "w");
    if (temp_file == NULL) {
        printf("Failed to create temporary file\n");
        exit(1);
    }
    for (int i = 0; i < num_commands; i++) {
        fprintf(temp_file, "%s\n", commands[i]);
    }
    fclose(temp_file);

    // Execute the sqlite_clone program and redirect the temporary file to its stdin
    fp = popen("sqlite_clone < temp_commands.txt", "r");
    if (fp == NULL) {
        printf("Failed to run command\n");
        exit(1);
    }

    // Read the output
    char actual_output[4096] = "";
    while (fgets(path, sizeof(path), fp) != NULL) {
        strcat(actual_output, path);
    }

    pclose(fp);

    // Clean up the temporary file
    remove(temp_file_name);

    // Compare actual output with expected output
    if (strcmp(actual_output, expected_output) == 0) {
        printf("Test passed!\n");
    } else {
        printf("Test failed!\n");
        printf("Expected:\n%s\n", expected_output);
        printf("Actual:\n%s\n", actual_output);
    }
}

#define COMMAND_BUFFER_SIZE 256


typedef struct {
  char buffer[COMMAND_BUFFER_SIZE];
  ssize_t input_length;
} InputBuffer;

InputBuffer* new_input_buffer() {
  InputBuffer* input_buffer = malloc(sizeof(InputBuffer));
  input_buffer->input_length = 0;

  return input_buffer;
}

void print_prompt() { printf("db > "); }

void read_input(InputBuffer* input_buffer) {
  if (fgets(input_buffer->buffer, COMMAND_BUFFER_SIZE, stdin) == NULL) {
    printf("Error reading input\n");
    exit(EXIT_FAILURE);
  }

  // Find the length of the input, excluding the newline
  input_buffer->input_length = strlen(input_buffer->buffer);
  if (input_buffer->buffer[input_buffer->input_length - 1] == '\n') {
    input_buffer->input_length--;
    input_buffer->buffer[input_buffer->input_length] = 0;
  }
}

void close_input_buffer(InputBuffer* input_buffer) {
    free(input_buffer);
}

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255

typedef struct {
  uint32_t id;
  char username[COLUMN_USERNAME_SIZE + 1];
  char email[COLUMN_EMAIL_SIZE + 1];
} Row;

typedef enum {
  STATEMENT_INSERT,
  STATEMENT_SELECT
} StatementType;

typedef struct {
  StatementType type;
  Row row_to_insert; // Only used for STATEMENT_INSERT
} Statement;

typedef enum {
  PREPARE_SUCCESS,
  PREPARE_UNRECOGNIZED_STATEMENT,
  PREPARE_SYNTAX_ERROR,
  PREPARE_STRING_TOO_LONG
} PrepareResult;

PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement) {
  if (strncmp(input_buffer->buffer, "insert", 6) == 0) {
    statement->type = STATEMENT_INSERT;
    char* keyword = strtok(input_buffer->buffer, " ");
    char* id_string = strtok(NULL, " ");
    char* username = strtok(NULL, " ");
    char* email = strtok(NULL, " ");

    if (id_string == NULL || username == NULL || email == NULL) {
      return PREPARE_SYNTAX_ERROR;
    }

    statement->row_to_insert.id = atoi(id_string);
    if (strlen(username) > COLUMN_USERNAME_SIZE) {
      return PREPARE_STRING_TOO_LONG;
    }
    strcpy(statement->row_to_insert.username, username);

    if (strlen(email) > COLUMN_EMAIL_SIZE) {
      return PREPARE_STRING_TOO_LONG;
    }
    strcpy(statement->row_to_insert.email, email);

    return PREPARE_SUCCESS;
  }

  if (strcmp(input_buffer->buffer, "select") == 0) {
    statement->type = STATEMENT_SELECT;
    return PREPARE_SUCCESS;
  }

  return PREPARE_UNRECOGNIZED_STATEMENT;
}

typedef enum {
  EXECUTE_SUCCESS,
  EXECUTE_TABLE_FULL
} ExecuteResult;

ExecuteResult execute_statement(Statement* statement) {
  switch (statement->type) {
    case STATEMENT_INSERT:
      printf("This is where we would do an insert.\n");
      break;
    case STATEMENT_SELECT:
      printf("This is where we would do a select.\n");
      break;
  }
  return EXECUTE_SUCCESS;
}

typedef enum {
  META_COMMAND_SUCCESS,
  META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

MetaCommandResult do_meta_command(InputBuffer* input_buffer) {
  if (strcmp(input_buffer->buffer, ".exit") == 0) {
    close_input_buffer(input_buffer);
    exit(EXIT_SUCCESS);
  } else {
    return META_COMMAND_UNRECOGNIZED_COMMAND;
  }
}

int main(int argc, char* argv[]) {
  InputBuffer* input_buffer = new_input_buffer();

  while (true) {
    print_prompt();
    read_input(input_buffer);

    if (input_buffer->buffer[0] == '.') {
      switch (do_meta_command(input_buffer)) {
        case META_COMMAND_SUCCESS:
          continue;
        case META_COMMAND_UNRECOGNIZED_COMMAND:
          printf("Unrecognized command '%s'.\n", input_buffer->buffer);
          continue;
      }
    }

        Statement statement;
    switch (prepare_statement(input_buffer, &statement)) {
      case PREPARE_SUCCESS:
        break;
      case PREPARE_UNRECOGNIZED_STATEMENT:
        printf("Unrecognized keyword at start of '%s'.\n",
               input_buffer->buffer);
        continue;
      case PREPARE_SYNTAX_ERROR:
        printf("Syntax error. Could not parse statement.\n");
        continue;
      case PREPARE_STRING_TOO_LONG:
        printf("String too long.\n");
        continue;
    }

    execute_statement(&statement);
  }
}