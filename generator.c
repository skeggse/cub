#include <stdio.h>
#include <string.h>

#include "xalloc.h"
#include "generator.h"

// GENERATION UTILITIES

type *instruction_type(code_block *parent, size_t src) {
  size_t params = parent->parameter_count;
  if (params > src) {
    return parent->parameters[src].field_type;
  }
  src -= params;
  if (parent->instruction_count > src) {
    return parent->instructions[src].type;
  }
  fprintf(stderr, "no such instruction, cannot get type\n");
  return NULL;
}

static void resize(size_t *used, size_t *total, void **ptr, size_t size) {
  const size_t u = *used;
  size_t t = *total;
  if (t == 0) {
    *total = t = 16;
  } else if (u >= t) {
    size_t n = t;
    while (u >= t) {
      const size_t mask = -!(n & (n - 1));
      n = (mask & (n | (n >> 1))) | (((n / 3) << 2) & ~mask);
    }
    *total = t = n;
  } else {
    return;
  }

  void *new_ptr = xrealloc(*ptr, size * t);
  *ptr = new_ptr;
}

static code_block *add_block(code_system *system) {
  printf("add_block\n");
  resize(&system->block_count, &system->block_cap, (void**) &system->blocks,
    sizeof(code_block));

  return &system->blocks[system->block_count++];
}

static code_struct *add_struct(code_system *system) {
  resize(&system->struct_count, &system->struct_cap, (void**) &system->structs,
    sizeof(code_struct));

  return &system->structs[system->struct_count++];
}

code_instruction *add_instruction(code_block *block) {
  resize(&block->instruction_count, &block->instruction_cap,
    (void**) &block->instructions, sizeof(code_instruction));

  return &block->instructions[block->instruction_count++];
}

static symbol_entry *add_symbol(code_block *block, const char *symbol_name,
    size_t instruction) {
  symbol_entry **entry = &block->symbol_head;

  for (; *entry; entry = &(*entry)->next) {
    if (strcmp(symbol_name, (*entry)->symbol_name) == 0) {
      fprintf(stderr, "symbol '%s' already defined\n", symbol_name);
      exit(1);
    }
  }

  block->symbol_count++;

  *entry = xmalloc(sizeof(symbol_entry));
  (*entry)->instruction = instruction;
  (*entry)->next = NULL;
  (*entry)->symbol_name = (char*) symbol_name;
  (*entry)->type = NULL;
  return *entry;
}

static type *resolve_type(code_system *system, type *t) {
  if (t->type == T_ARRAY) {
    resolve_type(system, t->arraytype);
  } else if (t->type == T_OBJECT) {
    t->struct_index = t->classtype->struct_index;
  }
  return t;
}

static code_block *create_block(code_system *system) {
  code_block *block = add_block(system);
  block->system = system;
  block->parameter_count = 0;
  block->parameters = NULL;
  block->symbol_count = 0;
  block->symbol_head = NULL;
  block->stack_size = 0;
  block->stack_head = NULL;
  block->accepts_return = false;
  block->has_return = false;
  block->return_instruction = 0;
  block->instruction_count = 0;
  block->instruction_cap = 0;
  block->instructions = NULL;
  block->is_final = false;
  block->tail.parameter_count = 0;
  block->tail.parameters = NULL;
  return block;
}

static code_block *create_child_block(code_block *parent) {
  size_t params = parent->symbol_count + parent->stack_size +
    parent->has_return;

  code_block *block = add_block(parent->system);
  block->system = parent->system;
  block->parameter_count = params;
  block->parameters = xmalloc(sizeof(code_field) * params);
  block->symbol_count = parent->symbol_count;
  block->stack_size = parent->stack_size;
  block->accepts_return = parent->has_return;
  block->has_return = parent->has_return;
  block->return_instruction = 0;
  block->instruction_count = 0;
  block->instruction_cap = 0;
  block->instructions = NULL;
  block->is_final = false;
  block->tail.parameter_count = 0;
  block->tail.parameters = NULL;

  if (parent->has_return) {
    type *ret_type = instruction_type(parent, parent->return_instruction);
    block->parameters[0].field_type = copy_type(ret_type);
  }

  size_t i = parent->has_return;
  symbol_entry *entry = parent->symbol_head, **tail = &block->symbol_head;
  for (; entry; i++, entry = entry->next) {
    block->parameters[i].field_type = copy_type(entry->type);
    symbol_entry *new = xmalloc(sizeof(*new));
    new->symbol_name = entry->symbol_name;
    new->type = copy_type(entry->type);
    new->instruction = i;
    *tail = new;
    tail = &new->next;
  }
  *tail = NULL;

  // TODO: stack passed in sort of the wrong order
  instruction_node *node = parent->stack_head, **ntail = &block->stack_head;
  for (; node; i++, node = node->next) {
    type *node_type = instruction_type(parent, node->instruction);
    block->parameters[i].field_type = copy_type(node_type);
    instruction_node *new = xmalloc(sizeof(*new));
    new->instruction = i;
    *ntail = new;
    ntail = &new->next;
  }
  *ntail = NULL;

  return block;
}

