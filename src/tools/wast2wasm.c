/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"

#include "ast.h"
#include "ast-parser.h"
#include "binary-writer.h"
#include "binary-writer-spec.h"
#include "common.h"
#include "option-parser.h"
#include "resolve-names.h"
#include "stack-allocator.h"
#include "stream.h"
#include "validator.h"
#include "writer.h"

#define PROGRAM_NAME "wast2wasm"

static const char* s_infile;
static const char* s_outfile;
static WabtBool s_dump_module;
static WabtBool s_verbose;
static WabtWriteBinaryOptions s_write_binary_options =
    WABT_WRITE_BINARY_OPTIONS_DEFAULT;
static WabtWriteBinarySpecOptions s_write_binary_spec_options =
    WABT_WRITE_BINARY_SPEC_OPTIONS_DEFAULT;
static WabtBool s_spec;
static WabtBool s_use_libc_allocator;
static WabtBool s_validate = WABT_TRUE;

static WabtSourceErrorHandler s_error_handler =
    WABT_SOURCE_ERROR_HANDLER_DEFAULT;

static WabtFileStream s_log_stream;

#define NOPE WABT_OPTION_NO_ARGUMENT
#define YEP WABT_OPTION_HAS_ARGUMENT

enum {
  FLAG_VERBOSE,
  FLAG_HELP,
  FLAG_DUMP_MODULE,
  FLAG_OUTPUT,
  FLAG_RELOCATABLE,
  FLAG_SPEC,
  FLAG_USE_LIBC_ALLOCATOR,
  FLAG_NO_CANONICALIZE_LEB128S,
  FLAG_DEBUG_NAMES,
  FLAG_NO_CHECK,
  NUM_FLAGS
};

static const char s_description[] =
    "  read a file in the wasm s-expression format, check it for errors, and\n"
    "  convert it to the wasm binary format.\n"
    "\n"
    "examples:\n"
    "  # parse and typecheck test.wast\n"
    "  $ wast2wasm test.wast\n"
    "\n"
    "  # parse test.wast and write to binary file test.wasm\n"
    "  $ wast2wasm test.wast -o test.wasm\n"
    "\n"
    "  # parse spec-test.wast, and write verbose output to stdout (including\n"
    "  # the meaning of every byte)\n"
    "  $ wast2wasm spec-test.wast -v\n"
    "\n"
    "  # parse spec-test.wast, and write files to spec-test.json. Modules are\n"
    "  # written to spec-test.0.wasm, spec-test.1.wasm, etc.\n"
    "  $ wast2wasm spec-test.wast --spec -o spec-test.json\n";

static WabtOption s_options[] = {
    {FLAG_VERBOSE, 'v', "verbose", NULL, NOPE,
     "use multiple times for more info"},
    {FLAG_HELP, 'h', "help", NULL, NOPE, "print this help message"},
    {FLAG_DUMP_MODULE, 'd', "dump-module", NULL, NOPE,
     "print a hexdump of the module to stdout"},
    {FLAG_OUTPUT, 'o', "output", "FILE", YEP, "output wasm binary file"},
    {FLAG_RELOCATABLE, 'r', NULL, NULL, NOPE,
     "create a relocatable wasm binary (suitable for linking with wasm-link)"},
    {FLAG_SPEC, 0, "spec", NULL, NOPE,
     "parse a file with multiple modules and assertions, like the spec "
     "tests"},
    {FLAG_USE_LIBC_ALLOCATOR, 0, "use-libc-allocator", NULL, NOPE,
     "use malloc, free, etc. instead of stack allocator"},
    {FLAG_NO_CANONICALIZE_LEB128S, 0, "no-canonicalize-leb128s", NULL, NOPE,
     "Write all LEB128 sizes as 5-bytes instead of their minimal size"},
    {FLAG_DEBUG_NAMES, 0, "debug-names", NULL, NOPE,
     "Write debug names to the generated binary file"},
    {FLAG_NO_CHECK, 0, "no-check", NULL, NOPE,
     "Don't check for invalid modules"},
};
WABT_STATIC_ASSERT(NUM_FLAGS == WABT_ARRAY_SIZE(s_options));

