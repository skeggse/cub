#include "xalloc.h"
#include "type.h"

bool is_void(type *t) {
  return !t || t->type == T_VOID;
}

bool compatible_type(type *left, type *right) {
  if (!left) {
    return (!right) || (right->type == T_VOID);
  }

  switch (left->type) {
  case T_ARRAY:
    return right->type == T_ARRAY && compatible_type(left->arraytype,
      right->arraytype);
  case T_OBJECT:
    return right->type == T_OBJECT && left->classtype == right->classtype;
  case T_STRING:
    return right->type == T_STRING;
  case T_VOID:
    return (!right) || (right->type == T_VOID);
  default:
    // TODO: warn for numeric compatibility
    return true;
  }
}

argument *copy_arguments(argument *arg, bool with_names) {
  if (arg == NULL) {
    return NULL;
  }

  argument *new = xmalloc(sizeof(*new));
  new->symbol_name = with_names ? arg->symbol_name : NULL;
  new->argument_type = copy_type(arg->argument_type);

  argument *tail = new;
  for (argument *a = arg->next; a; a = a->next) {
    argument *rep = xmalloc(sizeof(*rep));
    rep->symbol_name = with_names ? a->symbol_name : NULL;
    rep->argument_type = copy_type(a->argument_type);
    tail->next = rep;
    tail = rep;
  }
  tail->next = NULL;

  return new;
}

type *copy_type(type *t) {
  if (t == NULL) {
    return NULL;
  }

  type *new = xmalloc(sizeof(*new));
  new->type = t->type;

  switch (t->type) {
  case T_ARRAY: new->arraytype = copy_type(t->arraytype); break;
  case T_BLOCKREF: new->blocktype = copy_arguments(t->blocktype, false); break;
  case T_OBJECT:
    new->classtype = t->classtype;
    new->struct_index = t->struct_index;
    break;
  case T_REF: new->symbol_name = t->symbol_name; break;
  default: break;
  }

  return new;
}