static size_t get_block_index(code_block *block) {
  return block - block->system->blocks;
}

static type *get_blockref_type(code_block *block) {
  type *t = xmalloc(sizeof(*t));
  t->type = T_BLOCKREF;

  argument *blocktype = NULL;

  size_t params = block->parameter_count;
  if (params) {
    blocktype = xmalloc(sizeof(*blocktype));
    blocktype->symbol_name = NULL;
    blocktype->argument_type = copy_type(block->parameters[0].field_type);
    argument *tail = blocktype;
    for (size_t i = 1; i < params; i++) {
      argument *next = xmalloc(sizeof(*next));
      next->symbol_name = NULL;
      next->argument_type = copy_type(block->parameters[i].field_type);
      tail->next = next;
      tail = next;
    }
    tail->next = NULL;
  }

  t->blocktype = blocktype;

  return t;
}

static type *get_object_type(code_system *system, size_t struct_index) {
  type *t = xmalloc(sizeof(*t));
  t->type = T_OBJECT;
  t->struct_index = struct_index;
  return t;
}

static void set_block_tail_params(code_block *src, code_block *dest) {
  size_t params = dest->parameter_count;
  src->tail.parameter_count = params;
  src->tail.parameters = xmalloc(sizeof(size_t) * params);

  if (dest->accepts_return) {
    if (!src->has_return) {
      fprintf(stderr, "no return struct to forward\n");
      exit(1);
    }

    src->tail.parameters[0] = src->return_instruction;
  }

  size_t i = dest->accepts_return;
  for (symbol_entry *entry = dest->symbol_head; entry; entry = entry->next) {
    src->tail.parameters[i++] = entry->instruction;
  }

  for (instruction_node *node = dest->stack_head; node; node = node->next) {
    src->tail.parameters[i++] = node->instruction;
  }

  if (i != params) {
    fprintf(stderr, "block tail params mismatch\n");
    exit(1);
  }
}

static code_instruction *new_instruction(code_block *parent,
    size_t parameter_count) {
  code_instruction *ins = add_instruction(parent);
  ins->parameters = xmalloc(sizeof(size_t) * parameter_count);
  return ins;
}

static void make_blockref(code_block *src, code_block *dest) {
  code_instruction *get = add_instruction(src);
  get->operation.type = O_BLOCKREF;
  get->type = get_blockref_type(dest);
  get->block_index = get_block_index(dest);
}

static size_t last_instruction(code_block *block) {
  return block->parameter_count + block->instruction_count - 1;
}

static size_t next_instruction(code_block *block) {
  return block->parameter_count + block->instruction_count;
}

static void goto_block(code_block *src, code_block *dest) {
  make_blockref(src, dest);

  src->tail.type = GOTO;
  src->tail.first_block = last_instruction(src);
  set_block_tail_params(src, dest);
}

static void goto_id(code_block *src, size_t dest) {
  goto_block(src, &src->system->blocks[dest]);
}

static void push_stack_from(code_block *block, size_t src) {
  instruction_node *node = xmalloc(sizeof(*node));
  node->instruction = src;
  node->next = block->stack_head;
  block->stack_head = node;
  block->stack_size++;
}

static void push_stack(code_block *block) {
  push_stack_from(block, last_instruction(block));
}

static size_t pop_stack(code_block *block) {
  instruction_node *node = block->stack_head;

  if (node == NULL) {
    fprintf(stderr, "no stack to pop\n");
    exit(1);
  }

  size_t instruction = node->instruction;
  block->stack_head = node->next;
  block->stack_size--;
  free(node);

  return instruction;
}

static size_t peek_stack(code_block *block, size_t from_end) {
  instruction_node *node = block->stack_head;

  if (!node) {
    fprintf(stderr, "no stack to peek at %zu\n", from_end);
    exit(1);
  }

  for (size_t i = 0; i < from_end; i++) {
    node = node->next;
    if (!node) {
      fprintf(stderr, "not enough stack to peek at %zu\n", from_end);
      exit(1);
    }
  }

  return node->instruction;
}

static void mirror_instruction(code_block *parent, size_t src) {
  code_instruction *mirror = new_instruction(parent, 1);
  mirror->operation.type = O_GET_SYMBOL;
  mirror->type = copy_type(instruction_type(parent, src));
  mirror->parameters[0] = src;
}