static void on_option(struct WabtOptionParser* parser,
                      struct WabtOption* option,
                      const char* argument) {
  switch (option->id) {
    case FLAG_VERBOSE:
      s_verbose++;
      s_write_binary_options.log_stream = &s_log_stream.base;
      break;

    case FLAG_HELP:
      wabt_print_help(parser, PROGRAM_NAME);
      exit(0);
      break;

    case FLAG_DUMP_MODULE:
      s_dump_module = WABT_TRUE;
      break;

    case FLAG_OUTPUT:
      s_outfile = argument;
      break;

    case FLAG_RELOCATABLE:
      s_write_binary_options.relocatable = WABT_TRUE;
      break;

    case FLAG_SPEC:
      s_spec = WABT_TRUE;
      break;

    case FLAG_USE_LIBC_ALLOCATOR:
      s_use_libc_allocator = WABT_TRUE;
      break;

    case FLAG_NO_CANONICALIZE_LEB128S:
      s_write_binary_options.canonicalize_lebs = WABT_FALSE;
      break;

    case FLAG_DEBUG_NAMES:
      s_write_binary_options.write_debug_names = WABT_TRUE;
      break;

    case FLAG_NO_CHECK:
      s_validate = WABT_FALSE;
      break;
  }
}

static void on_argument(struct WabtOptionParser* parser, const char* argument) {
  s_infile = argument;
}

static void on_option_error(struct WabtOptionParser* parser,
                            const char* message) {
  WABT_FATAL("%s\n", message);
}

static void parse_options(int argc, char** argv) {
  WabtOptionParser parser;
  WABT_ZERO_MEMORY(parser);
  parser.description = s_description;
  parser.options = s_options;
  parser.num_options = WABT_ARRAY_SIZE(s_options);
  parser.on_option = on_option;
  parser.on_argument = on_argument;
  parser.on_error = on_option_error;
  wabt_parse_options(&parser, argc, argv);

  if (!s_infile) {
    wabt_print_help(&parser, PROGRAM_NAME);
    WABT_FATAL("No filename given.\n");
  }
}

static void write_buffer_to_file(const char* filename,
                                 WabtOutputBuffer* buffer) {
  if (s_dump_module) {
    if (s_verbose)
      wabt_writef(&s_log_stream.base, ";; dump\n");
    wabt_write_output_buffer_memory_dump(&s_log_stream.base, buffer);
  }

  if (filename) {
    wabt_write_output_buffer_to_file(buffer, filename);
  }
}

int main(int argc, char** argv) {
  WabtStackAllocator stack_allocator;
  WabtAllocator* allocator;

  wabt_init_stdio();

  wabt_init_file_stream_from_existing(&s_log_stream, stdout);
  parse_options(argc, argv);

  if (s_use_libc_allocator) {
    allocator = &g_wabt_libc_allocator;
  } else {
    wabt_init_stack_allocator(&stack_allocator, &g_wabt_libc_allocator);
    allocator = &stack_allocator.allocator;
  }

  WabtAstLexer* lexer = wabt_new_ast_file_lexer(allocator, s_infile);
  if (!lexer)
    WABT_FATAL("unable to read file: %s\n", s_infile);

  WabtScript script;
  WabtResult result = wabt_parse_ast(lexer, &script, &s_error_handler);

  if (WABT_SUCCEEDED(result)) {
    result =
        wabt_resolve_names_script(allocator, lexer, &script, &s_error_handler);

    if (WABT_SUCCEEDED(result) && s_validate) {
      result =
          wabt_validate_script(allocator, lexer, &script, &s_error_handler);
    }

    if (WABT_SUCCEEDED(result)) {
      if (s_spec) {
        s_write_binary_spec_options.json_filename = s_outfile;
        s_write_binary_spec_options.write_binary_options =
            s_write_binary_options;
        result = wabt_write_binary_spec_script(allocator, &script, s_infile,
                                               &s_write_binary_spec_options);
      } else {
        WabtMemoryWriter writer;
        WABT_ZERO_MEMORY(writer);
        if (WABT_FAILED(wabt_init_mem_writer(allocator, &writer)))
          WABT_FATAL("unable to open memory writer for writing\n");

        WabtModule* module = wabt_get_first_module(&script);
        if (module) {
          result = wabt_write_binary_module(allocator, &writer.base, module,
                                            &s_write_binary_options);
        } else {
          WABT_FATAL("no module found\n");
        }

        if (WABT_SUCCEEDED(result))
          write_buffer_to_file(s_outfile, &writer.buf);
        wabt_close_mem_writer(&writer);
      }
    }
  }

  wabt_destroy_ast_lexer(lexer);

  if (s_use_libc_allocator)
    wabt_destroy_script(&script);
  wabt_print_allocator_stats(allocator);
  wabt_destroy_allocator(allocator);
  return result;
}