// HEAD

static code_block *generate_block(code_block*, block_statement*);
static void generate_control(code_block*, control_statement*);
static code_block *generate_define(code_block*, define_statement*);
static code_block *generate_loop(code_block*, loop_statement*);
static code_block *generate_if(code_block*, if_statement*);
static void generate_return(code_block*, return_statement*);
static code_struct *generate_class(code_system*, class*);
static code_struct *generate_class_stub(code_system*, class*);
static code_block *generate_function(code_system*, function*);
static code_block *generate_function_stub(code_system*, function*);
static code_block *generate_expression(code_block*, expression*);
static code_block *generate_call(code_block*, expression*);
static code_block *generate_linear(code_block*, expression*,
  size_t ref_param_count);
static code_block *generate_new(code_block*, expression*);
static code_block *generate_ternary(code_block*, expression*);

// STATEMENT-RELATED GENERATION

static code_block *generate_block(code_block *parent, block_statement *block) {
  for (symbol_entry *c = block->class_head; c; c = c->next) {
    generate_class_stub(parent->system, c->classtype);
  }

  for (symbol_entry *f = block->function_head; f; f = f->next) {
    generate_function_stub(parent->system, f->function);
  }

  for (symbol_entry *c = block->class_head; c; c = c->next) {
    generate_class(parent->system, c->classtype);
  }

  for (symbol_entry *f = block->function_head; f; f = f->next) {
    generate_function(parent->system, f->function);
  }

  code_block *body_block = create_child_block(parent),
    *result_block = create_child_block(parent);

  goto_block(parent, body_block);
  goto_block(body_block, result_block);

  for (statement *node = block->body; node; node = node->next) {
    switch (node->type) {
    case S_BLOCK:
      body_block = generate_block(body_block, (block_statement*) node);
      break;
    case S_BREAK:
    case S_CONTINUE:
      generate_control(body_block, (control_statement*) node);
      body_block = NULL;
      break;
    case S_DEFINE:
      body_block = generate_define(body_block, (define_statement*) node);
      break;
    case S_DO_WHILE:
    case S_WHILE:
      body_block = generate_loop(body_block, (loop_statement*) node);
      break;
    case S_EXPRESSION: {
      expression_statement *express = (expression_statement*) node;
      body_block = generate_expression(body_block, express->value);
    } break;
    case S_IF:
      body_block = generate_if(body_block, (if_statement*) node);
      break;
    case S_RETURN:
      generate_return(body_block, (return_statement*) node);
      body_block = NULL;
      break;
    // already processed
    case S_CLASS:
    // case S_DECLARE:
    case S_FUNCTION:
      break;
    }

    // most recently processed statement terminates abruptly
    if (body_block == NULL) {
      if (node->next) {
        fprintf(stderr, "not reachable\n");
      }

      return NULL;
    }
  }

  return result_block;
}

static void generate_control(code_block *parent, control_statement *control) {
  loop_statement *target = control->target;
  goto_id(parent, control->type == S_BREAK
    ? target->block_tail
    : target->block_head);
}

static code_block *generate_define(code_block *parent,
    define_statement *define) {
  type *define_type = define->symbol_type;
  define_clause *clause = define->clause;

  do {
    parent = generate_expression(parent, clause->value);
    size_t value = last_instruction(parent);

    symbol_entry *entry = add_symbol(parent, clause->symbol_name, value);
    entry->type = copy_type(define_type);
    clause = clause->next;
  } while (clause);

  return parent;
}

static code_block *generate_loop(code_block *parent, loop_statement *loop) {
  code_block *condition, *body, *post;

  size_t condition_block = parent->system->block_count;
  condition = create_child_block(parent);
  body = create_child_block(parent);
  post = create_child_block(parent);

  goto_block(parent, loop->type == S_WHILE ? condition : body);

  // available for the body generation
  loop->block_head = condition_block;
  loop->block_tail = condition_block + 2; // post

  code_block *condition_end = generate_expression(condition, loop->condition);

  condition_end->tail.type = BRANCH;
  condition_end->tail.condition_instruction = last_instruction(condition_end);
  set_block_tail_params(condition_end, post);

  code_instruction *ref;

  ref = add_instruction(condition_end);
  ref->operation.type = O_BLOCKREF;
  ref->type = get_blockref_type(body);
  ref->block_index = condition_block + 1; // body

  condition_end->tail.first_block = last_instruction(condition_end);

  ref = add_instruction(condition_end);
  ref->operation.type = O_BLOCKREF;
  ref->type = get_blockref_type(post);
  ref->block_index = condition_block + 2; // post

  condition_end->tail.second_block = last_instruction(condition_end);

  code_block *body_end = generate_block(body, loop->body);

  if (body_end != NULL) {
    goto_block(body_end, condition);
    loop->tail_used = true;
  }

  if (!loop->tail_used) {
    fprintf(stderr, "post-loop not reachable\n");
  }

  return post;
}

static code_block *generate_if(code_block *parent, if_statement *branch) {
  code_block *first, *second;

  size_t first_block = parent->system->block_count;
  first = create_child_block(parent);
  second = create_child_block(parent);

  parent = generate_expression(parent, branch->condition);

  parent->tail.type = BRANCH;
  parent->tail.parameter_count = parent->symbol_count;
  parent->tail.condition_instruction = last_instruction(parent);

  code_instruction *ref;

  ref = add_instruction(parent);
  ref->operation.type = O_BLOCKREF;
  ref->type = get_blockref_type(first);
  ref->block_index = first_block; // first

  parent->tail.first_block = last_instruction(parent);

  ref = add_instruction(parent);
  ref->operation.type = O_BLOCKREF;
  ref->type = get_blockref_type(second);
  ref->block_index = first_block + 1; // second

  parent->tail.second_block = last_instruction(parent);

  set_block_tail_params(parent, first);

  code_block *first_end, *second_end;
  first_end = branch->first ? generate_block(first, branch->first) : first;
  second_end = branch->second ? generate_block(second, branch->second) : second;

  // reachability check
  if (!first_end && !second_end) {
    return NULL;
  }

  code_block *post = create_child_block(parent);

  if (first_end) {
    goto_block(first_end, post);
  }

  if (second_end) {
    goto_block(second_end, post);
  }

  return post;
}

static void generate_return(code_block *parent, return_statement *ret) {
  function *fn = ret->target;

  if (is_void(fn->return_type) != !ret->value) {
    fprintf(stderr, "return from void function with return or non-void without "
      "return\n");
    abort();
  }

  size_t param_count = ret->value ? 2 : 1;

  parent->tail.type = GOTO;
  parent->tail.parameter_count = param_count;
  parent->tail.parameters = xmalloc(sizeof(size_t) * param_count);

  if (ret->value) {
    parent = generate_expression(parent, ret->value);
    parent->tail.parameters[1] = last_instruction(parent);
  }

  parent->tail.parameters[0] = 0;
  parent->tail.first_block = next_instruction(parent);

  code_instruction *unwrap = new_instruction(parent, 2);
  unwrap->operation.type = O_GET_FIELD;
  unwrap->type = xmalloc(sizeof(type));
  unwrap->type->type = T_BLOCKREF;
  argument *blocktype = xmalloc(sizeof(*blocktype));
  blocktype->symbol_name = NULL;
  blocktype->argument_type = get_object_type(parent->system,
    ret->target->return_struct);
  blocktype->next = fn->argument;
  unwrap->type->blocktype = blocktype;
  unwrap->parameters[0] = 0; // return structs are always the first argument
  unwrap->parameters[1] = 0; // blockref position in all return structs
}

// STRUCTURE AND CLASS GENERATION

static code_struct *generate_class(code_system *system, class *class) {
  code_struct *cstruct = &system->structs[class->struct_index];

  size_t index = 0;
  for (field *entry = class->field; entry; entry = entry->next) {
    type *field_type = resolve_type(system, entry->field_type);
    cstruct->fields[index].field_type = copy_type(field_type);
    ++index;
  }

  return cstruct;
}

static code_struct *generate_class_stub(code_system *system, class *class) {
  code_struct *cstruct = add_struct(system);
  cstruct->field_count = class->field_count;
  cstruct->fields = xmalloc(sizeof(code_field) * class->field_count);

  class->struct_index = system->struct_count - 1;

  return cstruct;
}

static code_block *generate_function(code_system *system, function *fn) {
  code_block *start_block = &system->blocks[fn->block_body];

  code_block *block = generate_block(start_block, fn->body);

  if (block) {
    if (fn->return_type && fn->return_type->type != T_VOID) {
      fprintf(stderr, "execution reaches end of non-void function '%s'\n",
        fn->function_name);
      exit(1);
    }

    // to handle exiting a void function
    return_statement ret = {
      .type = S_RETURN,
      .next = NULL,
      .parent = (statement*) fn->body,
      .value = NULL,
      .target = fn
    };

    generate_return(block, &ret);
  }

  return start_block;
}

static code_block *generate_function_stub(code_system *system, function *fn) {
  size_t struct_index = system->struct_count;

  code_struct *return_struct = add_struct(system);
  return_struct->field_count = 1;
  return_struct->fields = xmalloc(sizeof(code_field));
  return_struct->fields[0].field_type = xmalloc(sizeof(type));
  return_struct->fields[0].field_type->type = T_BLOCKREF;
  argument *blocktype = xmalloc(sizeof(*blocktype));
  blocktype->symbol_name = NULL;
  blocktype->argument_type = get_object_type(system, struct_index);
  blocktype->next = copy_arguments(fn->argument, false);
  return_struct->fields[0].field_type->blocktype = blocktype;

  fn->return_struct = struct_index;

  fn->block_body = system->block_count;
  code_block *start_block = create_block(system);

  size_t handoff_size = fn->argument_count + 1;
  start_block->parameter_count = handoff_size;
  start_block->parameters = xmalloc(sizeof(code_field) * handoff_size);

  start_block->parameters[0].field_type = get_object_type(system, struct_index);
  start_block->has_return = true;
  start_block->return_instruction = 0;

  size_t i = 0;
  for (argument *arg = fn->argument; arg; arg = arg->next) {
    type *arg_type = resolve_type(system, arg->argument_type);
    start_block->parameters[++i].field_type = copy_type(arg_type);
    add_symbol(start_block, arg->symbol_name, i);
  }

  return start_block;
}

// EXPRESSION GENERATION

static code_block *generate_expression(code_block *parent, expression *value) {
  switch (value->operation.type) {
  case O_BITWISE_NOT:
  case O_NEGATE:
  case O_NOT:
    return generate_linear(parent, value, 1);
  case O_BLOCKREF:
  case O_CAST:
  case O_FUNCTION:
    abort();
  case O_CALL:
    return generate_call(parent, value);
  case O_COMPARE:
  case O_GET_INDEX:
  case O_IDENTITY:
  case O_LOGIC:
  case O_NUMERIC:
  case O_SHIFT:
  case O_STR_CONCAT:
    return generate_linear(parent, value, 2);
  case O_GET_FIELD: {
    parent = generate_expression(parent, value->value);

    class *class = value->value->type->classtype;
    code_struct *target = &(parent->system->structs[class->struct_index]);

    code_instruction *get = new_instruction(parent, 2);
    get->operation.type = O_GET_FIELD;
    get->type = target->fields[value->field_index].field_type;
    get->parameters[0] = last_instruction(parent) - 1;
    get->parameters[1] = value->field_index;

    return parent;
  }
  case O_GET_SYMBOL: {
    const char *symbol = value->symbol_name;

    for (symbol_entry *entry = parent->symbol_head; entry;
        entry = entry->next) {
      if (strcmp(entry->symbol_name, symbol) == 0) {
        mirror_instruction(parent, entry->instruction);
        return parent;
      }
    }

    // crap
    fprintf(stderr, "no symbol '%s' post-analysis\n", symbol);
    abort();
  }
  case O_LITERAL: {
    code_instruction *literal = add_instruction(parent);
    literal->operation.type = O_LITERAL;
    literal->type = copy_type(value->type);
    switch (literal->type->type) {
    case T_BOOL: literal->value_bool = value->value_bool; break;
    case T_STRING: literal->value_string = value->value_string; break;
    case T_U8: literal->value_u8 = value->value_u8; break;
    case T_U16: literal->value_u16 = value->value_u16; break;
    case T_U32: literal->value_u32 = value->value_u32; break;
    case T_U64: literal->value_u64 = value->value_u64; break;
    case T_OBJECT:
      // gotta be null
      break;
    default:
      fprintf(stderr, "literal handoff not implemented for this type\n");
      exit(1);
    }

    return parent;
  }
  case O_NEW:
    return generate_new(parent, value);
  case O_NUMERIC_ASSIGN:
  case O_SHIFT_ASSIGN:
  case O_STR_CONCAT_ASSIGN: {
    expression *left = value->value, *right = value->value->next;

    switch (left->operation.type) {
    case O_GET_FIELD:
      parent = generate_expression(parent, left);

      // object
      push_stack_from(parent, last_instruction(parent) - 1);

      // old field value
      push_stack(parent);
      break;
    case O_GET_INDEX: {
      // array
      parent = generate_expression(parent, left->value);
      push_stack(parent);

      // index
      parent = generate_expression(parent, left->value->next);
      push_stack(parent);

      // array value
      code_instruction *get = new_instruction(parent, 2);
      get->operation.type = O_GET_INDEX;
      get->type = left->type;
      get->parameters[0] = peek_stack(parent, 1);
      get->parameters[1] = peek_stack(parent, 0);
      push_stack(parent);
    } break;
    case O_GET_SYMBOL:
      // old value
      parent = generate_expression(parent, left);
      push_stack(parent);
      break;
    default:
      abort();
    }

    // new value/right value
    parent = generate_expression(parent, right);

    code_instruction *compute = new_instruction(parent, 2);

    switch (value->operation.type) {
    case O_NUMERIC_ASSIGN:
      compute->operation.type = O_NUMERIC;
      compute->operation.numeric_type = value->operation.numeric_type;
      break;
    case O_SHIFT_ASSIGN:
      compute->operation.type = O_SHIFT;
      compute->operation.shift_type = value->operation.shift_type;
      break;
    case O_STR_CONCAT_ASSIGN:
      compute->operation.type = O_STR_CONCAT;
      break;
    default:
      abort();
    }

    compute->type = value->type;
    compute->parameters[0] = pop_stack(parent);
    compute->parameters[1] = last_instruction(parent);

    switch (value->operation.type) {
    case O_GET_FIELD: {
      code_instruction *set = new_instruction(parent, 3);
      set->operation.type = O_SET_FIELD;
      set->type = NULL;
      set->parameters[0] = pop_stack(parent);
      set->parameters[1] = left->field_index;
      set->parameters[2] = last_instruction(parent) - 1;

      mirror_instruction(parent, last_instruction(parent) - 1);
    } break;
    case O_GET_INDEX: {
      code_instruction *set = new_instruction(parent, 3);
      set->operation.type = O_SET_INDEX;
      set->type = NULL;
      set->parameters[2] = last_instruction(parent) - 1;
      set->parameters[1] = pop_stack(parent); // index
      set->parameters[0] = pop_stack(parent); // array

      mirror_instruction(parent, last_instruction(parent) - 1);
    } break;
    case O_GET_SYMBOL: {
      const char *symbol = left->symbol_name;

      for (symbol_entry *entry = parent->symbol_head; entry;
          entry = entry->next) {
        if (strcmp(entry->symbol_name, symbol) == 0) {
          entry->instruction = last_instruction(parent);
          return parent;
        }
      }

      fprintf(stderr, "YOU CHECKED THIS :(\n");
      abort();
    }
    default:
      abort();
    }

    return parent;
  }
  case O_SET_FIELD: {
    parent = generate_expression(parent, value->value);
    push_stack(parent);

    parent = generate_expression(parent, value->value->next);

    code_instruction *set = new_instruction(parent, 3);
    set->operation.type = O_SET_FIELD;
    set->type = NULL;
    set->parameters[0] = pop_stack(parent);
    set->parameters[1] = value->field_index;
    set->parameters[2] = last_instruction(parent) - 1;

    mirror_instruction(parent, last_instruction(parent) - 1);

    return parent;
  }
  case O_SET_INDEX: {
    parent = generate_expression(parent, value->value);

    push_stack(parent);

    parent = generate_expression(parent, value->value->next);

    push_stack(parent);

    parent = generate_expression(parent, value->value->next->next);

    code_instruction *set = new_instruction(parent, 3);
    set->operation.type = O_SET_INDEX;
    set->type = NULL;
    set->parameters[2] = pop_stack(parent);
    set->parameters[0] = pop_stack(parent);
    set->parameters[1] = last_instruction(parent) - 1;

    mirror_instruction(parent, last_instruction(parent) - 1);

    return parent;
  }
  case O_SET_SYMBOL: {
    // TODO: revert to using define instead of declare and expression otherwise
    // we'll have a lot of uninitialized symbol_entry.instruction problems
    parent = generate_expression(parent, value->value);

    const char *symbol = value->symbol_name;

    for (symbol_entry *entry = parent->symbol_head; entry;
        entry = entry->next) {
      if (strcmp(entry->symbol_name, symbol) == 0) {
        entry->instruction = last_instruction(parent);
        return parent;
      }
    }

    // crap
    fprintf(stderr, "YOU CHECKED THIS :(\n");
    abort();
  }
  case O_TERNARY:
    return generate_ternary(parent, value);
  }
}

static code_block *generate_call(code_block *parent, expression *value) {
  size_t ref_arg_count = 0;
  for (expression *node = value->value; node; node = node->next) {
    generate_expression(parent, node);
    push_stack(parent);
    ref_arg_count++;
  }

  function *fn = value->function;

  if (ref_arg_count != fn->argument_count) {
    fprintf(stderr, "wrong parameter count to '%s' post-analysis\n",
      fn->function_name);
    abort();
  }

  size_t base_param_count = is_void(fn->return_type) ? 1 : 2;
  size_t context_size = parent->symbol_count + parent->stack_size;
  size_t field_count = context_size + 1;

  code_struct *call_struct = add_struct(parent->system);
  call_struct->field_count = field_count;
  call_struct->fields = xmalloc(sizeof(code_field) * field_count);

  call_struct->fields[0].field_type = xmalloc(sizeof(type));
  call_struct->fields[0].field_type->type = T_BLOCKREF;
  argument *blocktype = xmalloc(sizeof(*blocktype));
  blocktype->symbol_name = NULL;
  blocktype->argument_type = get_object_type(parent->system,
    parent->system->struct_count - 1);
  call_struct->fields[0].field_type->blocktype = blocktype;

  if (base_param_count == 2) {
    blocktype->next = xmalloc(sizeof(argument));
    blocktype = blocktype->next;
    blocktype->symbol_name = NULL;
    blocktype->argument_type = copy_type(fn->return_type);
  }
  blocktype->next = NULL;

  code_block *return_block = create_block(parent->system);
  return_block->parameter_count = base_param_count;
  return_block->parameters = xmalloc(sizeof(code_field) * base_param_count);
  return_block->symbol_count = parent->symbol_count;
  return_block->stack_size = parent->stack_size;
  return_block->instruction_count = 0;

  code_field *param = &return_block->parameters[0];
  param->field_type = get_object_type(parent->system, fn->return_struct);

  if (base_param_count == 2) {
    return_block->parameters[1].field_type = fn->return_type;
  }

  // return struct allocation in parent block
  code_instruction *new = new_instruction(parent, 1);
  new->operation.type = O_NEW;
  new->type = get_object_type(parent->system, parent->system->struct_count - 1);
  new->parameters[0] = parent->system->struct_count - 1;

  size_t object = last_instruction(parent);

  // blockref in parent block
  code_instruction *get = new_instruction(parent, 1);
  get->operation.type = O_BLOCKREF;
  get->type = xmalloc(sizeof(type));
  get->type->type = T_BLOCKREF;
  get->type->blocktype = copy_arguments(blocktype, false);
  get->block_index = parent->system->block_count - 1;

  // blockref field assignment in parent block
  code_instruction *set = new_instruction(parent, 3);
  set->operation.type = O_SET_FIELD;
  set->type = NULL;
  set->parameters[0] = object;
  set->parameters[1] = 0;
  set->parameters[2] = last_instruction(parent) - 1;

  // downcast in return block
  get = new_instruction(return_block, 2);
  get->operation.type = O_CAST;
  get->operation.cast_type = O_DOWNCAST;
  get->type = get_object_type(parent->system, fn->return_struct);
  get->parameters[0] = fn->return_struct;
  get->parameters[1] = 0;

  size_t i = 1;
  symbol_entry **symtail = &return_block->symbol_head;
  for (symbol_entry *entry = parent->symbol_head; entry; entry = entry->next) {
    call_struct->fields[i].field_type = copy_type(entry->type);

    // put the variable into the return struct
    set = new_instruction(parent, 3);
    set->operation.type = O_SET_FIELD;
    set->type = NULL;
    set->parameters[0] = object;
    set->parameters[1] = i; // offset by 1
    set->parameters[2] = entry->instruction;

    // add an entry to the symbol table representing the variable
    symbol_entry *retentry = xmalloc(sizeof(*retentry));
    retentry->symbol_name = entry->symbol_name;
    retentry->type = entry->type;
    retentry->instruction = i + base_param_count; // offset by 1 or 2
    *symtail = retentry;
    symtail = &retentry->next;

    // get the variable from the return struct
    get = new_instruction(return_block, 2);
    get->operation.type = O_GET_FIELD;
    get->type = copy_type(entry->type);
    get->parameters[0] = base_param_count;
    get->parameters[1] = i; // offset by 1

    i++;
  }
  *symtail = NULL;

  instruction_node **stacktail = &return_block->stack_head;
  for (instruction_node *node = parent->stack_head; node; node = node->next) {
    type *node_type = instruction_type(parent, node->instruction);
    call_struct->fields[i].field_type = copy_type(node_type);

    // put the stack entry into the return struct
    set = new_instruction(parent, 3);
    set->operation.type = O_SET_FIELD;
    set->type = NULL;
    set->parameters[0] = object;
    set->parameters[1] = i; // offset by 1
    set->parameters[2] = node->instruction;

    // add a stack entry in the return block
    instruction_node *retnode = xmalloc(sizeof(*retnode));
    retnode->instruction = i + base_param_count; // offset by 2
    *stacktail = retnode;
    stacktail = &retnode->next;

    // get the stack entry from the return struct
    get = new_instruction(return_block, 2);
    get->operation.type = O_GET_FIELD;
    get->type = copy_type(node_type);
    get->parameters[0] = base_param_count;
    get->parameters[1] = i; // offset by 1

    i++;
  }
  *stacktail = NULL;

  code_instruction *cast = new_instruction(parent, 2);
  cast->operation.type = O_CAST;
  cast->operation.cast_type = O_UPCAST;
  cast->type = get_object_type(parent->system, fn->return_struct);
  cast->parameters[0] = fn->return_struct;
  cast->parameters[1] = object;

  object = last_instruction(parent);

  get = new_instruction(parent, 1);
  get->operation.type = O_BLOCKREF;
  get->type = xmalloc(sizeof(type));
  get->type->type = T_BLOCKREF;
  blocktype = xmalloc(sizeof(*blocktype));
  blocktype->symbol_name = NULL;
  blocktype->argument_type = get_object_type(parent->system,
    parent->system->struct_count - 1);
  blocktype->next = copy_arguments(fn->argument, false);
  get->type->blocktype = blocktype;
  get->parameters[0] = fn->block_body;

  size_t handoff_size = ref_arg_count + 1;

  parent->tail.type = GOTO;
  parent->tail.first_block = last_instruction(parent);
  parent->tail.parameter_count = handoff_size;
  parent->tail.parameters = xmalloc(sizeof(size_t) * handoff_size);

  parent->tail.parameters[0] = object;

  for (i = ref_arg_count; i >= 1; i--) {
    parent->tail.parameters[i] = pop_stack(parent);
  }

  return return_block;
}

// TODO: is using the expression value chain good enough?
static code_block *generate_linear(code_block *parent, expression *value,
    size_t ref_param_count) {
  size_t param_count = 0;
  for (expression *p = value->value; p; p = p->next) {
    parent = generate_expression(parent, p);
    push_stack(parent);
    param_count++;
  }

  if (ref_param_count != param_count) {
    fprintf(stderr, "wrong linear parameter count post-analysis\n");
    abort();
  }

  code_instruction *ins = new_instruction(parent, param_count);
  ins->operation = value->operation;
  ins->type = value->type;
  while (param_count-- > 0) {
    ins->parameters[param_count] = pop_stack(parent);
  }

  return parent;
}

// TODO: have a bunch of O_SET_FIELD instead of O_NEW parameters
static code_block *generate_new(code_block *parent, expression *value) {
  size_t param_count = 0;
  for (expression *p = value->value; p; p = p->next) {
    parent = generate_expression(parent, p);
    push_stack(parent);
    param_count++;
  }

  class *class = value->type->classtype;
  size_t field_count = class->field_count;

  if (param_count != field_count) {
    fprintf(stderr, "wrong parameter count to '%s' constructor post-analysis\n",
      class->class_name);
    abort();
  }

  code_instruction *new = new_instruction(parent, 1);
  new->operation.type = O_NEW;
  new->type = xmalloc(sizeof(type));
  new->type->type = O_NEW;
  new->type->struct_index = class->struct_index;
  new->parameters[0] = class->struct_index;

  size_t object = last_instruction(parent);

  for (size_t i = field_count; i > 0; ) {
    size_t value = pop_stack(parent);

    code_instruction *set = new_instruction(parent, 3);
    set->operation.type = O_SET_FIELD;
    set->parameters[0] = object;
    set->parameters[1] = --i;
    set->parameters[2] = value;
    set->type = copy_type(instruction_type(parent, value));
  }

  return parent;
}

static code_block *generate_ternary(code_block *parent, expression *value) {
  generate_expression(parent, value->value);

  code_block *first = create_child_block(parent),
    *second = create_child_block(parent);

  parent->tail.type = BRANCH;
  parent->tail.condition_instruction = last_instruction(parent);
  set_block_tail_params(parent, first);
  make_blockref(parent, first);
  parent->tail.first_block = last_instruction(parent);
  make_blockref(parent, second);
  parent->tail.second_block = last_instruction(parent);

  generate_expression(first, value->value->next);
  push_stack(first);

  generate_expression(second, value->value->next->next);
  push_stack(second);

  code_block *after = create_child_block(first);

  size_t result = pop_stack(after);

  mirror_instruction(after, result);

  goto_block(first, after);
  goto_block(second, after);

  return after;
}

// TOP-LEVEL GENERATION

code_system *generate(block_statement *root) {
  code_system *system = xmalloc(sizeof(*system));
  system->struct_count = 0;
  system->struct_cap = 0;
  system->structs = NULL;
  system->block_count = 0;
  system->block_cap = 0;
  system->blocks = NULL;
  code_block *block = create_block(system);

  code_block *tail_block = generate_block(block, root);

  if (tail_block != NULL) {
    // TODO: add exit code when the final block exists
    tail_block->is_final = true;
  }

  return system;
}
