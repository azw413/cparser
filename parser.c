#include <config.h>

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>

#include "diagnostic.h"
#include "format_check.h"
#include "parser.h"
#include "lexer.h"
#include "token_t.h"
#include "types.h"
#include "type_t.h"
#include "type_hash.h"
#include "ast_t.h"
#include "lang_features.h"
#include "warning.h"
#include "adt/bitfiddle.h"
#include "adt/error.h"
#include "adt/array.h"

//#define PRINT_TOKENS
#define MAX_LOOKAHEAD 2

typedef struct {
	declaration_t *old_declaration;
	symbol_t      *symbol;
	unsigned short namespc;
} stack_entry_t;

typedef struct declaration_specifiers_t  declaration_specifiers_t;
struct declaration_specifiers_t {
	source_position_t  source_position;
	unsigned char      storage_class;
	bool               is_inline;
	decl_modifiers_t   decl_modifiers;
	type_t            *type;
};

typedef declaration_t* (*parsed_declaration_func) (declaration_t *declaration);

static token_t             token;
static token_t             lookahead_buffer[MAX_LOOKAHEAD];
static int                 lookahead_bufpos;
static stack_entry_t      *environment_stack = NULL;
static stack_entry_t      *label_stack       = NULL;
static scope_t            *global_scope      = NULL;
static scope_t            *scope             = NULL;
static declaration_t      *last_declaration  = NULL;
static declaration_t      *current_function  = NULL;
static switch_statement_t *current_switch    = NULL;
static statement_t        *current_loop      = NULL;
static goto_statement_t   *goto_first        = NULL;
static goto_statement_t   *goto_last         = NULL;
static label_statement_t  *label_first       = NULL;
static label_statement_t  *label_last        = NULL;
static struct obstack  temp_obst;

/** The current source position. */
#define HERE token.source_position

static type_t *type_valist;

static statement_t *parse_compound_statement(void);
static statement_t *parse_statement(void);

static expression_t *parse_sub_expression(unsigned precedence);
static expression_t *parse_expression(void);
static type_t       *parse_typename(void);

static void parse_compound_type_entries(void);
static declaration_t *parse_declarator(
		const declaration_specifiers_t *specifiers, bool may_be_abstract);
static declaration_t *record_declaration(declaration_t *declaration);

static void semantic_comparison(binary_expression_t *expression);

#define STORAGE_CLASSES     \
	case T_typedef:         \
	case T_extern:          \
	case T_static:          \
	case T_auto:            \
	case T_register:

#define TYPE_QUALIFIERS     \
	case T_const:           \
	case T_restrict:        \
	case T_volatile:        \
	case T_inline:          \
	case T_forceinline:

#ifdef PROVIDE_COMPLEX
#define COMPLEX_SPECIFIERS  \
	case T__Complex:
#define IMAGINARY_SPECIFIERS \
	case T__Imaginary:
#else
#define COMPLEX_SPECIFIERS
#define IMAGINARY_SPECIFIERS
#endif

#define TYPE_SPECIFIERS       \
	case T_void:              \
	case T_char:              \
	case T_short:             \
	case T_int:               \
	case T_long:              \
	case T_float:             \
	case T_double:            \
	case T_signed:            \
	case T_unsigned:          \
	case T__Bool:             \
	case T_struct:            \
	case T_union:             \
	case T_enum:              \
	case T___typeof__:        \
	case T___builtin_va_list: \
	COMPLEX_SPECIFIERS        \
	IMAGINARY_SPECIFIERS

#define DECLARATION_START   \
	STORAGE_CLASSES         \
	TYPE_QUALIFIERS         \
	TYPE_SPECIFIERS

#define TYPENAME_START      \
	TYPE_QUALIFIERS         \
	TYPE_SPECIFIERS

/**
 * Allocate an AST node with given size and
 * initialize all fields with zero.
 */
static void *allocate_ast_zero(size_t size)
{
	void *res = allocate_ast(size);
	memset(res, 0, size);
	return res;
}

static declaration_t *allocate_declaration_zero(void)
{
	declaration_t *declaration = allocate_ast_zero(sizeof(*allocate_declaration_zero()));
	declaration->type = type_error_type;
	return declaration;
}

/**
 * Returns the size of a statement node.
 *
 * @param kind  the statement kind
 */
static size_t get_statement_struct_size(statement_kind_t kind)
{
	static const size_t sizes[] = {
		[STATEMENT_COMPOUND]    = sizeof(compound_statement_t),
		[STATEMENT_RETURN]      = sizeof(return_statement_t),
		[STATEMENT_DECLARATION] = sizeof(declaration_statement_t),
		[STATEMENT_IF]          = sizeof(if_statement_t),
		[STATEMENT_SWITCH]      = sizeof(switch_statement_t),
		[STATEMENT_EXPRESSION]  = sizeof(expression_statement_t),
		[STATEMENT_CONTINUE]    = sizeof(statement_base_t),
		[STATEMENT_BREAK]       = sizeof(statement_base_t),
		[STATEMENT_GOTO]        = sizeof(goto_statement_t),
		[STATEMENT_LABEL]       = sizeof(label_statement_t),
		[STATEMENT_CASE_LABEL]  = sizeof(case_label_statement_t),
		[STATEMENT_WHILE]       = sizeof(while_statement_t),
		[STATEMENT_DO_WHILE]    = sizeof(do_while_statement_t),
		[STATEMENT_FOR]         = sizeof(for_statement_t),
		[STATEMENT_ASM]         = sizeof(asm_statement_t)
	};
	assert(kind <= sizeof(sizes) / sizeof(sizes[0]));
	assert(sizes[kind] != 0);
	return sizes[kind];
}

/**
 * Allocate a statement node of given kind and initialize all
 * fields with zero.
 */
static statement_t *allocate_statement_zero(statement_kind_t kind)
{
	size_t       size = get_statement_struct_size(kind);
	statement_t *res  = allocate_ast_zero(size);

	res->base.kind = kind;
	return res;
}

/**
 * Returns the size of an expression node.
 *
 * @param kind  the expression kind
 */
static size_t get_expression_struct_size(expression_kind_t kind)
{
	static const size_t sizes[] = {
		[EXPR_INVALID]             = sizeof(expression_base_t),
		[EXPR_REFERENCE]           = sizeof(reference_expression_t),
		[EXPR_CONST]               = sizeof(const_expression_t),
		[EXPR_STRING_LITERAL]      = sizeof(string_literal_expression_t),
		[EXPR_WIDE_STRING_LITERAL] = sizeof(wide_string_literal_expression_t),
		[EXPR_CALL]                = sizeof(call_expression_t),
		[EXPR_UNARY_FIRST]         = sizeof(unary_expression_t),
		[EXPR_BINARY_FIRST]        = sizeof(binary_expression_t),
		[EXPR_CONDITIONAL]         = sizeof(conditional_expression_t),
		[EXPR_SELECT]              = sizeof(select_expression_t),
		[EXPR_ARRAY_ACCESS]        = sizeof(array_access_expression_t),
		[EXPR_SIZEOF]              = sizeof(typeprop_expression_t),
		[EXPR_ALIGNOF]             = sizeof(typeprop_expression_t),
		[EXPR_CLASSIFY_TYPE]       = sizeof(classify_type_expression_t),
		[EXPR_FUNCTION]            = sizeof(string_literal_expression_t),
		[EXPR_PRETTY_FUNCTION]     = sizeof(string_literal_expression_t),
		[EXPR_BUILTIN_SYMBOL]      = sizeof(builtin_symbol_expression_t),
		[EXPR_BUILTIN_CONSTANT_P]  = sizeof(builtin_constant_expression_t),
		[EXPR_BUILTIN_PREFETCH]    = sizeof(builtin_prefetch_expression_t),
		[EXPR_OFFSETOF]            = sizeof(offsetof_expression_t),
		[EXPR_VA_START]            = sizeof(va_start_expression_t),
		[EXPR_VA_ARG]              = sizeof(va_arg_expression_t),
		[EXPR_STATEMENT]           = sizeof(statement_expression_t),
	};
	if(kind >= EXPR_UNARY_FIRST && kind <= EXPR_UNARY_LAST) {
		return sizes[EXPR_UNARY_FIRST];
	}
	if(kind >= EXPR_BINARY_FIRST && kind <= EXPR_BINARY_LAST) {
		return sizes[EXPR_BINARY_FIRST];
	}
	assert(kind <= sizeof(sizes) / sizeof(sizes[0]));
	assert(sizes[kind] != 0);
	return sizes[kind];
}

/**
 * Allocate an expression node of given kind and initialize all
 * fields with zero.
 */
static expression_t *allocate_expression_zero(expression_kind_t kind)
{
	size_t        size = get_expression_struct_size(kind);
	expression_t *res  = allocate_ast_zero(size);

	res->base.kind = kind;
	res->base.type = type_error_type;
	return res;
}

/**
 * Returns the size of a type node.
 *
 * @param kind  the type kind
 */
static size_t get_type_struct_size(type_kind_t kind)
{
	static const size_t sizes[] = {
		[TYPE_ATOMIC]          = sizeof(atomic_type_t),
		[TYPE_BITFIELD]        = sizeof(bitfield_type_t),
		[TYPE_COMPOUND_STRUCT] = sizeof(compound_type_t),
		[TYPE_COMPOUND_UNION]  = sizeof(compound_type_t),
		[TYPE_ENUM]            = sizeof(enum_type_t),
		[TYPE_FUNCTION]        = sizeof(function_type_t),
		[TYPE_POINTER]         = sizeof(pointer_type_t),
		[TYPE_ARRAY]           = sizeof(array_type_t),
		[TYPE_BUILTIN]         = sizeof(builtin_type_t),
		[TYPE_TYPEDEF]         = sizeof(typedef_type_t),
		[TYPE_TYPEOF]          = sizeof(typeof_type_t),
	};
	assert(sizeof(sizes) / sizeof(sizes[0]) == (int) TYPE_TYPEOF + 1);
	assert(kind <= TYPE_TYPEOF);
	assert(sizes[kind] != 0);
	return sizes[kind];
}

/**
 * Allocate a type node of given kind and initialize all
 * fields with zero.
 */
static type_t *allocate_type_zero(type_kind_t kind)
{
	size_t  size = get_type_struct_size(kind);
	type_t *res  = obstack_alloc(type_obst, size);
	memset(res, 0, size);

	res->base.kind = kind;
	return res;
}

/**
 * Returns the size of an initializer node.
 *
 * @param kind  the initializer kind
 */
static size_t get_initializer_size(initializer_kind_t kind)
{
	static const size_t sizes[] = {
		[INITIALIZER_VALUE]       = sizeof(initializer_value_t),
		[INITIALIZER_STRING]      = sizeof(initializer_string_t),
		[INITIALIZER_WIDE_STRING] = sizeof(initializer_wide_string_t),
		[INITIALIZER_LIST]        = sizeof(initializer_list_t)
	};
	assert(kind < sizeof(sizes) / sizeof(*sizes));
	assert(sizes[kind] != 0);
	return sizes[kind];
}

/**
 * Allocate an initializer node of given kind and initialize all
 * fields with zero.
 */
static initializer_t *allocate_initializer_zero(initializer_kind_t kind)
{
	initializer_t *result = allocate_ast_zero(get_initializer_size(kind));
	result->kind          = kind;

	return result;
}

/**
 * Free a type from the type obstack.
 */
static void free_type(void *type)
{
	obstack_free(type_obst, type);
}

/**
 * Returns the index of the top element of the environment stack.
 */
static size_t environment_top(void)
{
	return ARR_LEN(environment_stack);
}

/**
 * Returns the index of the top element of the label stack.
 */
static size_t label_top(void)
{
	return ARR_LEN(label_stack);
}


/**
 * Return the next token.
 */
static inline void next_token(void)
{
	token                              = lookahead_buffer[lookahead_bufpos];
	lookahead_buffer[lookahead_bufpos] = lexer_token;
	lexer_next_token();

	lookahead_bufpos = (lookahead_bufpos+1) % MAX_LOOKAHEAD;

#ifdef PRINT_TOKENS
	print_token(stderr, &token);
	fprintf(stderr, "\n");
#endif
}

/**
 * Return the next token with a given lookahead.
 */
static inline const token_t *look_ahead(int num)
{
	assert(num > 0 && num <= MAX_LOOKAHEAD);
	int pos = (lookahead_bufpos+num-1) % MAX_LOOKAHEAD;
	return &lookahead_buffer[pos];
}

#define eat(token_type)  do { assert(token.type == token_type); next_token(); } while(0)

/**
 * Report a parse error because an expected token was not found.
 */
static void parse_error_expected(const char *message, ...)
{
	if(message != NULL) {
		errorf(HERE, "%s", message);
	}
	va_list ap;
	va_start(ap, message);
	errorf(HERE, "got '%K', expected %#k", &token, &ap, ", ");
	va_end(ap);
}

/**
 * Report a type error.
 */
static void type_error(const char *msg, const source_position_t source_position,
                       type_t *type)
{
	errorf(source_position, "%s, but found type '%T'", msg, type);
}

/**
 * Report an incompatible type.
 */
static void type_error_incompatible(const char *msg,
		const source_position_t source_position, type_t *type1, type_t *type2)
{
	errorf(source_position, "%s, incompatible types: '%T' - '%T'", msg, type1, type2);
}

/**
 * Eat an complete block, ie. '{ ... }'.
 */
static void eat_block(void)
{
	if(token.type == '{')
		next_token();

	while(token.type != '}') {
		if(token.type == T_EOF)
			return;
		if(token.type == '{') {
			eat_block();
			continue;
		}
		next_token();
	}
	eat('}');
}

/**
 * Eat a statement until an ';' token.
 */
static void eat_statement(void)
{
	while(token.type != ';') {
		if(token.type == T_EOF)
			return;
		if(token.type == '}')
			return;
		if(token.type == '{') {
			eat_block();
			continue;
		}
		next_token();
	}
	eat(';');
}

/**
 * Eat a parenthesed term, ie. '( ... )'.
 */
static void eat_paren(void)
{
	if(token.type == '(')
		next_token();

	while(token.type != ')') {
		if(token.type == T_EOF)
			return;
		if(token.type == ')' || token.type == ';' || token.type == '}') {
			return;
		}
		if(token.type == '(') {
			eat_paren();
			continue;
		}
		if(token.type == '{') {
			eat_block();
			continue;
		}
		next_token();
	}
	eat(')');
}

#define expect(expected)                           \
    if(UNLIKELY(token.type != (expected))) {       \
        parse_error_expected(NULL, (expected), 0); \
        eat_statement();                           \
        return NULL;                               \
    }                                              \
    next_token();

#define expect_block(expected)                     \
    if(UNLIKELY(token.type != (expected))) {       \
        parse_error_expected(NULL, (expected), 0); \
        eat_block();                               \
        return NULL;                               \
    }                                              \
    next_token();

#define expect_void(expected)                      \
    if(UNLIKELY(token.type != (expected))) {       \
        parse_error_expected(NULL, (expected), 0); \
        eat_statement();                           \
        return;                                    \
    }                                              \
    next_token();

static void set_scope(scope_t *new_scope)
{
	scope = new_scope;

	last_declaration = new_scope->declarations;
	if(last_declaration != NULL) {
		while(last_declaration->next != NULL) {
			last_declaration = last_declaration->next;
		}
	}
}

/**
 * Search a symbol in a given namespace and returns its declaration or
 * NULL if this symbol was not found.
 */
static declaration_t *get_declaration(const symbol_t *const symbol, const namespace_t namespc)
{
	declaration_t *declaration = symbol->declaration;
	for( ; declaration != NULL; declaration = declaration->symbol_next) {
		if(declaration->namespc == namespc)
			return declaration;
	}

	return NULL;
}

/**
 * pushs an environment_entry on the environment stack and links the
 * corresponding symbol to the new entry
 */
static void stack_push(stack_entry_t **stack_ptr, declaration_t *declaration)
{
	symbol_t    *symbol    = declaration->symbol;
	namespace_t  namespc = (namespace_t)declaration->namespc;

	/* remember old declaration */
	stack_entry_t entry;
	entry.symbol          = symbol;
	entry.old_declaration = symbol->declaration;
	entry.namespc         = (unsigned short) namespc;
	ARR_APP1(stack_entry_t, *stack_ptr, entry);

	/* replace/add declaration into declaration list of the symbol */
	if(symbol->declaration == NULL) {
		symbol->declaration = declaration;
	} else {
		declaration_t *iter_last = NULL;
		declaration_t *iter      = symbol->declaration;
		for( ; iter != NULL; iter_last = iter, iter = iter->symbol_next) {
			/* replace an entry? */
			if(iter->namespc == namespc) {
				if(iter_last == NULL) {
					symbol->declaration = declaration;
				} else {
					iter_last->symbol_next = declaration;
				}
				declaration->symbol_next = iter->symbol_next;
				break;
			}
		}
		if(iter == NULL) {
			assert(iter_last->symbol_next == NULL);
			iter_last->symbol_next = declaration;
		}
	}
}

static void environment_push(declaration_t *declaration)
{
	assert(declaration->source_position.input_name != NULL);
	assert(declaration->parent_scope != NULL);
	stack_push(&environment_stack, declaration);
}

static void label_push(declaration_t *declaration)
{
	declaration->parent_scope = &current_function->scope;
	stack_push(&label_stack, declaration);
}

/**
 * pops symbols from the environment stack until @p new_top is the top element
 */
static void stack_pop_to(stack_entry_t **stack_ptr, size_t new_top)
{
	stack_entry_t *stack = *stack_ptr;
	size_t         top   = ARR_LEN(stack);
	size_t         i;

	assert(new_top <= top);
	if(new_top == top)
		return;

	for(i = top; i > new_top; --i) {
		stack_entry_t *entry = &stack[i - 1];

		declaration_t *old_declaration = entry->old_declaration;
		symbol_t      *symbol          = entry->symbol;
		namespace_t    namespc         = (namespace_t)entry->namespc;

		/* replace/remove declaration */
		declaration_t *declaration = symbol->declaration;
		assert(declaration != NULL);
		if(declaration->namespc == namespc) {
			if(old_declaration == NULL) {
				symbol->declaration = declaration->symbol_next;
			} else {
				symbol->declaration = old_declaration;
			}
		} else {
			declaration_t *iter_last = declaration;
			declaration_t *iter      = declaration->symbol_next;
			for( ; iter != NULL; iter_last = iter, iter = iter->symbol_next) {
				/* replace an entry? */
				if(iter->namespc == namespc) {
					assert(iter_last != NULL);
					iter_last->symbol_next = old_declaration;
					old_declaration->symbol_next = iter->symbol_next;
					break;
				}
			}
			assert(iter != NULL);
		}
	}

	ARR_SHRINKLEN(*stack_ptr, (int) new_top);
}

static void environment_pop_to(size_t new_top)
{
	stack_pop_to(&environment_stack, new_top);
}

static void label_pop_to(size_t new_top)
{
	stack_pop_to(&label_stack, new_top);
}


static int get_rank(const type_t *type)
{
	assert(!is_typeref(type));
	/* The C-standard allows promoting to int or unsigned int (see § 7.2.2
	 * and esp. footnote 108). However we can't fold constants (yet), so we
	 * can't decide whether unsigned int is possible, while int always works.
	 * (unsigned int would be preferable when possible... for stuff like
	 *  struct { enum { ... } bla : 4; } ) */
	if(type->kind == TYPE_ENUM)
		return ATOMIC_TYPE_INT;

	assert(type->kind == TYPE_ATOMIC);
	return type->atomic.akind;
}

static type_t *promote_integer(type_t *type)
{
	if(type->kind == TYPE_BITFIELD)
		type = type->bitfield.base;

	if(get_rank(type) < ATOMIC_TYPE_INT)
		type = type_int;

	return type;
}

/**
 * Create a cast expression.
 *
 * @param expression  the expression to cast
 * @param dest_type   the destination type
 */
static expression_t *create_cast_expression(expression_t *expression,
                                            type_t *dest_type)
{
	expression_t *cast = allocate_expression_zero(EXPR_UNARY_CAST_IMPLICIT);

	cast->unary.value = expression;
	cast->base.type   = dest_type;

	return cast;
}

/**
 * Check if a given expression represents the 0 pointer constant.
 */
static bool is_null_pointer_constant(const expression_t *expression)
{
	/* skip void* cast */
	if(expression->kind == EXPR_UNARY_CAST
			|| expression->kind == EXPR_UNARY_CAST_IMPLICIT) {
		expression = expression->unary.value;
	}

	/* TODO: not correct yet, should be any constant integer expression
	 * which evaluates to 0 */
	if (expression->kind != EXPR_CONST)
		return false;

	type_t *const type = skip_typeref(expression->base.type);
	if (!is_type_integer(type))
		return false;

	return expression->conste.v.int_value == 0;
}

/**
 * Create an implicit cast expression.
 *
 * @param expression  the expression to cast
 * @param dest_type   the destination type
 */
static expression_t *create_implicit_cast(expression_t *expression,
                                          type_t *dest_type)
{
	type_t *const source_type = expression->base.type;

	if (source_type == dest_type)
		return expression;

	return create_cast_expression(expression, dest_type);
}

/** Implements the rules from § 6.5.16.1 */
static type_t *semantic_assign(type_t *orig_type_left,
                            const expression_t *const right,
                            const char *context)
{
	type_t *const orig_type_right = right->base.type;
	type_t *const type_left       = skip_typeref(orig_type_left);
	type_t *const type_right      = skip_typeref(orig_type_right);

	if ((is_type_arithmetic(type_left) && is_type_arithmetic(type_right)) ||
	    (is_type_pointer(type_left) && is_null_pointer_constant(right)) ||
	    (is_type_atomic(type_left, ATOMIC_TYPE_BOOL)
	     	&& is_type_pointer(type_right))) {
		return orig_type_left;
	}

	if (is_type_pointer(type_left) && is_type_pointer(type_right)) {
		type_t *points_to_left  = skip_typeref(type_left->pointer.points_to);
		type_t *points_to_right = skip_typeref(type_right->pointer.points_to);

		/* the left type has all qualifiers from the right type */
		unsigned missing_qualifiers
			= points_to_right->base.qualifiers & ~points_to_left->base.qualifiers;
		if(missing_qualifiers != 0) {
			errorf(HERE, "destination type '%T' in %s from type '%T' lacks qualifiers '%Q' in pointed-to type", type_left, context, type_right, missing_qualifiers);
			return orig_type_left;
		}

		points_to_left  = get_unqualified_type(points_to_left);
		points_to_right = get_unqualified_type(points_to_right);

		if(!is_type_atomic(points_to_left, ATOMIC_TYPE_VOID)
				&& !is_type_atomic(points_to_right, ATOMIC_TYPE_VOID)
				&& !types_compatible(points_to_left, points_to_right)) {
			return NULL;
		}

		return orig_type_left;
	}

	if (is_type_compound(type_left)  && is_type_compound(type_right)) {
		type_t *const unqual_type_left  = get_unqualified_type(type_left);
		type_t *const unqual_type_right = get_unqualified_type(type_right);
		if (types_compatible(unqual_type_left, unqual_type_right)) {
			return orig_type_left;
		}
	}

	if (!is_type_valid(type_left))
		return type_left;

	if (!is_type_valid(type_right))
		return orig_type_right;

	return NULL;
}

static expression_t *parse_constant_expression(void)
{
	/* start parsing at precedence 7 (conditional expression) */
	expression_t *result = parse_sub_expression(7);

	if(!is_constant_expression(result)) {
		errorf(result->base.source_position, "expression '%E' is not constant\n", result);
	}

	return result;
}

static expression_t *parse_assignment_expression(void)
{
	/* start parsing at precedence 2 (assignment expression) */
	return parse_sub_expression(2);
}

static type_t *make_global_typedef(const char *name, type_t *type)
{
	symbol_t *const symbol       = symbol_table_insert(name);

	declaration_t *const declaration = allocate_declaration_zero();
	declaration->namespc         = NAMESPACE_NORMAL;
	declaration->storage_class   = STORAGE_CLASS_TYPEDEF;
	declaration->type            = type;
	declaration->symbol          = symbol;
	declaration->source_position = builtin_source_position;

	record_declaration(declaration);

	type_t *typedef_type               = allocate_type_zero(TYPE_TYPEDEF);
	typedef_type->typedeft.declaration = declaration;

	return typedef_type;
}

static string_t parse_string_literals(void)
{
	assert(token.type == T_STRING_LITERAL);
	string_t result = token.v.string;

	next_token();

	while (token.type == T_STRING_LITERAL) {
		result = concat_strings(&result, &token.v.string);
		next_token();
	}

	return result;
}

static void parse_attributes(void)
{
	while(true) {
		switch(token.type) {
		case T___attribute__: {
			next_token();

			expect_void('(');
			int depth = 1;
			while(depth > 0) {
				switch(token.type) {
				case T_EOF:
					errorf(HERE, "EOF while parsing attribute");
					break;
				case '(':
					next_token();
					depth++;
					break;
				case ')':
					next_token();
					depth--;
					break;
				default:
					next_token();
				}
			}
			break;
		}
		case T_asm:
			next_token();
			expect_void('(');
			if(token.type != T_STRING_LITERAL) {
				parse_error_expected("while parsing assembler attribute",
				                     T_STRING_LITERAL);
				eat_paren();
				break;
			} else {
				parse_string_literals();
			}
			expect_void(')');
			break;
		default:
			goto attributes_finished;
		}
	}

attributes_finished:
	;
}

#if 0
static designator_t *parse_designation(void)
{
	if(token.type != '[' && token.type != '.')
		return NULL;

	designator_t *result = NULL;
	designator_t *last   = NULL;

	while(1) {
		designator_t *designator;
		switch(token.type) {
		case '[':
			designator = allocate_ast_zero(sizeof(designator[0]));
			next_token();
			designator->array_access = parse_constant_expression();
			expect(']');
			break;
		case '.':
			designator = allocate_ast_zero(sizeof(designator[0]));
			next_token();
			if(token.type != T_IDENTIFIER) {
				parse_error_expected("while parsing designator",
				                     T_IDENTIFIER, 0);
				return NULL;
			}
			designator->symbol = token.v.symbol;
			next_token();
			break;
		default:
			expect('=');
			return result;
		}

		assert(designator != NULL);
		if(last != NULL) {
			last->next = designator;
		} else {
			result = designator;
		}
		last = designator;
	}
}
#endif

static initializer_t *initializer_from_string(array_type_t *type,
                                              const string_t *const string)
{
	/* TODO: check len vs. size of array type */
	(void) type;

	initializer_t *initializer = allocate_initializer_zero(INITIALIZER_STRING);
	initializer->string.string = *string;

	return initializer;
}

static initializer_t *initializer_from_wide_string(array_type_t *const type,
                                                   wide_string_t *const string)
{
	/* TODO: check len vs. size of array type */
	(void) type;

	initializer_t *const initializer =
		allocate_initializer_zero(INITIALIZER_WIDE_STRING);
	initializer->wide_string.string = *string;

	return initializer;
}

static initializer_t *initializer_from_expression(type_t *type,
                                                  expression_t *expression)
{
	/* TODO check that expression is a constant expression */

	/* § 6.7.8.14/15 char array may be initialized by string literals */
	type_t *const expr_type = expression->base.type;
	if (is_type_array(type) && expr_type->kind == TYPE_POINTER) {
		array_type_t *const array_type   = &type->array;
		type_t       *const element_type = skip_typeref(array_type->element_type);

		if (element_type->kind == TYPE_ATOMIC) {
			switch (expression->kind) {
				case EXPR_STRING_LITERAL:
					if (element_type->atomic.akind == ATOMIC_TYPE_CHAR) {
						return initializer_from_string(array_type,
							&expression->string.value);
					}

				case EXPR_WIDE_STRING_LITERAL: {
					type_t *bare_wchar_type = skip_typeref(type_wchar_t);
					if (get_unqualified_type(element_type) == bare_wchar_type) {
						return initializer_from_wide_string(array_type,
							&expression->wide_string.value);
					}
				}

				default:
					break;
			}
		}
	}

	type_t *const res_type = semantic_assign(type, expression, "initializer");
	if (res_type == NULL)
		return NULL;

	initializer_t *const result = allocate_initializer_zero(INITIALIZER_VALUE);
	result->value.value = create_implicit_cast(expression, res_type);

	return result;
}

static initializer_t *parse_sub_initializer(type_t *type,
                                            expression_t *expression);

static initializer_t *parse_sub_initializer_elem(type_t *type)
{
	if(token.type == '{') {
		return parse_sub_initializer(type, NULL);
	}

	expression_t *expression = parse_assignment_expression();
	return parse_sub_initializer(type, expression);
}

static bool had_initializer_brace_warning;

static void skip_designator(void)
{
	while(1) {
		if(token.type == '.') {
			next_token();
			if(token.type == T_IDENTIFIER)
				next_token();
		} else if(token.type == '[') {
			next_token();
			parse_constant_expression();
			if(token.type == ']')
				next_token();
		} else {
			break;
		}
	}
}

static initializer_t *parse_sub_initializer(type_t *type,
                                            expression_t *expression)
{
	if(is_type_scalar(type)) {
		/* there might be extra {} hierarchies */
		if(token.type == '{') {
			next_token();
			if(!had_initializer_brace_warning) {
				warningf(HERE, "braces around scalar initializer");
				had_initializer_brace_warning = true;
			}
			initializer_t *result = parse_sub_initializer(type, NULL);
			if(token.type == ',') {
				next_token();
				/* TODO: warn about excessive elements */
			}
			expect_block('}');
			return result;
		}

		if(expression == NULL) {
			expression = parse_assignment_expression();
		}
		return initializer_from_expression(type, expression);
	}

	/* does the expression match the currently looked at object to initialize */
	if(expression != NULL) {
		initializer_t *result = initializer_from_expression(type, expression);
		if(result != NULL)
			return result;
	}

	bool read_paren = false;
	if(token.type == '{') {
		next_token();
		read_paren = true;
	}

	/* descend into subtype */
	initializer_t  *result = NULL;
	initializer_t **elems;
	if(is_type_array(type)) {
		if(token.type == '.') {
			errorf(HERE,
			       "compound designator in initializer for array type '%T'",
			       type);
			skip_designator();
		}

		type_t *const element_type = skip_typeref(type->array.element_type);

		initializer_t *sub;
		had_initializer_brace_warning = false;
		if(expression == NULL) {
			sub = parse_sub_initializer_elem(element_type);
		} else {
			sub = parse_sub_initializer(element_type, expression);
		}

		/* didn't match the subtypes -> try the parent type */
		if(sub == NULL) {
			assert(!read_paren);
			return NULL;
		}

		elems = NEW_ARR_F(initializer_t*, 0);
		ARR_APP1(initializer_t*, elems, sub);

		while(true) {
			if(token.type == '}')
				break;
			expect_block(',');
			if(token.type == '}')
				break;

			sub = parse_sub_initializer_elem(element_type);
			if(sub == NULL) {
				/* TODO error, do nicer cleanup */
				errorf(HERE, "member initializer didn't match");
				DEL_ARR_F(elems);
				return NULL;
			}
			ARR_APP1(initializer_t*, elems, sub);
		}
	} else {
		assert(is_type_compound(type));
		scope_t *const scope = &type->compound.declaration->scope;

		if(token.type == '[') {
			errorf(HERE,
			       "array designator in initializer for compound type '%T'",
			       type);
			skip_designator();
		}

		declaration_t *first = scope->declarations;
		if(first == NULL)
			return NULL;
		type_t *first_type = first->type;
		first_type         = skip_typeref(first_type);

		initializer_t *sub;
		had_initializer_brace_warning = false;
		if(expression == NULL) {
			sub = parse_sub_initializer_elem(first_type);
		} else {
			sub = parse_sub_initializer(first_type, expression);
		}

		/* didn't match the subtypes -> try our parent type */
		if(sub == NULL) {
			assert(!read_paren);
			return NULL;
		}

		elems = NEW_ARR_F(initializer_t*, 0);
		ARR_APP1(initializer_t*, elems, sub);

		declaration_t *iter  = first->next;
		for( ; iter != NULL; iter = iter->next) {
			if(iter->symbol == NULL)
				continue;
			if(iter->namespc != NAMESPACE_NORMAL)
				continue;

			if(token.type == '}')
				break;
			expect_block(',');
			if(token.type == '}')
				break;

			type_t *iter_type = iter->type;
			iter_type         = skip_typeref(iter_type);

			sub = parse_sub_initializer_elem(iter_type);
			if(sub == NULL) {
				/* TODO error, do nicer cleanup */
				errorf(HERE, "member initializer didn't match");
				DEL_ARR_F(elems);
				return NULL;
			}
			ARR_APP1(initializer_t*, elems, sub);
		}
	}

	int    len        = ARR_LEN(elems);
	size_t elems_size = sizeof(initializer_t*) * len;

	initializer_list_t *init = allocate_ast_zero(sizeof(init[0]) + elems_size);

	init->initializer.kind = INITIALIZER_LIST;
	init->len              = len;
	memcpy(init->initializers, elems, elems_size);
	DEL_ARR_F(elems);

	result = (initializer_t*) init;

	if(read_paren) {
		if(token.type == ',')
			next_token();
		expect('}');
	}
	return result;
}

static initializer_t *parse_initializer(type_t *const orig_type)
{
	initializer_t *result;

	type_t *const type = skip_typeref(orig_type);

	if(token.type != '{') {
		expression_t  *expression  = parse_assignment_expression();
		initializer_t *initializer = initializer_from_expression(type, expression);
		if(initializer == NULL) {
			errorf(HERE,
				"initializer expression '%E' of type '%T' is incompatible with type '%T'",
				expression, expression->base.type, orig_type);
		}
		return initializer;
	}

	if(is_type_scalar(type)) {
		/* § 6.7.8.11 */
		eat('{');

		expression_t *expression = parse_assignment_expression();
		result = initializer_from_expression(type, expression);

		if(token.type == ',')
			next_token();

		expect('}');
		return result;
	} else {
		result = parse_sub_initializer(type, NULL);
	}

	return result;
}

static declaration_t *append_declaration(declaration_t *declaration);

static declaration_t *parse_compound_type_specifier(bool is_struct)
{
	if(is_struct) {
		eat(T_struct);
	} else {
		eat(T_union);
	}

	symbol_t      *symbol      = NULL;
	declaration_t *declaration = NULL;

	if (token.type == T___attribute__) {
		/* TODO */
		parse_attributes();
	}

	if(token.type == T_IDENTIFIER) {
		symbol = token.v.symbol;
		next_token();

		if(is_struct) {
			declaration = get_declaration(symbol, NAMESPACE_STRUCT);
		} else {
			declaration = get_declaration(symbol, NAMESPACE_UNION);
		}
	} else if(token.type != '{') {
		if(is_struct) {
			parse_error_expected("while parsing struct type specifier",
			                     T_IDENTIFIER, '{', 0);
		} else {
			parse_error_expected("while parsing union type specifier",
			                     T_IDENTIFIER, '{', 0);
		}

		return NULL;
	}

	if(declaration == NULL) {
		declaration = allocate_declaration_zero();
		declaration->namespc         =
			(is_struct ? NAMESPACE_STRUCT : NAMESPACE_UNION);
		declaration->source_position = token.source_position;
		declaration->symbol          = symbol;
		declaration->parent_scope  = scope;
		if (symbol != NULL) {
			environment_push(declaration);
		}
		append_declaration(declaration);
	}

	if(token.type == '{') {
		if(declaration->init.is_defined) {
			assert(symbol != NULL);
			errorf(HERE, "multiple definition of '%s %Y'",
			       is_struct ? "struct" : "union", symbol);
			declaration->scope.declarations = NULL;
		}
		declaration->init.is_defined = true;

		int       top        = environment_top();
		scope_t  *last_scope = scope;
		set_scope(&declaration->scope);

		parse_compound_type_entries();
		parse_attributes();

		assert(scope == &declaration->scope);
		set_scope(last_scope);
		environment_pop_to(top);
	}

	return declaration;
}

static void parse_enum_entries(type_t *const enum_type)
{
	eat('{');

	if(token.type == '}') {
		next_token();
		errorf(HERE, "empty enum not allowed");
		return;
	}

	do {
		if(token.type != T_IDENTIFIER) {
			parse_error_expected("while parsing enum entry", T_IDENTIFIER, 0);
			eat_block();
			return;
		}

		declaration_t *const entry = allocate_declaration_zero();
		entry->storage_class   = STORAGE_CLASS_ENUM_ENTRY;
		entry->type            = enum_type;
		entry->symbol          = token.v.symbol;
		entry->source_position = token.source_position;
		next_token();

		if(token.type == '=') {
			next_token();
			entry->init.enum_value = parse_constant_expression();

			/* TODO semantic */
		}

		record_declaration(entry);

		if(token.type != ',')
			break;
		next_token();
	} while(token.type != '}');

	expect_void('}');
}

static type_t *parse_enum_specifier(void)
{
	eat(T_enum);

	declaration_t *declaration;
	symbol_t      *symbol;

	if(token.type == T_IDENTIFIER) {
		symbol = token.v.symbol;
		next_token();

		declaration = get_declaration(symbol, NAMESPACE_ENUM);
	} else if(token.type != '{') {
		parse_error_expected("while parsing enum type specifier",
		                     T_IDENTIFIER, '{', 0);
		return NULL;
	} else {
		declaration = NULL;
		symbol      = NULL;
	}

	if(declaration == NULL) {
		declaration = allocate_declaration_zero();
		declaration->namespc         = NAMESPACE_ENUM;
		declaration->source_position = token.source_position;
		declaration->symbol          = symbol;
		declaration->parent_scope  = scope;
	}

	type_t *const type      = allocate_type_zero(TYPE_ENUM);
	type->enumt.declaration = declaration;

	if(token.type == '{') {
		if(declaration->init.is_defined) {
			errorf(HERE, "multiple definitions of enum %Y", symbol);
		}
		if (symbol != NULL) {
			environment_push(declaration);
		}
		append_declaration(declaration);
		declaration->init.is_defined = 1;

		parse_enum_entries(type);
		parse_attributes();
	}

	return type;
}

/**
 * if a symbol is a typedef to another type, return true
 */
static bool is_typedef_symbol(symbol_t *symbol)
{
	const declaration_t *const declaration =
		get_declaration(symbol, NAMESPACE_NORMAL);
	return
		declaration != NULL &&
		declaration->storage_class == STORAGE_CLASS_TYPEDEF;
}

static type_t *parse_typeof(void)
{
	eat(T___typeof__);

	type_t *type;

	expect('(');

	expression_t *expression  = NULL;

restart:
	switch(token.type) {
	case T___extension__:
		/* this can be a prefix to a typename or an expression */
		/* we simply eat it now. */
		do {
			next_token();
		} while(token.type == T___extension__);
		goto restart;

	case T_IDENTIFIER:
		if(is_typedef_symbol(token.v.symbol)) {
			type = parse_typename();
		} else {
			expression = parse_expression();
			type       = expression->base.type;
		}
		break;

	TYPENAME_START
		type = parse_typename();
		break;

	default:
		expression = parse_expression();
		type       = expression->base.type;
		break;
	}

	expect(')');

	type_t *typeof_type              = allocate_type_zero(TYPE_TYPEOF);
	typeof_type->typeoft.expression  = expression;
	typeof_type->typeoft.typeof_type = type;

	return typeof_type;
}

typedef enum {
	SPECIFIER_SIGNED    = 1 << 0,
	SPECIFIER_UNSIGNED  = 1 << 1,
	SPECIFIER_LONG      = 1 << 2,
	SPECIFIER_INT       = 1 << 3,
	SPECIFIER_DOUBLE    = 1 << 4,
	SPECIFIER_CHAR      = 1 << 5,
	SPECIFIER_SHORT     = 1 << 6,
	SPECIFIER_LONG_LONG = 1 << 7,
	SPECIFIER_FLOAT     = 1 << 8,
	SPECIFIER_BOOL      = 1 << 9,
	SPECIFIER_VOID      = 1 << 10,
#ifdef PROVIDE_COMPLEX
	SPECIFIER_COMPLEX   = 1 << 11,
	SPECIFIER_IMAGINARY = 1 << 12,
#endif
} specifiers_t;

static type_t *create_builtin_type(symbol_t *const symbol,
                                   type_t *const real_type)
{
	type_t *type            = allocate_type_zero(TYPE_BUILTIN);
	type->builtin.symbol    = symbol;
	type->builtin.real_type = real_type;

	type_t *result = typehash_insert(type);
	if (type != result) {
		free_type(type);
	}

	return result;
}

static type_t *get_typedef_type(symbol_t *symbol)
{
	declaration_t *declaration = get_declaration(symbol, NAMESPACE_NORMAL);
	if(declaration == NULL
			|| declaration->storage_class != STORAGE_CLASS_TYPEDEF)
		return NULL;

	type_t *type               = allocate_type_zero(TYPE_TYPEDEF);
	type->typedeft.declaration = declaration;

	return type;
}

static void parse_declaration_specifiers(declaration_specifiers_t *specifiers)
{
	type_t   *type            = NULL;
	unsigned  type_qualifiers = 0;
	unsigned  type_specifiers = 0;
	int       newtype         = 0;

	specifiers->source_position = token.source_position;

	while(true) {
		switch(token.type) {

		/* storage class */
#define MATCH_STORAGE_CLASS(token, class)                                \
		case token:                                                      \
			if(specifiers->storage_class != STORAGE_CLASS_NONE) {        \
				errorf(HERE, "multiple storage classes in declaration specifiers"); \
			}                                                            \
			specifiers->storage_class = class;                           \
			next_token();                                                \
			break;

		MATCH_STORAGE_CLASS(T_typedef,  STORAGE_CLASS_TYPEDEF)
		MATCH_STORAGE_CLASS(T_extern,   STORAGE_CLASS_EXTERN)
		MATCH_STORAGE_CLASS(T_static,   STORAGE_CLASS_STATIC)
		MATCH_STORAGE_CLASS(T_auto,     STORAGE_CLASS_AUTO)
		MATCH_STORAGE_CLASS(T_register, STORAGE_CLASS_REGISTER)

		case T___thread:
			switch (specifiers->storage_class) {
				case STORAGE_CLASS_NONE:
					specifiers->storage_class = STORAGE_CLASS_THREAD;
					break;

				case STORAGE_CLASS_EXTERN:
					specifiers->storage_class = STORAGE_CLASS_THREAD_EXTERN;
					break;

				case STORAGE_CLASS_STATIC:
					specifiers->storage_class = STORAGE_CLASS_THREAD_STATIC;
					break;

				default:
					errorf(HERE, "multiple storage classes in declaration specifiers");
					break;
			}
			next_token();
			break;

		/* type qualifiers */
#define MATCH_TYPE_QUALIFIER(token, qualifier)                          \
		case token:                                                     \
			type_qualifiers |= qualifier;                               \
			next_token();                                               \
			break;

		MATCH_TYPE_QUALIFIER(T_const,    TYPE_QUALIFIER_CONST);
		MATCH_TYPE_QUALIFIER(T_restrict, TYPE_QUALIFIER_RESTRICT);
		MATCH_TYPE_QUALIFIER(T_volatile, TYPE_QUALIFIER_VOLATILE);

		case T___extension__:
			/* TODO */
			next_token();
			break;

		/* type specifiers */
#define MATCH_SPECIFIER(token, specifier, name)                         \
		case token:                                                     \
			next_token();                                               \
			if(type_specifiers & specifier) {                           \
				errorf(HERE, "multiple " name " type specifiers given"); \
			} else {                                                    \
				type_specifiers |= specifier;                           \
			}                                                           \
			break;

		MATCH_SPECIFIER(T_void,       SPECIFIER_VOID,      "void")
		MATCH_SPECIFIER(T_char,       SPECIFIER_CHAR,      "char")
		MATCH_SPECIFIER(T_short,      SPECIFIER_SHORT,     "short")
		MATCH_SPECIFIER(T_int,        SPECIFIER_INT,       "int")
		MATCH_SPECIFIER(T_float,      SPECIFIER_FLOAT,     "float")
		MATCH_SPECIFIER(T_double,     SPECIFIER_DOUBLE,    "double")
		MATCH_SPECIFIER(T_signed,     SPECIFIER_SIGNED,    "signed")
		MATCH_SPECIFIER(T_unsigned,   SPECIFIER_UNSIGNED,  "unsigned")
		MATCH_SPECIFIER(T__Bool,      SPECIFIER_BOOL,      "_Bool")
#ifdef PROVIDE_COMPLEX
		MATCH_SPECIFIER(T__Complex,   SPECIFIER_COMPLEX,   "_Complex")
		MATCH_SPECIFIER(T__Imaginary, SPECIFIER_IMAGINARY, "_Imaginary")
#endif
		case T_forceinline:
			/* only in microsoft mode */
			specifiers->decl_modifiers |= DM_FORCEINLINE;

		case T_inline:
			next_token();
			specifiers->is_inline = true;
			break;

		case T_long:
			next_token();
			if(type_specifiers & SPECIFIER_LONG_LONG) {
				errorf(HERE, "multiple type specifiers given");
			} else if(type_specifiers & SPECIFIER_LONG) {
				type_specifiers |= SPECIFIER_LONG_LONG;
			} else {
				type_specifiers |= SPECIFIER_LONG;
			}
			break;

		/* TODO: if is_type_valid(type) for the following rules should issue
		 * an error */
		case T_struct: {
			type = allocate_type_zero(TYPE_COMPOUND_STRUCT);

			type->compound.declaration = parse_compound_type_specifier(true);
			break;
		}
		case T_union: {
			type = allocate_type_zero(TYPE_COMPOUND_STRUCT);

			type->compound.declaration = parse_compound_type_specifier(false);
			break;
		}
		case T_enum:
			type = parse_enum_specifier();
			break;
		case T___typeof__:
			type = parse_typeof();
			break;
		case T___builtin_va_list:
			type = duplicate_type(type_valist);
			next_token();
			break;

		case T___attribute__:
			/* TODO */
			parse_attributes();
			break;

		case T_IDENTIFIER: {
			type_t *typedef_type = get_typedef_type(token.v.symbol);

			if(typedef_type == NULL)
				goto finish_specifiers;

			next_token();
			type = typedef_type;
			break;
		}

		/* function specifier */
		default:
			goto finish_specifiers;
		}
	}

finish_specifiers:

	if(type == NULL) {
		atomic_type_kind_t atomic_type;

		/* match valid basic types */
		switch(type_specifiers) {
		case SPECIFIER_VOID:
			atomic_type = ATOMIC_TYPE_VOID;
			break;
		case SPECIFIER_CHAR:
			atomic_type = ATOMIC_TYPE_CHAR;
			break;
		case SPECIFIER_SIGNED | SPECIFIER_CHAR:
			atomic_type = ATOMIC_TYPE_SCHAR;
			break;
		case SPECIFIER_UNSIGNED | SPECIFIER_CHAR:
			atomic_type = ATOMIC_TYPE_UCHAR;
			break;
		case SPECIFIER_SHORT:
		case SPECIFIER_SIGNED | SPECIFIER_SHORT:
		case SPECIFIER_SHORT | SPECIFIER_INT:
		case SPECIFIER_SIGNED | SPECIFIER_SHORT | SPECIFIER_INT:
			atomic_type = ATOMIC_TYPE_SHORT;
			break;
		case SPECIFIER_UNSIGNED | SPECIFIER_SHORT:
		case SPECIFIER_UNSIGNED | SPECIFIER_SHORT | SPECIFIER_INT:
			atomic_type = ATOMIC_TYPE_USHORT;
			break;
		case SPECIFIER_INT:
		case SPECIFIER_SIGNED:
		case SPECIFIER_SIGNED | SPECIFIER_INT:
			atomic_type = ATOMIC_TYPE_INT;
			break;
		case SPECIFIER_UNSIGNED:
		case SPECIFIER_UNSIGNED | SPECIFIER_INT:
			atomic_type = ATOMIC_TYPE_UINT;
			break;
		case SPECIFIER_LONG:
		case SPECIFIER_SIGNED | SPECIFIER_LONG:
		case SPECIFIER_LONG | SPECIFIER_INT:
		case SPECIFIER_SIGNED | SPECIFIER_LONG | SPECIFIER_INT:
			atomic_type = ATOMIC_TYPE_LONG;
			break;
		case SPECIFIER_UNSIGNED | SPECIFIER_LONG:
		case SPECIFIER_UNSIGNED | SPECIFIER_LONG | SPECIFIER_INT:
			atomic_type = ATOMIC_TYPE_ULONG;
			break;
		case SPECIFIER_LONG | SPECIFIER_LONG_LONG:
		case SPECIFIER_SIGNED | SPECIFIER_LONG | SPECIFIER_LONG_LONG:
		case SPECIFIER_LONG | SPECIFIER_LONG_LONG | SPECIFIER_INT:
		case SPECIFIER_SIGNED | SPECIFIER_LONG | SPECIFIER_LONG_LONG
			| SPECIFIER_INT:
			atomic_type = ATOMIC_TYPE_LONGLONG;
			break;
		case SPECIFIER_UNSIGNED | SPECIFIER_LONG | SPECIFIER_LONG_LONG:
		case SPECIFIER_UNSIGNED | SPECIFIER_LONG | SPECIFIER_LONG_LONG
			| SPECIFIER_INT:
			atomic_type = ATOMIC_TYPE_ULONGLONG;
			break;
		case SPECIFIER_FLOAT:
			atomic_type = ATOMIC_TYPE_FLOAT;
			break;
		case SPECIFIER_DOUBLE:
			atomic_type = ATOMIC_TYPE_DOUBLE;
			break;
		case SPECIFIER_LONG | SPECIFIER_DOUBLE:
			atomic_type = ATOMIC_TYPE_LONG_DOUBLE;
			break;
		case SPECIFIER_BOOL:
			atomic_type = ATOMIC_TYPE_BOOL;
			break;
#ifdef PROVIDE_COMPLEX
		case SPECIFIER_FLOAT | SPECIFIER_COMPLEX:
			atomic_type = ATOMIC_TYPE_FLOAT_COMPLEX;
			break;
		case SPECIFIER_DOUBLE | SPECIFIER_COMPLEX:
			atomic_type = ATOMIC_TYPE_DOUBLE_COMPLEX;
			break;
		case SPECIFIER_LONG | SPECIFIER_DOUBLE | SPECIFIER_COMPLEX:
			atomic_type = ATOMIC_TYPE_LONG_DOUBLE_COMPLEX;
			break;
		case SPECIFIER_FLOAT | SPECIFIER_IMAGINARY:
			atomic_type = ATOMIC_TYPE_FLOAT_IMAGINARY;
			break;
		case SPECIFIER_DOUBLE | SPECIFIER_IMAGINARY:
			atomic_type = ATOMIC_TYPE_DOUBLE_IMAGINARY;
			break;
		case SPECIFIER_LONG | SPECIFIER_DOUBLE | SPECIFIER_IMAGINARY:
			atomic_type = ATOMIC_TYPE_LONG_DOUBLE_IMAGINARY;
			break;
#endif
		default:
			/* invalid specifier combination, give an error message */
			if(type_specifiers == 0) {
				if (! strict_mode) {
					if (warning.implicit_int) {
						warningf(HERE, "no type specifiers in declaration, using 'int'");
					}
					atomic_type = ATOMIC_TYPE_INT;
					break;
				} else {
					errorf(HERE, "no type specifiers given in declaration");
				}
			} else if((type_specifiers & SPECIFIER_SIGNED) &&
			          (type_specifiers & SPECIFIER_UNSIGNED)) {
				errorf(HERE, "signed and unsigned specifiers gives");
			} else if(type_specifiers & (SPECIFIER_SIGNED | SPECIFIER_UNSIGNED)) {
				errorf(HERE, "only integer types can be signed or unsigned");
			} else {
				errorf(HERE, "multiple datatypes in declaration");
			}
			atomic_type = ATOMIC_TYPE_INVALID;
		}

		type               = allocate_type_zero(TYPE_ATOMIC);
		type->atomic.akind = atomic_type;
		newtype            = 1;
	} else {
		if(type_specifiers != 0) {
			errorf(HERE, "multiple datatypes in declaration");
		}
	}

	type->base.qualifiers = type_qualifiers;

	type_t *result = typehash_insert(type);
	if(newtype && result != type) {
		free_type(type);
	}

	specifiers->type = result;
}

static type_qualifiers_t parse_type_qualifiers(void)
{
	type_qualifiers_t type_qualifiers = TYPE_QUALIFIER_NONE;

	while(true) {
		switch(token.type) {
		/* type qualifiers */
		MATCH_TYPE_QUALIFIER(T_const,    TYPE_QUALIFIER_CONST);
		MATCH_TYPE_QUALIFIER(T_restrict, TYPE_QUALIFIER_RESTRICT);
		MATCH_TYPE_QUALIFIER(T_volatile, TYPE_QUALIFIER_VOLATILE);

		default:
			return type_qualifiers;
		}
	}
}

static declaration_t *parse_identifier_list(void)
{
	declaration_t *declarations     = NULL;
	declaration_t *last_declaration = NULL;
	do {
		declaration_t *const declaration = allocate_declaration_zero();
		declaration->type            = NULL; /* a K&R parameter list has no types, yet */
		declaration->source_position = token.source_position;
		declaration->symbol          = token.v.symbol;
		next_token();

		if(last_declaration != NULL) {
			last_declaration->next = declaration;
		} else {
			declarations = declaration;
		}
		last_declaration = declaration;

		if(token.type != ',')
			break;
		next_token();
	} while(token.type == T_IDENTIFIER);

	return declarations;
}

static void semantic_parameter(declaration_t *declaration)
{
	/* TODO: improve error messages */

	if(declaration->storage_class == STORAGE_CLASS_TYPEDEF) {
		errorf(HERE, "typedef not allowed in parameter list");
	} else if(declaration->storage_class != STORAGE_CLASS_NONE
			&& declaration->storage_class != STORAGE_CLASS_REGISTER) {
		errorf(HERE, "parameter may only have none or register storage class");
	}

	type_t *const orig_type = declaration->type;
	type_t *      type      = skip_typeref(orig_type);

	/* Array as last part of a parameter type is just syntactic sugar.  Turn it
	 * into a pointer. § 6.7.5.3 (7) */
	if (is_type_array(type)) {
		type_t *const element_type = type->array.element_type;

		type = make_pointer_type(element_type, type->base.qualifiers);

		declaration->type = type;
	}

	if(is_type_incomplete(type)) {
		errorf(HERE, "incomplete type ('%T') not allowed for parameter '%Y'",
		       orig_type, declaration->symbol);
	}
}

static declaration_t *parse_parameter(void)
{
	declaration_specifiers_t specifiers;
	memset(&specifiers, 0, sizeof(specifiers));

	parse_declaration_specifiers(&specifiers);

	declaration_t *declaration = parse_declarator(&specifiers, /*may_be_abstract=*/true);

	semantic_parameter(declaration);

	return declaration;
}

static declaration_t *parse_parameters(function_type_t *type)
{
	if(token.type == T_IDENTIFIER) {
		symbol_t *symbol = token.v.symbol;
		if(!is_typedef_symbol(symbol)) {
			type->kr_style_parameters = true;
			return parse_identifier_list();
		}
	}

	if(token.type == ')') {
		type->unspecified_parameters = 1;
		return NULL;
	}
	if(token.type == T_void && look_ahead(1)->type == ')') {
		next_token();
		return NULL;
	}

	declaration_t        *declarations = NULL;
	declaration_t        *declaration;
	declaration_t        *last_declaration = NULL;
	function_parameter_t *parameter;
	function_parameter_t *last_parameter = NULL;

	while(true) {
		switch(token.type) {
		case T_DOTDOTDOT:
			next_token();
			type->variadic = 1;
			return declarations;

		case T_IDENTIFIER:
		case T___extension__:
		DECLARATION_START
			declaration = parse_parameter();

			parameter       = obstack_alloc(type_obst, sizeof(parameter[0]));
			memset(parameter, 0, sizeof(parameter[0]));
			parameter->type = declaration->type;

			if(last_parameter != NULL) {
				last_declaration->next = declaration;
				last_parameter->next   = parameter;
			} else {
				type->parameters = parameter;
				declarations     = declaration;
			}
			last_parameter   = parameter;
			last_declaration = declaration;
			break;

		default:
			return declarations;
		}
		if(token.type != ',')
			return declarations;
		next_token();
	}
}

typedef enum {
	CONSTRUCT_INVALID,
	CONSTRUCT_POINTER,
	CONSTRUCT_FUNCTION,
	CONSTRUCT_ARRAY
} construct_type_type_t;

typedef struct construct_type_t construct_type_t;
struct construct_type_t {
	construct_type_type_t  type;
	construct_type_t      *next;
};

typedef struct parsed_pointer_t parsed_pointer_t;
struct parsed_pointer_t {
	construct_type_t  construct_type;
	type_qualifiers_t type_qualifiers;
};

typedef struct construct_function_type_t construct_function_type_t;
struct construct_function_type_t {
	construct_type_t  construct_type;
	type_t           *function_type;
};

typedef struct parsed_array_t parsed_array_t;
struct parsed_array_t {
	construct_type_t  construct_type;
	type_qualifiers_t type_qualifiers;
	bool              is_static;
	bool              is_variable;
	expression_t     *size;
};

typedef struct construct_base_type_t construct_base_type_t;
struct construct_base_type_t {
	construct_type_t  construct_type;
	type_t           *type;
};

static construct_type_t *parse_pointer_declarator(void)
{
	eat('*');

	parsed_pointer_t *pointer = obstack_alloc(&temp_obst, sizeof(pointer[0]));
	memset(pointer, 0, sizeof(pointer[0]));
	pointer->construct_type.type = CONSTRUCT_POINTER;
	pointer->type_qualifiers     = parse_type_qualifiers();

	return (construct_type_t*) pointer;
}

static construct_type_t *parse_array_declarator(void)
{
	eat('[');

	parsed_array_t *array = obstack_alloc(&temp_obst, sizeof(array[0]));
	memset(array, 0, sizeof(array[0]));
	array->construct_type.type = CONSTRUCT_ARRAY;

	if(token.type == T_static) {
		array->is_static = true;
		next_token();
	}

	type_qualifiers_t type_qualifiers = parse_type_qualifiers();
	if(type_qualifiers != 0) {
		if(token.type == T_static) {
			array->is_static = true;
			next_token();
		}
	}
	array->type_qualifiers = type_qualifiers;

	if(token.type == '*' && look_ahead(1)->type == ']') {
		array->is_variable = true;
		next_token();
	} else if(token.type != ']') {
		array->size = parse_assignment_expression();
	}

	expect(']');

	return (construct_type_t*) array;
}

static construct_type_t *parse_function_declarator(declaration_t *declaration)
{
	eat('(');

	type_t *type = allocate_type_zero(TYPE_FUNCTION);

	declaration_t *parameters = parse_parameters(&type->function);
	if(declaration != NULL) {
		declaration->scope.declarations = parameters;
	}

	construct_function_type_t *construct_function_type =
		obstack_alloc(&temp_obst, sizeof(construct_function_type[0]));
	memset(construct_function_type, 0, sizeof(construct_function_type[0]));
	construct_function_type->construct_type.type = CONSTRUCT_FUNCTION;
	construct_function_type->function_type       = type;

	expect(')');

	return (construct_type_t*) construct_function_type;
}

static construct_type_t *parse_inner_declarator(declaration_t *declaration,
		bool may_be_abstract)
{
	/* construct a single linked list of construct_type_t's which describe
	 * how to construct the final declarator type */
	construct_type_t *first = NULL;
	construct_type_t *last  = NULL;

	/* pointers */
	while(token.type == '*') {
		construct_type_t *type = parse_pointer_declarator();

		if(last == NULL) {
			first = type;
			last  = type;
		} else {
			last->next = type;
			last       = type;
		}
	}

	/* TODO: find out if this is correct */
	parse_attributes();

	construct_type_t *inner_types = NULL;

	switch(token.type) {
	case T_IDENTIFIER:
		if(declaration == NULL) {
			errorf(HERE, "no identifier expected in typename");
		} else {
			declaration->symbol          = token.v.symbol;
			declaration->source_position = token.source_position;
		}
		next_token();
		break;
	case '(':
		next_token();
		inner_types = parse_inner_declarator(declaration, may_be_abstract);
		expect(')');
		break;
	default:
		if(may_be_abstract)
			break;
		parse_error_expected("while parsing declarator", T_IDENTIFIER, '(', 0);
		/* avoid a loop in the outermost scope, because eat_statement doesn't
		 * eat '}' */
		if(token.type == '}' && current_function == NULL) {
			next_token();
		} else {
			eat_statement();
		}
		return NULL;
	}

	construct_type_t *p = last;

	while(true) {
		construct_type_t *type;
		switch(token.type) {
		case '(':
			type = parse_function_declarator(declaration);
			break;
		case '[':
			type = parse_array_declarator();
			break;
		default:
			goto declarator_finished;
		}

		/* insert in the middle of the list (behind p) */
		if(p != NULL) {
			type->next = p->next;
			p->next    = type;
		} else {
			type->next = first;
			first      = type;
		}
		if(last == p) {
			last = type;
		}
	}

declarator_finished:
	parse_attributes();

	/* append inner_types at the end of the list, we don't to set last anymore
	 * as it's not needed anymore */
	if(last == NULL) {
		assert(first == NULL);
		first = inner_types;
	} else {
		last->next = inner_types;
	}

	return first;
}

static type_t *construct_declarator_type(construct_type_t *construct_list,
                                         type_t *type)
{
	construct_type_t *iter = construct_list;
	for( ; iter != NULL; iter = iter->next) {
		switch(iter->type) {
		case CONSTRUCT_INVALID:
			panic("invalid type construction found");
		case CONSTRUCT_FUNCTION: {
			construct_function_type_t *construct_function_type
				= (construct_function_type_t*) iter;

			type_t *function_type = construct_function_type->function_type;

			function_type->function.return_type = type;

			type_t *skipped_return_type = skip_typeref(type);
			if (is_type_function(skipped_return_type)) {
				errorf(HERE, "function returning function is not allowed");
				type = type_error_type;
			} else if (is_type_array(skipped_return_type)) {
				errorf(HERE, "function returning array is not allowed");
				type = type_error_type;
			} else {
				type = function_type;
			}
			break;
		}

		case CONSTRUCT_POINTER: {
			parsed_pointer_t *parsed_pointer = (parsed_pointer_t*) iter;
			type_t           *pointer_type   = allocate_type_zero(TYPE_POINTER);
			pointer_type->pointer.points_to  = type;
			pointer_type->base.qualifiers    = parsed_pointer->type_qualifiers;

			type = pointer_type;
			break;
		}

		case CONSTRUCT_ARRAY: {
			parsed_array_t *parsed_array  = (parsed_array_t*) iter;
			type_t         *array_type    = allocate_type_zero(TYPE_ARRAY);

			array_type->base.qualifiers    = parsed_array->type_qualifiers;
			array_type->array.element_type = type;
			array_type->array.is_static    = parsed_array->is_static;
			array_type->array.is_variable  = parsed_array->is_variable;
			array_type->array.size         = parsed_array->size;

			type_t *skipped_type = skip_typeref(type);
			if (is_type_atomic(skipped_type, ATOMIC_TYPE_VOID)) {
				errorf(HERE, "array of void is not allowed");
				type = type_error_type;
			} else {
				type = array_type;
			}
			break;
		}
		}

		type_t *hashed_type = typehash_insert(type);
		if(hashed_type != type) {
			/* the function type was constructed earlier freeing it here will
			 * destroy other types... */
			if(iter->type != CONSTRUCT_FUNCTION) {
				free_type(type);
			}
			type = hashed_type;
		}
	}

	return type;
}

static declaration_t *parse_declarator(
		const declaration_specifiers_t *specifiers, bool may_be_abstract)
{
	declaration_t *const declaration = allocate_declaration_zero();
	declaration->storage_class  = specifiers->storage_class;
	declaration->modifiers      = specifiers->decl_modifiers;
	declaration->is_inline      = specifiers->is_inline;

	construct_type_t *construct_type
		= parse_inner_declarator(declaration, may_be_abstract);
	type_t *const type = specifiers->type;
	declaration->type = construct_declarator_type(construct_type, type);

	if(construct_type != NULL) {
		obstack_free(&temp_obst, construct_type);
	}

	return declaration;
}

static type_t *parse_abstract_declarator(type_t *base_type)
{
	construct_type_t *construct_type = parse_inner_declarator(NULL, 1);

	type_t *result = construct_declarator_type(construct_type, base_type);
	if(construct_type != NULL) {
		obstack_free(&temp_obst, construct_type);
	}

	return result;
}

static declaration_t *append_declaration(declaration_t* const declaration)
{
	if (last_declaration != NULL) {
		last_declaration->next = declaration;
	} else {
		scope->declarations = declaration;
	}
	last_declaration = declaration;
	return declaration;
}

/**
 * Check if the declaration of main is suspicious.  main should be a
 * function with external linkage, returning int, taking either zero
 * arguments, two, or three arguments of appropriate types, ie.
 *
 * int main([ int argc, char **argv [, char **env ] ]).
 *
 * @param decl    the declaration to check
 * @param type    the function type of the declaration
 */
static void check_type_of_main(const declaration_t *const decl, const function_type_t *const func_type)
{
	if (decl->storage_class == STORAGE_CLASS_STATIC) {
		warningf(decl->source_position, "'main' is normally a non-static function");
	}
	if (skip_typeref(func_type->return_type) != type_int) {
		warningf(decl->source_position, "return type of 'main' should be 'int', but is '%T'", func_type->return_type);
	}
	const function_parameter_t *parm = func_type->parameters;
	if (parm != NULL) {
		type_t *const first_type = parm->type;
		if (!types_compatible(skip_typeref(first_type), type_int)) {
			warningf(decl->source_position, "first argument of 'main' should be 'int', but is '%T'", first_type);
		}
		parm = parm->next;
		if (parm != NULL) {
			type_t *const second_type = parm->type;
			if (!types_compatible(skip_typeref(second_type), type_char_ptr_ptr)) {
				warningf(decl->source_position, "second argument of 'main' should be 'char**', but is '%T'", second_type);
			}
			parm = parm->next;
			if (parm != NULL) {
				type_t *const third_type = parm->type;
				if (!types_compatible(skip_typeref(third_type), type_char_ptr_ptr)) {
					warningf(decl->source_position, "third argument of 'main' should be 'char**', but is '%T'", third_type);
				}
				parm = parm->next;
				if (parm != NULL) {
					warningf(decl->source_position, "'main' takes only zero, two or three arguments");
				}
			}
		} else {
			warningf(decl->source_position, "'main' takes only zero, two or three arguments");
		}
	}
}

/**
 * Check if a symbol is the equal to "main".
 */
static bool is_sym_main(const symbol_t *const sym)
{
	return strcmp(sym->string, "main") == 0;
}

static declaration_t *internal_record_declaration(
	declaration_t *const declaration,
	const bool is_function_definition)
{
	const symbol_t *const symbol  = declaration->symbol;
	const namespace_t     namespc = (namespace_t)declaration->namespc;

	type_t       *const orig_type = declaration->type;
	const type_t *const type      = skip_typeref(orig_type);
	if (is_type_function(type) &&
			type->function.unspecified_parameters &&
			warning.strict_prototypes) {
		warningf(declaration->source_position,
		         "function declaration '%#T' is not a prototype",
		         orig_type, declaration->symbol);
	}

	if (is_function_definition && warning.main && is_sym_main(symbol)) {
		check_type_of_main(declaration, &type->function);
	}

	declaration_t *const previous_declaration = get_declaration(symbol, namespc);
	assert(declaration != previous_declaration);
	if (previous_declaration != NULL) {
		if (previous_declaration->parent_scope == scope) {
			/* can happen for K&R style declarations */
			if(previous_declaration->type == NULL) {
				previous_declaration->type = declaration->type;
			}

			const type_t *const prev_type = skip_typeref(previous_declaration->type);
			if (!types_compatible(type, prev_type)) {
				errorf(declaration->source_position,
					"declaration '%#T' is incompatible with previous declaration '%#T'",
					orig_type, symbol, previous_declaration->type, symbol);
				errorf(previous_declaration->source_position, "previous declaration of '%Y' was here", symbol);
			} else {
				unsigned old_storage_class = previous_declaration->storage_class;
				unsigned new_storage_class = declaration->storage_class;

				/* pretend no storage class means extern for function declarations
				 * (except if the previous declaration is neither none nor extern) */
				if (is_type_function(type)) {
					switch (old_storage_class) {
						case STORAGE_CLASS_NONE:
							old_storage_class = STORAGE_CLASS_EXTERN;

						case STORAGE_CLASS_EXTERN:
							if (is_function_definition) {
								if (warning.missing_prototypes &&
								    prev_type->function.unspecified_parameters &&
								    !is_sym_main(symbol)) {
									warningf(declaration->source_position, "no previous prototype for '%#T'", orig_type, symbol);
								}
							} else if (new_storage_class == STORAGE_CLASS_NONE) {
								new_storage_class = STORAGE_CLASS_EXTERN;
							}
							break;

						default: break;
					}
				}

				if (old_storage_class == STORAGE_CLASS_EXTERN &&
						new_storage_class == STORAGE_CLASS_EXTERN) {
warn_redundant_declaration:
					if (warning.redundant_decls) {
						warningf(declaration->source_position, "redundant declaration for '%Y'", symbol);
						warningf(previous_declaration->source_position, "previous declaration of '%Y' was here", symbol);
					}
				} else if (current_function == NULL) {
					if (old_storage_class != STORAGE_CLASS_STATIC &&
							new_storage_class == STORAGE_CLASS_STATIC) {
						errorf(declaration->source_position, "static declaration of '%Y' follows non-static declaration", symbol);
						errorf(previous_declaration->source_position, "previous declaration of '%Y' was here", symbol);
					} else {
						if (old_storage_class != STORAGE_CLASS_EXTERN && !is_function_definition) {
							goto warn_redundant_declaration;
						}
						if (new_storage_class == STORAGE_CLASS_NONE) {
							previous_declaration->storage_class = STORAGE_CLASS_NONE;
						}
					}
				} else {
					if (old_storage_class == new_storage_class) {
						errorf(declaration->source_position, "redeclaration of '%Y'", symbol);
					} else {
						errorf(declaration->source_position, "redeclaration of '%Y' with different linkage", symbol);
					}
					errorf(previous_declaration->source_position, "previous declaration of '%Y' was here", symbol);
				}
			}
			return previous_declaration;
		}
	} else if (is_function_definition) {
		if (declaration->storage_class != STORAGE_CLASS_STATIC) {
			if (warning.missing_prototypes && !is_sym_main(symbol)) {
				warningf(declaration->source_position, "no previous prototype for '%#T'", orig_type, symbol);
			} else if (warning.missing_declarations && !is_sym_main(symbol)) {
				warningf(declaration->source_position, "no previous declaration for '%#T'", orig_type, symbol);
			}
		}
	} else if (warning.missing_declarations &&
	    scope == global_scope &&
	    !is_type_function(type) && (
	      declaration->storage_class == STORAGE_CLASS_NONE ||
	      declaration->storage_class == STORAGE_CLASS_THREAD
	    )) {
		warningf(declaration->source_position, "no previous declaration for '%#T'", orig_type, symbol);
	}

	assert(declaration->parent_scope == NULL);
	assert(declaration->symbol != NULL);
	assert(scope != NULL);

	declaration->parent_scope = scope;

	environment_push(declaration);
	return append_declaration(declaration);
}

static declaration_t *record_declaration(declaration_t *declaration)
{
	return internal_record_declaration(declaration, false);
}

static declaration_t *record_function_definition(declaration_t *declaration)
{
	return internal_record_declaration(declaration, true);
}

static void parser_error_multiple_definition(declaration_t *declaration,
		const source_position_t source_position)
{
	errorf(source_position, "multiple definition of symbol '%Y'",
	       declaration->symbol);
	errorf(declaration->source_position,
	       "this is the location of the previous definition.");
}

static bool is_declaration_specifier(const token_t *token,
                                     bool only_type_specifiers)
{
	switch(token->type) {
		TYPE_SPECIFIERS
			return true;
		case T_IDENTIFIER:
			return is_typedef_symbol(token->v.symbol);

		case T___extension__:
		STORAGE_CLASSES
		TYPE_QUALIFIERS
			return !only_type_specifiers;

		default:
			return false;
	}
}

static void parse_init_declarator_rest(declaration_t *declaration)
{
	eat('=');

	type_t *orig_type = declaration->type;
	type_t *type      = type = skip_typeref(orig_type);

	if(declaration->init.initializer != NULL) {
		parser_error_multiple_definition(declaration, token.source_position);
	}

	initializer_t *initializer = parse_initializer(type);

	/* § 6.7.5 (22)  array initializers for arrays with unknown size determine
	 * the array type size */
	if(is_type_array(type) && initializer != NULL) {
		array_type_t *array_type = &type->array;

		if(array_type->size == NULL) {
			expression_t *cnst = allocate_expression_zero(EXPR_CONST);

			cnst->base.type = type_size_t;

			switch (initializer->kind) {
				case INITIALIZER_LIST: {
					cnst->conste.v.int_value = initializer->list.len;
					break;
				}

				case INITIALIZER_STRING: {
					cnst->conste.v.int_value = initializer->string.string.size;
					break;
				}

				case INITIALIZER_WIDE_STRING: {
					cnst->conste.v.int_value = initializer->wide_string.string.size;
					break;
				}

				default:
					panic("invalid initializer type");
			}

			array_type->size = cnst;
		}
	}

	if(is_type_function(type)) {
		errorf(declaration->source_position,
		       "initializers not allowed for function types at declator '%Y' (type '%T')",
		       declaration->symbol, orig_type);
	} else {
		declaration->init.initializer = initializer;
	}
}

/* parse rest of a declaration without any declarator */
static void parse_anonymous_declaration_rest(
		const declaration_specifiers_t *specifiers,
		parsed_declaration_func finished_declaration)
{
	eat(';');

	declaration_t *const declaration = allocate_declaration_zero();
	declaration->type            = specifiers->type;
	declaration->storage_class   = specifiers->storage_class;
	declaration->source_position = specifiers->source_position;

	if (declaration->storage_class != STORAGE_CLASS_NONE) {
		warningf(declaration->source_position, "useless storage class in empty declaration");
	}

	type_t *type = declaration->type;
	switch (type->kind) {
		case TYPE_COMPOUND_STRUCT:
		case TYPE_COMPOUND_UNION: {
			if (type->compound.declaration->symbol == NULL) {
				warningf(declaration->source_position, "unnamed struct/union that defines no instances");
			}
			break;
		}

		case TYPE_ENUM:
			break;

		default:
			warningf(declaration->source_position, "empty declaration");
			break;
	}

	finished_declaration(declaration);
}

static void parse_declaration_rest(declaration_t *ndeclaration,
		const declaration_specifiers_t *specifiers,
		parsed_declaration_func finished_declaration)
{
	while(true) {
		declaration_t *declaration = finished_declaration(ndeclaration);

		type_t *orig_type = declaration->type;
		type_t *type      = skip_typeref(orig_type);

		if (type->kind != TYPE_FUNCTION &&
		    declaration->is_inline &&
		    is_type_valid(type)) {
			warningf(declaration->source_position,
			         "variable '%Y' declared 'inline'\n", declaration->symbol);
		}

		if(token.type == '=') {
			parse_init_declarator_rest(declaration);
		}

		if(token.type != ',')
			break;
		eat(',');

		ndeclaration = parse_declarator(specifiers, /*may_be_abstract=*/false);
	}
	expect_void(';');
}

static declaration_t *finished_kr_declaration(declaration_t *declaration)
{
	symbol_t *symbol  = declaration->symbol;
	if(symbol == NULL) {
		errorf(HERE, "anonymous declaration not valid as function parameter");
		return declaration;
	}
	namespace_t namespc = (namespace_t) declaration->namespc;
	if(namespc != NAMESPACE_NORMAL) {
		return record_declaration(declaration);
	}

	declaration_t *previous_declaration = get_declaration(symbol, namespc);
	if(previous_declaration == NULL ||
			previous_declaration->parent_scope != scope) {
		errorf(HERE, "expected declaration of a function parameter, found '%Y'",
		       symbol);
		return declaration;
	}

	if(previous_declaration->type == NULL) {
		previous_declaration->type           = declaration->type;
		previous_declaration->storage_class  = declaration->storage_class;
		previous_declaration->parent_scope = scope;
		return previous_declaration;
	} else {
		return record_declaration(declaration);
	}
}

static void parse_declaration(parsed_declaration_func finished_declaration)
{
	declaration_specifiers_t specifiers;
	memset(&specifiers, 0, sizeof(specifiers));
	parse_declaration_specifiers(&specifiers);

	if(token.type == ';') {
		parse_anonymous_declaration_rest(&specifiers, finished_declaration);
	} else {
		declaration_t *declaration = parse_declarator(&specifiers, /*may_be_abstract=*/false);
		parse_declaration_rest(declaration, &specifiers, finished_declaration);
	}
}

static void parse_kr_declaration_list(declaration_t *declaration)
{
	type_t *type = skip_typeref(declaration->type);
	if(!is_type_function(type))
		return;

	if(!type->function.kr_style_parameters)
		return;

	/* push function parameters */
	int       top        = environment_top();
	scope_t  *last_scope = scope;
	set_scope(&declaration->scope);

	declaration_t *parameter = declaration->scope.declarations;
	for( ; parameter != NULL; parameter = parameter->next) {
		assert(parameter->parent_scope == NULL);
		parameter->parent_scope = scope;
		environment_push(parameter);
	}

	/* parse declaration list */
	while(is_declaration_specifier(&token, false)) {
		parse_declaration(finished_kr_declaration);
	}

	/* pop function parameters */
	assert(scope == &declaration->scope);
	set_scope(last_scope);
	environment_pop_to(top);

	/* update function type */
	type_t *new_type = duplicate_type(type);
	new_type->function.kr_style_parameters = false;

	function_parameter_t *parameters     = NULL;
	function_parameter_t *last_parameter = NULL;

	declaration_t *parameter_declaration = declaration->scope.declarations;
	for( ; parameter_declaration != NULL;
			parameter_declaration = parameter_declaration->next) {
		type_t *parameter_type = parameter_declaration->type;
		if(parameter_type == NULL) {
			if (strict_mode) {
				errorf(HERE, "no type specified for function parameter '%Y'",
				       parameter_declaration->symbol);
			} else {
				if (warning.implicit_int) {
					warningf(HERE, "no type specified for function parameter '%Y', using 'int'",
						parameter_declaration->symbol);
				}
				parameter_type              = type_int;
				parameter_declaration->type = parameter_type;
			}
		}

		semantic_parameter(parameter_declaration);
		parameter_type = parameter_declaration->type;

		function_parameter_t *function_parameter
			= obstack_alloc(type_obst, sizeof(function_parameter[0]));
		memset(function_parameter, 0, sizeof(function_parameter[0]));

		function_parameter->type = parameter_type;
		if(last_parameter != NULL) {
			last_parameter->next = function_parameter;
		} else {
			parameters = function_parameter;
		}
		last_parameter = function_parameter;
	}
	new_type->function.parameters = parameters;

	type = typehash_insert(new_type);
	if(type != new_type) {
		obstack_free(type_obst, new_type);
	}

	declaration->type = type;
}

static bool first_err = true;

/**
 * When called with first_err set, prints the name of the current function,
 * else does noting.
 */
static void print_in_function(void) {
	if (first_err) {
		first_err = false;
		diagnosticf("%s: In function '%Y':\n",
			current_function->source_position.input_name,
			current_function->symbol);
	}
}

/**
 * Check if all labels are defined in the current function.
 * Check if all labels are used in the current function.
 */
static void check_labels(void)
{
	for (const goto_statement_t *goto_statement = goto_first;
	    goto_statement != NULL;
	    goto_statement = goto_statement->next) {
		declaration_t *label = goto_statement->label;

		label->used = true;
		if (label->source_position.input_name == NULL) {
			print_in_function();
			errorf(goto_statement->base.source_position,
			       "label '%Y' used but not defined", label->symbol);
		 }
	}
	goto_first = goto_last = NULL;

	if (warning.unused_label) {
		for (const label_statement_t *label_statement = label_first;
			 label_statement != NULL;
			 label_statement = label_statement->next) {
			const declaration_t *label = label_statement->label;

			if (! label->used) {
				print_in_function();
				warningf(label_statement->base.source_position,
					"label '%Y' defined but not used", label->symbol);
			}
		}
	}
	label_first = label_last = NULL;
}

/**
 * Check declarations of current_function for unused entities.
 */
static void check_declarations(void)
{
	if (warning.unused_parameter) {
		const scope_t *scope = &current_function->scope;

		const declaration_t *parameter = scope->declarations;
		for (; parameter != NULL; parameter = parameter->next) {
			if (! parameter->used) {
				print_in_function();
				warningf(parameter->source_position,
					"unused parameter '%Y'", parameter->symbol);
			}
		}
	}
	if (warning.unused_variable) {
	}
}

static void parse_external_declaration(void)
{
	/* function-definitions and declarations both start with declaration
	 * specifiers */
	declaration_specifiers_t specifiers;
	memset(&specifiers, 0, sizeof(specifiers));
	parse_declaration_specifiers(&specifiers);

	/* must be a declaration */
	if(token.type == ';') {
		parse_anonymous_declaration_rest(&specifiers, append_declaration);
		return;
	}

	/* declarator is common to both function-definitions and declarations */
	declaration_t *ndeclaration = parse_declarator(&specifiers, /*may_be_abstract=*/false);

	/* must be a declaration */
	if(token.type == ',' || token.type == '=' || token.type == ';') {
		parse_declaration_rest(ndeclaration, &specifiers, record_declaration);
		return;
	}

	/* must be a function definition */
	parse_kr_declaration_list(ndeclaration);

	if(token.type != '{') {
		parse_error_expected("while parsing function definition", '{', 0);
		eat_statement();
		return;
	}

	type_t *type = ndeclaration->type;

	/* note that we don't skip typerefs: the standard doesn't allow them here
	 * (so we can't use is_type_function here) */
	if(type->kind != TYPE_FUNCTION) {
		if (is_type_valid(type)) {
			errorf(HERE, "declarator '%#T' has a body but is not a function type",
			       type, ndeclaration->symbol);
		}
		eat_block();
		return;
	}

	/* § 6.7.5.3 (14) a function definition with () means no
	 * parameters (and not unspecified parameters) */
	if(type->function.unspecified_parameters) {
		type_t *duplicate = duplicate_type(type);
		duplicate->function.unspecified_parameters = false;

		type = typehash_insert(duplicate);
		if(type != duplicate) {
			obstack_free(type_obst, duplicate);
		}
		ndeclaration->type = type;
	}

	declaration_t *const declaration = record_function_definition(ndeclaration);
	if(ndeclaration != declaration) {
		declaration->scope = ndeclaration->scope;
	}
	type = skip_typeref(declaration->type);

	/* push function parameters and switch scope */
	int       top        = environment_top();
	scope_t  *last_scope = scope;
	set_scope(&declaration->scope);

	declaration_t *parameter = declaration->scope.declarations;
	for( ; parameter != NULL; parameter = parameter->next) {
		if(parameter->parent_scope == &ndeclaration->scope) {
			parameter->parent_scope = scope;
		}
		assert(parameter->parent_scope == NULL
				|| parameter->parent_scope == scope);
		parameter->parent_scope = scope;
		environment_push(parameter);
	}

	if(declaration->init.statement != NULL) {
		parser_error_multiple_definition(declaration, token.source_position);
		eat_block();
		goto end_of_parse_external_declaration;
	} else {
		/* parse function body */
		int            label_stack_top      = label_top();
		declaration_t *old_current_function = current_function;
		current_function                    = declaration;

		declaration->init.statement = parse_compound_statement();
		first_err = true;
		check_labels();
		check_declarations();

		assert(current_function == declaration);
		current_function = old_current_function;
		label_pop_to(label_stack_top);
	}

end_of_parse_external_declaration:
	assert(scope == &declaration->scope);
	set_scope(last_scope);
	environment_pop_to(top);
}

static type_t *make_bitfield_type(type_t *base, expression_t *size)
{
	type_t *type        = allocate_type_zero(TYPE_BITFIELD);
	type->bitfield.base = base;
	type->bitfield.size = size;

	return type;
}

static void parse_struct_declarators(const declaration_specifiers_t *specifiers)
{
	/* TODO: check constraints for struct declarations (in specifiers) */
	while(1) {
		declaration_t *declaration;

		if(token.type == ':') {
			next_token();

			type_t *base_type = specifiers->type;
			expression_t *size = parse_constant_expression();

			type_t *type = make_bitfield_type(base_type, size);

			declaration = allocate_declaration_zero();
			declaration->namespc         = NAMESPACE_NORMAL;
			declaration->storage_class   = STORAGE_CLASS_NONE;
			declaration->source_position = token.source_position;
			declaration->modifiers       = specifiers->decl_modifiers;
			declaration->type            = type;
		} else {
			declaration = parse_declarator(specifiers,/*may_be_abstract=*/true);

			if(token.type == ':') {
				next_token();
				expression_t *size = parse_constant_expression();

				type_t *type = make_bitfield_type(declaration->type, size);
				declaration->type = type;
			}
		}
		record_declaration(declaration);

		if(token.type != ',')
			break;
		next_token();
	}
	expect_void(';');
}

static void parse_compound_type_entries(void)
{
	eat('{');

	while(token.type != '}' && token.type != T_EOF) {
		declaration_specifiers_t specifiers;
		memset(&specifiers, 0, sizeof(specifiers));
		parse_declaration_specifiers(&specifiers);

		parse_struct_declarators(&specifiers);
	}
	if(token.type == T_EOF) {
		errorf(HERE, "EOF while parsing struct");
	}
	next_token();
}

static type_t *parse_typename(void)
{
	declaration_specifiers_t specifiers;
	memset(&specifiers, 0, sizeof(specifiers));
	parse_declaration_specifiers(&specifiers);
	if(specifiers.storage_class != STORAGE_CLASS_NONE) {
		/* TODO: improve error message, user does probably not know what a
		 * storage class is...
		 */
		errorf(HERE, "typename may not have a storage class");
	}

	type_t *result = parse_abstract_declarator(specifiers.type);

	return result;
}




typedef expression_t* (*parse_expression_function) (unsigned precedence);
typedef expression_t* (*parse_expression_infix_function) (unsigned precedence,
                                                          expression_t *left);

typedef struct expression_parser_function_t expression_parser_function_t;
struct expression_parser_function_t {
	unsigned                         precedence;
	parse_expression_function        parser;
	unsigned                         infix_precedence;
	parse_expression_infix_function  infix_parser;
};

expression_parser_function_t expression_parsers[T_LAST_TOKEN];

/**
 * Creates a new invalid expression.
 */
static expression_t *create_invalid_expression(void)
{
	expression_t *expression         = allocate_expression_zero(EXPR_INVALID);
	expression->base.source_position = token.source_position;
	return expression;
}

/**
 * Prints an error message if an expression was expected but not read
 */
static expression_t *expected_expression_error(void)
{
	/* skip the error message if the error token was read */
	if (token.type != T_ERROR) {
		errorf(HERE, "expected expression, got token '%K'", &token);
	}
	next_token();

	return create_invalid_expression();
}

/**
 * Parse a string constant.
 */
static expression_t *parse_string_const(void)
{
	expression_t *cnst = allocate_expression_zero(EXPR_STRING_LITERAL);
	cnst->base.type    = type_char_ptr;
	cnst->string.value = parse_string_literals();

	return cnst;
}

/**
 * Parse a wide string constant.
 */
static expression_t *parse_wide_string_const(void)
{
	expression_t *const cnst = allocate_expression_zero(EXPR_WIDE_STRING_LITERAL);
	cnst->base.type         = type_wchar_t_ptr;
	cnst->wide_string.value = token.v.wide_string; /* TODO concatenate */
	next_token();
	return cnst;
}

/**
 * Parse an integer constant.
 */
static expression_t *parse_int_const(void)
{
	expression_t *cnst       = allocate_expression_zero(EXPR_CONST);
	cnst->base.type          = token.datatype;
	cnst->conste.v.int_value = token.v.intvalue;

	next_token();

	return cnst;
}

/**
 * Parse a float constant.
 */
static expression_t *parse_float_const(void)
{
	expression_t *cnst         = allocate_expression_zero(EXPR_CONST);
	cnst->base.type            = token.datatype;
	cnst->conste.v.float_value = token.v.floatvalue;

	next_token();

	return cnst;
}

static declaration_t *create_implicit_function(symbol_t *symbol,
		const source_position_t source_position)
{
	type_t *ntype                          = allocate_type_zero(TYPE_FUNCTION);
	ntype->function.return_type            = type_int;
	ntype->function.unspecified_parameters = true;

	type_t *type = typehash_insert(ntype);
	if(type != ntype) {
		free_type(ntype);
	}

	declaration_t *const declaration = allocate_declaration_zero();
	declaration->storage_class   = STORAGE_CLASS_EXTERN;
	declaration->type            = type;
	declaration->symbol          = symbol;
	declaration->source_position = source_position;
	declaration->parent_scope  = global_scope;

	scope_t *old_scope = scope;
	set_scope(global_scope);

	environment_push(declaration);
	/* prepends the declaration to the global declarations list */
	declaration->next   = scope->declarations;
	scope->declarations = declaration;

	assert(scope == global_scope);
	set_scope(old_scope);

	return declaration;
}

/**
 * Creates a return_type (func)(argument_type) function type if not
 * already exists.
 *
 * @param return_type    the return type
 * @param argument_type  the argument type
 */
static type_t *make_function_1_type(type_t *return_type, type_t *argument_type)
{
	function_parameter_t *parameter
		= obstack_alloc(type_obst, sizeof(parameter[0]));
	memset(parameter, 0, sizeof(parameter[0]));
	parameter->type = argument_type;

	type_t *type               = allocate_type_zero(TYPE_FUNCTION);
	type->function.return_type = return_type;
	type->function.parameters  = parameter;

	type_t *result = typehash_insert(type);
	if(result != type) {
		free_type(type);
	}

	return result;
}

/**
 * Creates a function type for some function like builtins.
 *
 * @param symbol   the symbol describing the builtin
 */
static type_t *get_builtin_symbol_type(symbol_t *symbol)
{
	switch(symbol->ID) {
	case T___builtin_alloca:
		return make_function_1_type(type_void_ptr, type_size_t);
	case T___builtin_nan:
		return make_function_1_type(type_double, type_char_ptr);
	case T___builtin_nanf:
		return make_function_1_type(type_float, type_char_ptr);
	case T___builtin_nand:
		return make_function_1_type(type_long_double, type_char_ptr);
	case T___builtin_va_end:
		return make_function_1_type(type_void, type_valist);
	default:
		panic("not implemented builtin symbol found");
	}
}

/**
 * Performs automatic type cast as described in § 6.3.2.1.
 *
 * @param orig_type  the original type
 */
static type_t *automatic_type_conversion(type_t *orig_type)
{
	type_t *type = skip_typeref(orig_type);
	if(is_type_array(type)) {
		array_type_t *array_type   = &type->array;
		type_t       *element_type = array_type->element_type;
		unsigned      qualifiers   = array_type->type.qualifiers;

		return make_pointer_type(element_type, qualifiers);
	}

	if(is_type_function(type)) {
		return make_pointer_type(orig_type, TYPE_QUALIFIER_NONE);
	}

	return orig_type;
}

/**
 * reverts the automatic casts of array to pointer types and function
 * to function-pointer types as defined § 6.3.2.1
 */
type_t *revert_automatic_type_conversion(const expression_t *expression)
{
	switch (expression->kind) {
		case EXPR_REFERENCE: return expression->reference.declaration->type;
		case EXPR_SELECT:    return expression->select.compound_entry->type;

		case EXPR_UNARY_DEREFERENCE: {
			const expression_t *const value = expression->unary.value;
			type_t             *const type  = skip_typeref(value->base.type);
			assert(is_type_pointer(type));
			return type->pointer.points_to;
		}

		case EXPR_BUILTIN_SYMBOL:
			return get_builtin_symbol_type(expression->builtin_symbol.symbol);

		case EXPR_ARRAY_ACCESS: {
			const expression_t *array_ref = expression->array_access.array_ref;
			type_t             *type_left = skip_typeref(array_ref->base.type);
			if (!is_type_valid(type_left))
				return type_left;
			assert(is_type_pointer(type_left));
			return type_left->pointer.points_to;
		}

		default: break;
	}

	return expression->base.type;
}

static expression_t *parse_reference(void)
{
	expression_t *expression = allocate_expression_zero(EXPR_REFERENCE);

	reference_expression_t *ref = &expression->reference;
	ref->symbol = token.v.symbol;

	declaration_t *declaration = get_declaration(ref->symbol, NAMESPACE_NORMAL);

	source_position_t source_position = token.source_position;
	next_token();

	if(declaration == NULL) {
		if (! strict_mode && token.type == '(') {
			/* an implicitly defined function */
			if (warning.implicit_function_declaration) {
				warningf(HERE, "implicit declaration of function '%Y'",
					ref->symbol);
			}

			declaration = create_implicit_function(ref->symbol,
			                                       source_position);
		} else {
			errorf(HERE, "unknown symbol '%Y' found.", ref->symbol);
			return expression;
		}
	}

	type_t *type         = declaration->type;

	/* we always do the auto-type conversions; the & and sizeof parser contains
	 * code to revert this! */
	type = automatic_type_conversion(type);

	ref->declaration = declaration;
	ref->base.type   = type;

	/* this declaration is used */
	declaration->used = true;

	return expression;
}

static void check_cast_allowed(expression_t *expression, type_t *dest_type)
{
	(void) expression;
	(void) dest_type;
	/* TODO check if explicit cast is allowed and issue warnings/errors */
}

static expression_t *parse_cast(void)
{
	expression_t *cast = allocate_expression_zero(EXPR_UNARY_CAST);

	cast->base.source_position = token.source_position;

	type_t *type  = parse_typename();

	expect(')');
	expression_t *value = parse_sub_expression(20);

	check_cast_allowed(value, type);

	cast->base.type   = type;
	cast->unary.value = value;

	return cast;
}

static expression_t *parse_statement_expression(void)
{
	expression_t *expression = allocate_expression_zero(EXPR_STATEMENT);

	statement_t *statement           = parse_compound_statement();
	expression->statement.statement  = statement;
	expression->base.source_position = statement->base.source_position;

	/* find last statement and use its type */
	type_t *type = type_void;
	const statement_t *stmt = statement->compound.statements;
	if (stmt != NULL) {
		while (stmt->base.next != NULL)
			stmt = stmt->base.next;

		if (stmt->kind == STATEMENT_EXPRESSION) {
			type = stmt->expression.expression->base.type;
		}
	} else {
		warningf(expression->base.source_position, "empty statement expression ({})");
	}
	expression->base.type = type;

	expect(')');

	return expression;
}

static expression_t *parse_brace_expression(void)
{
	eat('(');

	switch(token.type) {
	case '{':
		/* gcc extension: a statement expression */
		return parse_statement_expression();

	TYPE_QUALIFIERS
	TYPE_SPECIFIERS
		return parse_cast();
	case T_IDENTIFIER:
		if(is_typedef_symbol(token.v.symbol)) {
			return parse_cast();
		}
	}

	expression_t *result = parse_expression();
	expect(')');

	return result;
}

static expression_t *parse_function_keyword(void)
{
	next_token();
	/* TODO */

	if (current_function == NULL) {
		errorf(HERE, "'__func__' used outside of a function");
	}

	expression_t *expression = allocate_expression_zero(EXPR_FUNCTION);
	expression->base.type    = type_char_ptr;

	return expression;
}

static expression_t *parse_pretty_function_keyword(void)
{
	eat(T___PRETTY_FUNCTION__);
	/* TODO */

	if (current_function == NULL) {
		errorf(HERE, "'__PRETTY_FUNCTION__' used outside of a function");
	}

	expression_t *expression = allocate_expression_zero(EXPR_PRETTY_FUNCTION);
	expression->base.type    = type_char_ptr;

	return expression;
}

static designator_t *parse_designator(void)
{
	designator_t *result = allocate_ast_zero(sizeof(result[0]));

	if(token.type != T_IDENTIFIER) {
		parse_error_expected("while parsing member designator",
		                     T_IDENTIFIER, 0);
		eat_paren();
		return NULL;
	}
	result->symbol = token.v.symbol;
	next_token();

	designator_t *last_designator = result;
	while(true) {
		if(token.type == '.') {
			next_token();
			if(token.type != T_IDENTIFIER) {
				parse_error_expected("while parsing member designator",
				                     T_IDENTIFIER, 0);
				eat_paren();
				return NULL;
			}
			designator_t *designator = allocate_ast_zero(sizeof(result[0]));
			designator->symbol       = token.v.symbol;
			next_token();

			last_designator->next = designator;
			last_designator       = designator;
			continue;
		}
		if(token.type == '[') {
			next_token();
			designator_t *designator = allocate_ast_zero(sizeof(result[0]));
			designator->array_access = parse_expression();
			if(designator->array_access == NULL) {
				eat_paren();
				return NULL;
			}
			expect(']');

			last_designator->next = designator;
			last_designator       = designator;
			continue;
		}
		break;
	}

	return result;
}

static expression_t *parse_offsetof(void)
{
	eat(T___builtin_offsetof);

	expression_t *expression = allocate_expression_zero(EXPR_OFFSETOF);
	expression->base.type    = type_size_t;

	expect('(');
	expression->offsetofe.type = parse_typename();
	expect(',');
	expression->offsetofe.designator = parse_designator();
	expect(')');

	return expression;
}

static expression_t *parse_va_start(void)
{
	eat(T___builtin_va_start);

	expression_t *expression = allocate_expression_zero(EXPR_VA_START);

	expect('(');
	expression->va_starte.ap = parse_assignment_expression();
	expect(',');
	expression_t *const expr = parse_assignment_expression();
	if (expr->kind == EXPR_REFERENCE) {
		declaration_t *const decl = expr->reference.declaration;
		if (decl == NULL)
			return create_invalid_expression();
		if (decl->parent_scope == &current_function->scope &&
		    decl->next == NULL) {
			expression->va_starte.parameter = decl;
			expect(')');
			return expression;
		}
	}
	errorf(expr->base.source_position, "second argument of 'va_start' must be last parameter of the current function");

	return create_invalid_expression();
}

static expression_t *parse_va_arg(void)
{
	eat(T___builtin_va_arg);

	expression_t *expression = allocate_expression_zero(EXPR_VA_ARG);

	expect('(');
	expression->va_arge.ap = parse_assignment_expression();
	expect(',');
	expression->base.type = parse_typename();
	expect(')');

	return expression;
}

static expression_t *parse_builtin_symbol(void)
{
	expression_t *expression = allocate_expression_zero(EXPR_BUILTIN_SYMBOL);

	symbol_t *symbol = token.v.symbol;

	expression->builtin_symbol.symbol = symbol;
	next_token();

	type_t *type = get_builtin_symbol_type(symbol);
	type = automatic_type_conversion(type);

	expression->base.type = type;
	return expression;
}

static expression_t *parse_builtin_constant(void)
{
	eat(T___builtin_constant_p);

	expression_t *expression = allocate_expression_zero(EXPR_BUILTIN_CONSTANT_P);

	expect('(');
	expression->builtin_constant.value = parse_assignment_expression();
	expect(')');
	expression->base.type = type_int;

	return expression;
}

static expression_t *parse_builtin_prefetch(void)
{
	eat(T___builtin_prefetch);

	expression_t *expression = allocate_expression_zero(EXPR_BUILTIN_PREFETCH);

	expect('(');
	expression->builtin_prefetch.adr = parse_assignment_expression();
	if (token.type == ',') {
		next_token();
		expression->builtin_prefetch.rw = parse_assignment_expression();
	}
	if (token.type == ',') {
		next_token();
		expression->builtin_prefetch.locality = parse_assignment_expression();
	}
	expect(')');
	expression->base.type = type_void;

	return expression;
}

static expression_t *parse_compare_builtin(void)
{
	expression_t *expression;

	switch(token.type) {
	case T___builtin_isgreater:
		expression = allocate_expression_zero(EXPR_BINARY_ISGREATER);
		break;
	case T___builtin_isgreaterequal:
		expression = allocate_expression_zero(EXPR_BINARY_ISGREATEREQUAL);
		break;
	case T___builtin_isless:
		expression = allocate_expression_zero(EXPR_BINARY_ISLESS);
		break;
	case T___builtin_islessequal:
		expression = allocate_expression_zero(EXPR_BINARY_ISLESSEQUAL);
		break;
	case T___builtin_islessgreater:
		expression = allocate_expression_zero(EXPR_BINARY_ISLESSGREATER);
		break;
	case T___builtin_isunordered:
		expression = allocate_expression_zero(EXPR_BINARY_ISUNORDERED);
		break;
	default:
		panic("invalid compare builtin found");
		break;
	}
	expression->base.source_position = HERE;
	next_token();

	expect('(');
	expression->binary.left = parse_assignment_expression();
	expect(',');
	expression->binary.right = parse_assignment_expression();
	expect(')');

	type_t *const orig_type_left  = expression->binary.left->base.type;
	type_t *const orig_type_right = expression->binary.right->base.type;

	type_t *const type_left  = skip_typeref(orig_type_left);
	type_t *const type_right = skip_typeref(orig_type_right);
	if(!is_type_float(type_left) && !is_type_float(type_right)) {
		if (is_type_valid(type_left) && is_type_valid(type_right)) {
			type_error_incompatible("invalid operands in comparison",
				expression->base.source_position, orig_type_left, orig_type_right);
		}
	} else {
		semantic_comparison(&expression->binary);
	}

	return expression;
}

static expression_t *parse_builtin_expect(void)
{
	eat(T___builtin_expect);

	expression_t *expression
		= allocate_expression_zero(EXPR_BINARY_BUILTIN_EXPECT);

	expect('(');
	expression->binary.left = parse_assignment_expression();
	expect(',');
	expression->binary.right = parse_constant_expression();
	expect(')');

	expression->base.type = expression->binary.left->base.type;

	return expression;
}

static expression_t *parse_assume(void) {
	eat(T_assume);

	expression_t *expression
		= allocate_expression_zero(EXPR_UNARY_ASSUME);

	expect('(');
	expression->unary.value = parse_assignment_expression();
	expect(')');

	expression->base.type = type_void;
	return expression;
}

static expression_t *parse_primary_expression(void)
{
	switch(token.type) {
	case T_INTEGER:
		return parse_int_const();
	case T_FLOATINGPOINT:
		return parse_float_const();
	case T_STRING_LITERAL:
		return parse_string_const();
	case T_WIDE_STRING_LITERAL:
		return parse_wide_string_const();
	case T_IDENTIFIER:
		return parse_reference();
	case T___FUNCTION__:
	case T___func__:
		return parse_function_keyword();
	case T___PRETTY_FUNCTION__:
		return parse_pretty_function_keyword();
	case T___builtin_offsetof:
		return parse_offsetof();
	case T___builtin_va_start:
		return parse_va_start();
	case T___builtin_va_arg:
		return parse_va_arg();
	case T___builtin_expect:
		return parse_builtin_expect();
	case T___builtin_nanf:
	case T___builtin_alloca:
	case T___builtin_va_end:
		return parse_builtin_symbol();
	case T___builtin_isgreater:
	case T___builtin_isgreaterequal:
	case T___builtin_isless:
	case T___builtin_islessequal:
	case T___builtin_islessgreater:
	case T___builtin_isunordered:
		return parse_compare_builtin();
	case T___builtin_constant_p:
		return parse_builtin_constant();
	case T___builtin_prefetch:
		return parse_builtin_prefetch();
	case T_assume:
		return parse_assume();

	case '(':
		return parse_brace_expression();
	}

	errorf(HERE, "unexpected token '%K'", &token);
	eat_statement();

	return create_invalid_expression();
}

/**
 * Check if the expression has the character type and issue a warning then.
 */
static void check_for_char_index_type(const expression_t *expression) {
	type_t       *const type      = expression->base.type;
	const type_t *const base_type = skip_typeref(type);

	if (is_type_atomic(base_type, ATOMIC_TYPE_CHAR) &&
			warning.char_subscripts) {
		warningf(expression->base.source_position,
			"array subscript has type '%T'", type);
	}
}

static expression_t *parse_array_expression(unsigned precedence,
                                            expression_t *left)
{
	(void) precedence;

	eat('[');

	expression_t *inside = parse_expression();

	expression_t *expression = allocate_expression_zero(EXPR_ARRAY_ACCESS);

	array_access_expression_t *array_access = &expression->array_access;

	type_t *const orig_type_left   = left->base.type;
	type_t *const orig_type_inside = inside->base.type;

	type_t *const type_left   = skip_typeref(orig_type_left);
	type_t *const type_inside = skip_typeref(orig_type_inside);

	type_t *return_type;
	if (is_type_pointer(type_left)) {
		return_type             = type_left->pointer.points_to;
		array_access->array_ref = left;
		array_access->index     = inside;
		check_for_char_index_type(inside);
	} else if (is_type_pointer(type_inside)) {
		return_type             = type_inside->pointer.points_to;
		array_access->array_ref = inside;
		array_access->index     = left;
		array_access->flipped   = true;
		check_for_char_index_type(left);
	} else {
		if (is_type_valid(type_left) && is_type_valid(type_inside)) {
			errorf(HERE,
				"array access on object with non-pointer types '%T', '%T'",
				orig_type_left, orig_type_inside);
		}
		return_type             = type_error_type;
		array_access->array_ref = create_invalid_expression();
	}

	if(token.type != ']') {
		parse_error_expected("Problem while parsing array access", ']', 0);
		return expression;
	}
	next_token();

	return_type           = automatic_type_conversion(return_type);
	expression->base.type = return_type;

	return expression;
}

static expression_t *parse_typeprop(expression_kind_t kind, unsigned precedence)
{
	expression_t *tp_expression = allocate_expression_zero(kind);
	tp_expression->base.type    = type_size_t;

	if(token.type == '(' && is_declaration_specifier(look_ahead(1), true)) {
		next_token();
		tp_expression->typeprop.type = parse_typename();
		expect(')');
	} else {
		expression_t *expression = parse_sub_expression(precedence);
		expression->base.type    = revert_automatic_type_conversion(expression);

		tp_expression->typeprop.type          = expression->base.type;
		tp_expression->typeprop.tp_expression = expression;
	}

	return tp_expression;
}

static expression_t *parse_sizeof(unsigned precedence)
{
	eat(T_sizeof);
	return parse_typeprop(EXPR_SIZEOF, precedence);
}

static expression_t *parse_alignof(unsigned precedence)
{
	eat(T___alignof__);
	return parse_typeprop(EXPR_SIZEOF, precedence);
}

static expression_t *parse_select_expression(unsigned precedence,
                                             expression_t *compound)
{
	(void) precedence;
	assert(token.type == '.' || token.type == T_MINUSGREATER);

	bool is_pointer = (token.type == T_MINUSGREATER);
	next_token();

	expression_t *select    = allocate_expression_zero(EXPR_SELECT);
	select->select.compound = compound;

	if(token.type != T_IDENTIFIER) {
		parse_error_expected("while parsing select", T_IDENTIFIER, 0);
		return select;
	}
	symbol_t *symbol      = token.v.symbol;
	select->select.symbol = symbol;
	next_token();

	type_t *const orig_type = compound->base.type;
	type_t *const type      = skip_typeref(orig_type);

	type_t *type_left = type;
	if(is_pointer) {
		if (!is_type_pointer(type)) {
			if (is_type_valid(type)) {
				errorf(HERE, "left hand side of '->' is not a pointer, but '%T'", orig_type);
			}
			return create_invalid_expression();
		}
		type_left = type->pointer.points_to;
	}
	type_left = skip_typeref(type_left);

	if (type_left->kind != TYPE_COMPOUND_STRUCT &&
	    type_left->kind != TYPE_COMPOUND_UNION) {
		if (is_type_valid(type_left)) {
			errorf(HERE, "request for member '%Y' in something not a struct or "
			       "union, but '%T'", symbol, type_left);
		}
		return create_invalid_expression();
	}

	declaration_t *const declaration = type_left->compound.declaration;

	if(!declaration->init.is_defined) {
		errorf(HERE, "request for member '%Y' of incomplete type '%T'",
		       symbol, type_left);
		return create_invalid_expression();
	}

	declaration_t *iter = declaration->scope.declarations;
	for( ; iter != NULL; iter = iter->next) {
		if(iter->symbol == symbol) {
			break;
		}
	}
	if(iter == NULL) {
		errorf(HERE, "'%T' has no member named '%Y'", orig_type, symbol);
		return create_invalid_expression();
	}

	/* we always do the auto-type conversions; the & and sizeof parser contains
	 * code to revert this! */
	type_t *expression_type = automatic_type_conversion(iter->type);

	select->select.compound_entry = iter;
	select->base.type             = expression_type;

	if(expression_type->kind == TYPE_BITFIELD) {
		expression_t *extract
			= allocate_expression_zero(EXPR_UNARY_BITFIELD_EXTRACT);
		extract->unary.value = select;
		extract->base.type   = expression_type->bitfield.base;

		return extract;
	}

	return select;
}

/**
 * Parse a call expression, ie. expression '( ... )'.
 *
 * @param expression  the function address
 */
static expression_t *parse_call_expression(unsigned precedence,
                                           expression_t *expression)
{
	(void) precedence;
	expression_t *result = allocate_expression_zero(EXPR_CALL);

	call_expression_t *call = &result->call;
	call->function          = expression;

	type_t *const orig_type = expression->base.type;
	type_t *const type      = skip_typeref(orig_type);

	function_type_t *function_type = NULL;
	if (is_type_pointer(type)) {
		type_t *const to_type = skip_typeref(type->pointer.points_to);

		if (is_type_function(to_type)) {
			function_type   = &to_type->function;
			call->base.type = function_type->return_type;
		}
	}

	if (function_type == NULL && is_type_valid(type)) {
		errorf(HERE, "called object '%E' (type '%T') is not a pointer to a function", expression, orig_type);
	}

	/* parse arguments */
	eat('(');

	if(token.type != ')') {
		call_argument_t *last_argument = NULL;

		while(true) {
			call_argument_t *argument = allocate_ast_zero(sizeof(argument[0]));

			argument->expression = parse_assignment_expression();
			if(last_argument == NULL) {
				call->arguments = argument;
			} else {
				last_argument->next = argument;
			}
			last_argument = argument;

			if(token.type != ',')
				break;
			next_token();
		}
	}
	expect(')');

	if(function_type != NULL) {
		function_parameter_t *parameter = function_type->parameters;
		call_argument_t      *argument  = call->arguments;
		for( ; parameter != NULL && argument != NULL;
				parameter = parameter->next, argument = argument->next) {
			type_t *expected_type = parameter->type;
			/* TODO report scope in error messages */
			expression_t *const arg_expr = argument->expression;
			type_t       *const res_type = semantic_assign(expected_type, arg_expr, "function call");
			if (res_type == NULL) {
				/* TODO improve error message */
				errorf(arg_expr->base.source_position,
					"Cannot call function with argument '%E' of type '%T' where type '%T' is expected",
					arg_expr, arg_expr->base.type, expected_type);
			} else {
				argument->expression = create_implicit_cast(argument->expression, expected_type);
			}
		}
		/* too few parameters */
		if(parameter != NULL) {
			errorf(HERE, "too few arguments to function '%E'", expression);
		} else if(argument != NULL) {
			/* too many parameters */
			if(!function_type->variadic
					&& !function_type->unspecified_parameters) {
				errorf(HERE, "too many arguments to function '%E'", expression);
			} else {
				/* do default promotion */
				for( ; argument != NULL; argument = argument->next) {
					type_t *type = argument->expression->base.type;

					type = skip_typeref(type);
					if(is_type_integer(type)) {
						type = promote_integer(type);
					} else if(type == type_float) {
						type = type_double;
					}

					argument->expression
						= create_implicit_cast(argument->expression, type);
				}

				check_format(&result->call);
			}
		} else {
			check_format(&result->call);
		}
	}

	return result;
}

static type_t *semantic_arithmetic(type_t *type_left, type_t *type_right);

static bool same_compound_type(const type_t *type1, const type_t *type2)
{
	return
		is_type_compound(type1) &&
		type1->kind == type2->kind &&
		type1->compound.declaration == type2->compound.declaration;
}

/**
 * Parse a conditional expression, ie. 'expression ? ... : ...'.
 *
 * @param expression  the conditional expression
 */
static expression_t *parse_conditional_expression(unsigned precedence,
                                                  expression_t *expression)
{
	eat('?');

	expression_t *result = allocate_expression_zero(EXPR_CONDITIONAL);

	conditional_expression_t *conditional = &result->conditional;
	conditional->condition = expression;

	/* 6.5.15.2 */
	type_t *const condition_type_orig = expression->base.type;
	type_t *const condition_type      = skip_typeref(condition_type_orig);
	if (!is_type_scalar(condition_type) && is_type_valid(condition_type)) {
		type_error("expected a scalar type in conditional condition",
		           expression->base.source_position, condition_type_orig);
	}

	expression_t *true_expression = parse_expression();
	expect(':');
	expression_t *false_expression = parse_sub_expression(precedence);

	conditional->true_expression  = true_expression;
	conditional->false_expression = false_expression;

	type_t *const orig_true_type  = true_expression->base.type;
	type_t *const orig_false_type = false_expression->base.type;
	type_t *const true_type       = skip_typeref(orig_true_type);
	type_t *const false_type      = skip_typeref(orig_false_type);

	/* 6.5.15.3 */
	type_t *result_type;
	if (is_type_arithmetic(true_type) && is_type_arithmetic(false_type)) {
		result_type = semantic_arithmetic(true_type, false_type);

		true_expression  = create_implicit_cast(true_expression, result_type);
		false_expression = create_implicit_cast(false_expression, result_type);

		conditional->true_expression  = true_expression;
		conditional->false_expression = false_expression;
		conditional->base.type        = result_type;
	} else if (same_compound_type(true_type, false_type) || (
	    is_type_atomic(true_type, ATOMIC_TYPE_VOID) &&
	    is_type_atomic(false_type, ATOMIC_TYPE_VOID)
		)) {
		/* just take 1 of the 2 types */
		result_type = true_type;
	} else if (is_type_pointer(true_type) && is_type_pointer(false_type)
			&& pointers_compatible(true_type, false_type)) {
		/* ok */
		result_type = true_type;
	} else {
		/* TODO */
		if (is_type_valid(true_type) && is_type_valid(false_type)) {
			type_error_incompatible("while parsing conditional",
			                        expression->base.source_position, true_type,
			                        false_type);
		}
		result_type = type_error_type;
	}

	conditional->base.type = result_type;
	return result;
}

/**
 * Parse an extension expression.
 */
static expression_t *parse_extension(unsigned precedence)
{
	eat(T___extension__);

	/* TODO enable extensions */
	expression_t *expression = parse_sub_expression(precedence);
	/* TODO disable extensions */
	return expression;
}

static expression_t *parse_builtin_classify_type(const unsigned precedence)
{
	eat(T___builtin_classify_type);

	expression_t *result = allocate_expression_zero(EXPR_CLASSIFY_TYPE);
	result->base.type    = type_int;

	expect('(');
	expression_t *expression = parse_sub_expression(precedence);
	expect(')');
	result->classify_type.type_expression = expression;

	return result;
}

static void semantic_incdec(unary_expression_t *expression)
{
	type_t *const orig_type = expression->value->base.type;
	type_t *const type      = skip_typeref(orig_type);
	/* TODO !is_type_real && !is_type_pointer */
	if(!is_type_arithmetic(type) && type->kind != TYPE_POINTER) {
		if (is_type_valid(type)) {
			/* TODO: improve error message */
			errorf(HERE, "operation needs an arithmetic or pointer type");
		}
		return;
	}

	expression->base.type = orig_type;
}

static void semantic_unexpr_arithmetic(unary_expression_t *expression)
{
	type_t *const orig_type = expression->value->base.type;
	type_t *const type      = skip_typeref(orig_type);
	if(!is_type_arithmetic(type)) {
		if (is_type_valid(type)) {
			/* TODO: improve error message */
			errorf(HERE, "operation needs an arithmetic type");
		}
		return;
	}

	expression->base.type = orig_type;
}

static void semantic_unexpr_scalar(unary_expression_t *expression)
{
	type_t *const orig_type = expression->value->base.type;
	type_t *const type      = skip_typeref(orig_type);
	if (!is_type_scalar(type)) {
		if (is_type_valid(type)) {
			errorf(HERE, "operand of ! must be of scalar type");
		}
		return;
	}

	expression->base.type = orig_type;
}

static void semantic_unexpr_integer(unary_expression_t *expression)
{
	type_t *const orig_type = expression->value->base.type;
	type_t *const type      = skip_typeref(orig_type);
	if (!is_type_integer(type)) {
		if (is_type_valid(type)) {
			errorf(HERE, "operand of ~ must be of integer type");
		}
		return;
	}

	expression->base.type = orig_type;
}

static void semantic_dereference(unary_expression_t *expression)
{
	type_t *const orig_type = expression->value->base.type;
	type_t *const type      = skip_typeref(orig_type);
	if(!is_type_pointer(type)) {
		if (is_type_valid(type)) {
			errorf(HERE, "Unary '*' needs pointer or arrray type, but type '%T' given", orig_type);
		}
		return;
	}

	type_t *result_type   = type->pointer.points_to;
	result_type           = automatic_type_conversion(result_type);
	expression->base.type = result_type;
}

/**
 * Check the semantic of the address taken expression.
 */
static void semantic_take_addr(unary_expression_t *expression)
{
	expression_t *value = expression->value;
	value->base.type    = revert_automatic_type_conversion(value);

	type_t *orig_type = value->base.type;
	if(!is_type_valid(orig_type))
		return;

	if(value->kind == EXPR_REFERENCE) {
		declaration_t *const declaration = value->reference.declaration;
		if(declaration != NULL) {
			if (declaration->storage_class == STORAGE_CLASS_REGISTER) {
				errorf(expression->base.source_position,
					"address of register variable '%Y' requested",
					declaration->symbol);
			}
			declaration->address_taken = 1;
		}
	}

	expression->base.type = make_pointer_type(orig_type, TYPE_QUALIFIER_NONE);
}

#define CREATE_UNARY_EXPRESSION_PARSER(token_type, unexpression_type, sfunc)   \
static expression_t *parse_##unexpression_type(unsigned precedence)            \
{                                                                              \
	eat(token_type);                                                           \
	                                                                           \
	expression_t *unary_expression                                             \
		= allocate_expression_zero(unexpression_type);                         \
	unary_expression->base.source_position = HERE;                             \
	unary_expression->unary.value = parse_sub_expression(precedence);          \
	                                                                           \
	sfunc(&unary_expression->unary);                                           \
	                                                                           \
	return unary_expression;                                                   \
}

CREATE_UNARY_EXPRESSION_PARSER('-', EXPR_UNARY_NEGATE,
                               semantic_unexpr_arithmetic)
CREATE_UNARY_EXPRESSION_PARSER('+', EXPR_UNARY_PLUS,
                               semantic_unexpr_arithmetic)
CREATE_UNARY_EXPRESSION_PARSER('!', EXPR_UNARY_NOT,
                               semantic_unexpr_scalar)
CREATE_UNARY_EXPRESSION_PARSER('*', EXPR_UNARY_DEREFERENCE,
                               semantic_dereference)
CREATE_UNARY_EXPRESSION_PARSER('&', EXPR_UNARY_TAKE_ADDRESS,
                               semantic_take_addr)
CREATE_UNARY_EXPRESSION_PARSER('~', EXPR_UNARY_BITWISE_NEGATE,
                               semantic_unexpr_integer)
CREATE_UNARY_EXPRESSION_PARSER(T_PLUSPLUS,   EXPR_UNARY_PREFIX_INCREMENT,
                               semantic_incdec)
CREATE_UNARY_EXPRESSION_PARSER(T_MINUSMINUS, EXPR_UNARY_PREFIX_DECREMENT,
                               semantic_incdec)

#define CREATE_UNARY_POSTFIX_EXPRESSION_PARSER(token_type, unexpression_type, \
                                               sfunc)                         \
static expression_t *parse_##unexpression_type(unsigned precedence,           \
                                               expression_t *left)            \
{                                                                             \
	(void) precedence;                                                        \
	eat(token_type);                                                          \
                                                                              \
	expression_t *unary_expression                                            \
		= allocate_expression_zero(unexpression_type);                        \
	unary_expression->unary.value = left;                                     \
	                                                                          \
	sfunc(&unary_expression->unary);                                          \
                                                                              \
	return unary_expression;                                                  \
}

CREATE_UNARY_POSTFIX_EXPRESSION_PARSER(T_PLUSPLUS,
                                       EXPR_UNARY_POSTFIX_INCREMENT,
                                       semantic_incdec)
CREATE_UNARY_POSTFIX_EXPRESSION_PARSER(T_MINUSMINUS,
                                       EXPR_UNARY_POSTFIX_DECREMENT,
                                       semantic_incdec)

static type_t *semantic_arithmetic(type_t *type_left, type_t *type_right)
{
	/* TODO: handle complex + imaginary types */

	/* § 6.3.1.8 Usual arithmetic conversions */
	if(type_left == type_long_double || type_right == type_long_double) {
		return type_long_double;
	} else if(type_left == type_double || type_right == type_double) {
		return type_double;
	} else if(type_left == type_float || type_right == type_float) {
		return type_float;
	}

	type_right = promote_integer(type_right);
	type_left  = promote_integer(type_left);

	if(type_left == type_right)
		return type_left;

	bool signed_left  = is_type_signed(type_left);
	bool signed_right = is_type_signed(type_right);
	int  rank_left    = get_rank(type_left);
	int  rank_right   = get_rank(type_right);
	if(rank_left < rank_right) {
		if(signed_left == signed_right || !signed_right) {
			return type_right;
		} else {
			return type_left;
		}
	} else {
		if(signed_left == signed_right || !signed_left) {
			return type_left;
		} else {
			return type_right;
		}
	}
}

/**
 * Check the semantic restrictions for a binary expression.
 */
static void semantic_binexpr_arithmetic(binary_expression_t *expression)
{
	expression_t *const left            = expression->left;
	expression_t *const right           = expression->right;
	type_t       *const orig_type_left  = left->base.type;
	type_t       *const orig_type_right = right->base.type;
	type_t       *const type_left       = skip_typeref(orig_type_left);
	type_t       *const type_right      = skip_typeref(orig_type_right);

	if(!is_type_arithmetic(type_left) || !is_type_arithmetic(type_right)) {
		/* TODO: improve error message */
		if (is_type_valid(type_left) && is_type_valid(type_right)) {
			errorf(HERE, "operation needs arithmetic types");
		}
		return;
	}

	type_t *arithmetic_type = semantic_arithmetic(type_left, type_right);
	expression->left      = create_implicit_cast(left, arithmetic_type);
	expression->right     = create_implicit_cast(right, arithmetic_type);
	expression->base.type = arithmetic_type;
}

static void semantic_shift_op(binary_expression_t *expression)
{
	expression_t *const left            = expression->left;
	expression_t *const right           = expression->right;
	type_t       *const orig_type_left  = left->base.type;
	type_t       *const orig_type_right = right->base.type;
	type_t       *      type_left       = skip_typeref(orig_type_left);
	type_t       *      type_right      = skip_typeref(orig_type_right);

	if(!is_type_integer(type_left) || !is_type_integer(type_right)) {
		/* TODO: improve error message */
		if (is_type_valid(type_left) && is_type_valid(type_right)) {
			errorf(HERE, "operation needs integer types");
		}
		return;
	}

	type_left  = promote_integer(type_left);
	type_right = promote_integer(type_right);

	expression->left      = create_implicit_cast(left, type_left);
	expression->right     = create_implicit_cast(right, type_right);
	expression->base.type = type_left;
}

static void semantic_add(binary_expression_t *expression)
{
	expression_t *const left            = expression->left;
	expression_t *const right           = expression->right;
	type_t       *const orig_type_left  = left->base.type;
	type_t       *const orig_type_right = right->base.type;
	type_t       *const type_left       = skip_typeref(orig_type_left);
	type_t       *const type_right      = skip_typeref(orig_type_right);

	/* § 5.6.5 */
	if(is_type_arithmetic(type_left) && is_type_arithmetic(type_right)) {
		type_t *arithmetic_type = semantic_arithmetic(type_left, type_right);
		expression->left  = create_implicit_cast(left, arithmetic_type);
		expression->right = create_implicit_cast(right, arithmetic_type);
		expression->base.type = arithmetic_type;
		return;
	} else if(is_type_pointer(type_left) && is_type_integer(type_right)) {
		expression->base.type = type_left;
	} else if(is_type_pointer(type_right) && is_type_integer(type_left)) {
		expression->base.type = type_right;
	} else if (is_type_valid(type_left) && is_type_valid(type_right)) {
		errorf(HERE, "invalid operands to binary + ('%T', '%T')", orig_type_left, orig_type_right);
	}
}

static void semantic_sub(binary_expression_t *expression)
{
	expression_t *const left            = expression->left;
	expression_t *const right           = expression->right;
	type_t       *const orig_type_left  = left->base.type;
	type_t       *const orig_type_right = right->base.type;
	type_t       *const type_left       = skip_typeref(orig_type_left);
	type_t       *const type_right      = skip_typeref(orig_type_right);

	/* § 5.6.5 */
	if(is_type_arithmetic(type_left) && is_type_arithmetic(type_right)) {
		type_t *arithmetic_type = semantic_arithmetic(type_left, type_right);
		expression->left        = create_implicit_cast(left, arithmetic_type);
		expression->right       = create_implicit_cast(right, arithmetic_type);
		expression->base.type =  arithmetic_type;
		return;
	} else if(is_type_pointer(type_left) && is_type_integer(type_right)) {
		expression->base.type = type_left;
	} else if(is_type_pointer(type_left) && is_type_pointer(type_right)) {
		if(!pointers_compatible(type_left, type_right)) {
			errorf(HERE,
			       "pointers to incompatible objects to binary '-' ('%T', '%T')",
			       orig_type_left, orig_type_right);
		} else {
			expression->base.type = type_ptrdiff_t;
		}
	} else if (is_type_valid(type_left) && is_type_valid(type_right)) {
		errorf(HERE, "invalid operands to binary '-' ('%T', '%T')",
		       orig_type_left, orig_type_right);
	}
}

/**
 * Check the semantics of comparison expressions.
 *
 * @param expression   The expression to check.
 */
static void semantic_comparison(binary_expression_t *expression)
{
	expression_t *left            = expression->left;
	expression_t *right           = expression->right;
	type_t       *orig_type_left  = left->base.type;
	type_t       *orig_type_right = right->base.type;

	type_t *type_left  = skip_typeref(orig_type_left);
	type_t *type_right = skip_typeref(orig_type_right);

	/* TODO non-arithmetic types */
	if(is_type_arithmetic(type_left) && is_type_arithmetic(type_right)) {
		if (warning.sign_compare &&
		    (expression->base.kind != EXPR_BINARY_EQUAL &&
		     expression->base.kind != EXPR_BINARY_NOTEQUAL) &&
		    (is_type_signed(type_left) != is_type_signed(type_right))) {
			warningf(expression->base.source_position,
			         "comparison between signed and unsigned");
		}
		type_t *arithmetic_type = semantic_arithmetic(type_left, type_right);
		expression->left        = create_implicit_cast(left, arithmetic_type);
		expression->right       = create_implicit_cast(right, arithmetic_type);
		expression->base.type   = arithmetic_type;
		if (warning.float_equal &&
		    (expression->base.kind == EXPR_BINARY_EQUAL ||
		     expression->base.kind == EXPR_BINARY_NOTEQUAL) &&
		    is_type_float(arithmetic_type)) {
			warningf(expression->base.source_position,
			         "comparing floating point with == or != is unsafe");
		}
	} else if (is_type_pointer(type_left) && is_type_pointer(type_right)) {
		/* TODO check compatibility */
	} else if (is_type_pointer(type_left)) {
		expression->right = create_implicit_cast(right, type_left);
	} else if (is_type_pointer(type_right)) {
		expression->left = create_implicit_cast(left, type_right);
	} else if (is_type_valid(type_left) && is_type_valid(type_right)) {
		type_error_incompatible("invalid operands in comparison",
		                        expression->base.source_position,
		                        type_left, type_right);
	}
	expression->base.type = type_int;
}

static void semantic_arithmetic_assign(binary_expression_t *expression)
{
	expression_t *left            = expression->left;
	expression_t *right           = expression->right;
	type_t       *orig_type_left  = left->base.type;
	type_t       *orig_type_right = right->base.type;

	type_t *type_left  = skip_typeref(orig_type_left);
	type_t *type_right = skip_typeref(orig_type_right);

	if(!is_type_arithmetic(type_left) || !is_type_arithmetic(type_right)) {
		/* TODO: improve error message */
		if (is_type_valid(type_left) && is_type_valid(type_right)) {
			errorf(HERE, "operation needs arithmetic types");
		}
		return;
	}

	/* combined instructions are tricky. We can't create an implicit cast on
	 * the left side, because we need the uncasted form for the store.
	 * The ast2firm pass has to know that left_type must be right_type
	 * for the arithmetic operation and create a cast by itself */
	type_t *arithmetic_type = semantic_arithmetic(type_left, type_right);
	expression->right       = create_implicit_cast(right, arithmetic_type);
	expression->base.type   = type_left;
}

static void semantic_arithmetic_addsubb_assign(binary_expression_t *expression)
{
	expression_t *const left            = expression->left;
	expression_t *const right           = expression->right;
	type_t       *const orig_type_left  = left->base.type;
	type_t       *const orig_type_right = right->base.type;
	type_t       *const type_left       = skip_typeref(orig_type_left);
	type_t       *const type_right      = skip_typeref(orig_type_right);

	if (is_type_arithmetic(type_left) && is_type_arithmetic(type_right)) {
		/* combined instructions are tricky. We can't create an implicit cast on
		 * the left side, because we need the uncasted form for the store.
		 * The ast2firm pass has to know that left_type must be right_type
		 * for the arithmetic operation and create a cast by itself */
		type_t *const arithmetic_type = semantic_arithmetic(type_left, type_right);
		expression->right     = create_implicit_cast(right, arithmetic_type);
		expression->base.type = type_left;
	} else if (is_type_pointer(type_left) && is_type_integer(type_right)) {
		expression->base.type = type_left;
	} else if (is_type_valid(type_left) && is_type_valid(type_right)) {
		errorf(HERE, "incompatible types '%T' and '%T' in assignment", orig_type_left, orig_type_right);
	}
}

/**
 * Check the semantic restrictions of a logical expression.
 */
static void semantic_logical_op(binary_expression_t *expression)
{
	expression_t *const left            = expression->left;
	expression_t *const right           = expression->right;
	type_t       *const orig_type_left  = left->base.type;
	type_t       *const orig_type_right = right->base.type;
	type_t       *const type_left       = skip_typeref(orig_type_left);
	type_t       *const type_right      = skip_typeref(orig_type_right);

	if (!is_type_scalar(type_left) || !is_type_scalar(type_right)) {
		/* TODO: improve error message */
		if (is_type_valid(type_left) && is_type_valid(type_right)) {
			errorf(HERE, "operation needs scalar types");
		}
		return;
	}

	expression->base.type = type_int;
}

/**
 * Checks if a compound type has constant fields.
 */
static bool has_const_fields(const compound_type_t *type)
{
	const scope_t       *scope       = &type->declaration->scope;
	const declaration_t *declaration = scope->declarations;

	for (; declaration != NULL; declaration = declaration->next) {
		if (declaration->namespc != NAMESPACE_NORMAL)
			continue;

		const type_t *decl_type = skip_typeref(declaration->type);
		if (decl_type->base.qualifiers & TYPE_QUALIFIER_CONST)
			return true;
	}
	/* TODO */
	return false;
}

/**
 * Check the semantic restrictions of a binary assign expression.
 */
static void semantic_binexpr_assign(binary_expression_t *expression)
{
	expression_t *left           = expression->left;
	type_t       *orig_type_left = left->base.type;

	type_t *type_left = revert_automatic_type_conversion(left);
	type_left         = skip_typeref(orig_type_left);

	/* must be a modifiable lvalue */
	if (is_type_array(type_left)) {
		errorf(HERE, "cannot assign to arrays ('%E')", left);
		return;
	}
	if(type_left->base.qualifiers & TYPE_QUALIFIER_CONST) {
		errorf(HERE, "assignment to readonly location '%E' (type '%T')", left,
		       orig_type_left);
		return;
	}
	if(is_type_incomplete(type_left)) {
		errorf(HERE,
		       "left-hand side of assignment '%E' has incomplete type '%T'",
		       left, orig_type_left);
		return;
	}
	if(is_type_compound(type_left) && has_const_fields(&type_left->compound)) {
		errorf(HERE, "cannot assign to '%E' because compound type '%T' has readonly fields",
		       left, orig_type_left);
		return;
	}

	type_t *const res_type = semantic_assign(orig_type_left, expression->right,
	                                         "assignment");
	if (res_type == NULL) {
		errorf(expression->base.source_position,
			"cannot assign to '%T' from '%T'",
			orig_type_left, expression->right->base.type);
	} else {
		expression->right = create_implicit_cast(expression->right, res_type);
	}

	expression->base.type = orig_type_left;
}

static bool expression_has_effect(const expression_t *const expr)
{
	switch (expr->kind) {
		case EXPR_UNKNOWN:                   break;
		case EXPR_INVALID:                   break;
		case EXPR_REFERENCE:                 return false;
		case EXPR_CONST:                     return false;
		case EXPR_STRING_LITERAL:            return false;
		case EXPR_WIDE_STRING_LITERAL:       return false;
		case EXPR_CALL: {
			const call_expression_t *const call = &expr->call;
			if (call->function->kind != EXPR_BUILTIN_SYMBOL)
				return true;

			switch (call->function->builtin_symbol.symbol->ID) {
				case T___builtin_va_end: return true;
				default:                 return false;
			}
		}
		case EXPR_CONDITIONAL: {
			const conditional_expression_t *const cond = &expr->conditional;
			return
				expression_has_effect(cond->true_expression) &&
				expression_has_effect(cond->false_expression);
		}
		case EXPR_SELECT:                    return false;
		case EXPR_ARRAY_ACCESS:              return false;
		case EXPR_SIZEOF:                    return false;
		case EXPR_CLASSIFY_TYPE:             return false;
		case EXPR_ALIGNOF:                   return false;

		case EXPR_FUNCTION:                  return false;
		case EXPR_PRETTY_FUNCTION:           return false;
		case EXPR_BUILTIN_SYMBOL:            break; /* handled in EXPR_CALL */
		case EXPR_BUILTIN_CONSTANT_P:        return false;
		case EXPR_BUILTIN_PREFETCH:          return true;
		case EXPR_OFFSETOF:                  return false;
		case EXPR_VA_START:                  return true;
		case EXPR_VA_ARG:                    return true;
		case EXPR_STATEMENT:                 return true; // TODO

		case EXPR_UNARY_NEGATE:              return false;
		case EXPR_UNARY_PLUS:                return false;
		case EXPR_UNARY_BITWISE_NEGATE:      return false;
		case EXPR_UNARY_NOT:                 return false;
		case EXPR_UNARY_DEREFERENCE:         return false;
		case EXPR_UNARY_TAKE_ADDRESS:        return false;
		case EXPR_UNARY_POSTFIX_INCREMENT:   return true;
		case EXPR_UNARY_POSTFIX_DECREMENT:   return true;
		case EXPR_UNARY_PREFIX_INCREMENT:    return true;
		case EXPR_UNARY_PREFIX_DECREMENT:    return true;
		case EXPR_UNARY_CAST:
			return is_type_atomic(expr->base.type, ATOMIC_TYPE_VOID);
		case EXPR_UNARY_CAST_IMPLICIT:       return true;
		case EXPR_UNARY_ASSUME:              return true;
		case EXPR_UNARY_BITFIELD_EXTRACT:    return false;

		case EXPR_BINARY_ADD:                return false;
		case EXPR_BINARY_SUB:                return false;
		case EXPR_BINARY_MUL:                return false;
		case EXPR_BINARY_DIV:                return false;
		case EXPR_BINARY_MOD:                return false;
		case EXPR_BINARY_EQUAL:              return false;
		case EXPR_BINARY_NOTEQUAL:           return false;
		case EXPR_BINARY_LESS:               return false;
		case EXPR_BINARY_LESSEQUAL:          return false;
		case EXPR_BINARY_GREATER:            return false;
		case EXPR_BINARY_GREATEREQUAL:       return false;
		case EXPR_BINARY_BITWISE_AND:        return false;
		case EXPR_BINARY_BITWISE_OR:         return false;
		case EXPR_BINARY_BITWISE_XOR:        return false;
		case EXPR_BINARY_SHIFTLEFT:          return false;
		case EXPR_BINARY_SHIFTRIGHT:         return false;
		case EXPR_BINARY_ASSIGN:             return true;
		case EXPR_BINARY_MUL_ASSIGN:         return true;
		case EXPR_BINARY_DIV_ASSIGN:         return true;
		case EXPR_BINARY_MOD_ASSIGN:         return true;
		case EXPR_BINARY_ADD_ASSIGN:         return true;
		case EXPR_BINARY_SUB_ASSIGN:         return true;
		case EXPR_BINARY_SHIFTLEFT_ASSIGN:   return true;
		case EXPR_BINARY_SHIFTRIGHT_ASSIGN:  return true;
		case EXPR_BINARY_BITWISE_AND_ASSIGN: return true;
		case EXPR_BINARY_BITWISE_XOR_ASSIGN: return true;
		case EXPR_BINARY_BITWISE_OR_ASSIGN:  return true;
		case EXPR_BINARY_LOGICAL_AND:
		case EXPR_BINARY_LOGICAL_OR:
		case EXPR_BINARY_COMMA:
			return expression_has_effect(expr->binary.right);

		case EXPR_BINARY_BUILTIN_EXPECT:     return true;
		case EXPR_BINARY_ISGREATER:          return false;
		case EXPR_BINARY_ISGREATEREQUAL:     return false;
		case EXPR_BINARY_ISLESS:             return false;
		case EXPR_BINARY_ISLESSEQUAL:        return false;
		case EXPR_BINARY_ISLESSGREATER:      return false;
		case EXPR_BINARY_ISUNORDERED:        return false;
	}

	panic("unexpected statement");
}

static void semantic_comma(binary_expression_t *expression)
{
	if (warning.unused_value) {
		const expression_t *const left = expression->left;
		if (!expression_has_effect(left)) {
			warningf(left->base.source_position, "left-hand operand of comma expression has no effect");
		}
	}
	expression->base.type = expression->right->base.type;
}

#define CREATE_BINEXPR_PARSER(token_type, binexpression_type, sfunc, lr)  \
static expression_t *parse_##binexpression_type(unsigned precedence,      \
                                                expression_t *left)       \
{                                                                         \
	eat(token_type);                                                      \
	source_position_t pos = HERE;                                         \
                                                                          \
	expression_t *right = parse_sub_expression(precedence + lr);          \
                                                                          \
	expression_t *binexpr = allocate_expression_zero(binexpression_type); \
	binexpr->base.source_position = pos;                                  \
	binexpr->binary.left  = left;                                         \
	binexpr->binary.right = right;                                        \
	sfunc(&binexpr->binary);                                              \
                                                                          \
	return binexpr;                                                       \
}

CREATE_BINEXPR_PARSER(',', EXPR_BINARY_COMMA,    semantic_comma, 1)
CREATE_BINEXPR_PARSER('*', EXPR_BINARY_MUL,      semantic_binexpr_arithmetic, 1)
CREATE_BINEXPR_PARSER('/', EXPR_BINARY_DIV,      semantic_binexpr_arithmetic, 1)
CREATE_BINEXPR_PARSER('%', EXPR_BINARY_MOD,      semantic_binexpr_arithmetic, 1)
CREATE_BINEXPR_PARSER('+', EXPR_BINARY_ADD,      semantic_add, 1)
CREATE_BINEXPR_PARSER('-', EXPR_BINARY_SUB,      semantic_sub, 1)
CREATE_BINEXPR_PARSER('<', EXPR_BINARY_LESS,     semantic_comparison, 1)
CREATE_BINEXPR_PARSER('>', EXPR_BINARY_GREATER,  semantic_comparison, 1)
CREATE_BINEXPR_PARSER('=', EXPR_BINARY_ASSIGN,   semantic_binexpr_assign, 0)

CREATE_BINEXPR_PARSER(T_EQUALEQUAL,           EXPR_BINARY_EQUAL,
                      semantic_comparison, 1)
CREATE_BINEXPR_PARSER(T_EXCLAMATIONMARKEQUAL, EXPR_BINARY_NOTEQUAL,
                      semantic_comparison, 1)
CREATE_BINEXPR_PARSER(T_LESSEQUAL,            EXPR_BINARY_LESSEQUAL,
                      semantic_comparison, 1)
CREATE_BINEXPR_PARSER(T_GREATEREQUAL,         EXPR_BINARY_GREATEREQUAL,
                      semantic_comparison, 1)

CREATE_BINEXPR_PARSER('&', EXPR_BINARY_BITWISE_AND,
                      semantic_binexpr_arithmetic, 1)
CREATE_BINEXPR_PARSER('|', EXPR_BINARY_BITWISE_OR,
                      semantic_binexpr_arithmetic, 1)
CREATE_BINEXPR_PARSER('^', EXPR_BINARY_BITWISE_XOR,
                      semantic_binexpr_arithmetic, 1)
CREATE_BINEXPR_PARSER(T_ANDAND, EXPR_BINARY_LOGICAL_AND,
                      semantic_logical_op, 1)
CREATE_BINEXPR_PARSER(T_PIPEPIPE, EXPR_BINARY_LOGICAL_OR,
                      semantic_logical_op, 1)
CREATE_BINEXPR_PARSER(T_LESSLESS, EXPR_BINARY_SHIFTLEFT,
                      semantic_shift_op, 1)
CREATE_BINEXPR_PARSER(T_GREATERGREATER, EXPR_BINARY_SHIFTRIGHT,
                      semantic_shift_op, 1)
CREATE_BINEXPR_PARSER(T_PLUSEQUAL, EXPR_BINARY_ADD_ASSIGN,
                      semantic_arithmetic_addsubb_assign, 0)
CREATE_BINEXPR_PARSER(T_MINUSEQUAL, EXPR_BINARY_SUB_ASSIGN,
                      semantic_arithmetic_addsubb_assign, 0)
CREATE_BINEXPR_PARSER(T_ASTERISKEQUAL, EXPR_BINARY_MUL_ASSIGN,
                      semantic_arithmetic_assign, 0)
CREATE_BINEXPR_PARSER(T_SLASHEQUAL, EXPR_BINARY_DIV_ASSIGN,
                      semantic_arithmetic_assign, 0)
CREATE_BINEXPR_PARSER(T_PERCENTEQUAL, EXPR_BINARY_MOD_ASSIGN,
                      semantic_arithmetic_assign, 0)
CREATE_BINEXPR_PARSER(T_LESSLESSEQUAL, EXPR_BINARY_SHIFTLEFT_ASSIGN,
                      semantic_arithmetic_assign, 0)
CREATE_BINEXPR_PARSER(T_GREATERGREATEREQUAL, EXPR_BINARY_SHIFTRIGHT_ASSIGN,
                      semantic_arithmetic_assign, 0)
CREATE_BINEXPR_PARSER(T_ANDEQUAL, EXPR_BINARY_BITWISE_AND_ASSIGN,
                      semantic_arithmetic_assign, 0)
CREATE_BINEXPR_PARSER(T_PIPEEQUAL, EXPR_BINARY_BITWISE_OR_ASSIGN,
                      semantic_arithmetic_assign, 0)
CREATE_BINEXPR_PARSER(T_CARETEQUAL, EXPR_BINARY_BITWISE_XOR_ASSIGN,
                      semantic_arithmetic_assign, 0)

static expression_t *parse_sub_expression(unsigned precedence)
{
	if(token.type < 0) {
		return expected_expression_error();
	}

	expression_parser_function_t *parser
		= &expression_parsers[token.type];
	source_position_t             source_position = token.source_position;
	expression_t                 *left;

	if(parser->parser != NULL) {
		left = parser->parser(parser->precedence);
	} else {
		left = parse_primary_expression();
	}
	assert(left != NULL);
	left->base.source_position = source_position;

	while(true) {
		if(token.type < 0) {
			return expected_expression_error();
		}

		parser = &expression_parsers[token.type];
		if(parser->infix_parser == NULL)
			break;
		if(parser->infix_precedence < precedence)
			break;

		left = parser->infix_parser(parser->infix_precedence, left);

		assert(left != NULL);
		assert(left->kind != EXPR_UNKNOWN);
		left->base.source_position = source_position;
	}

	return left;
}

/**
 * Parse an expression.
 */
static expression_t *parse_expression(void)
{
	return parse_sub_expression(1);
}

/**
 * Register a parser for a prefix-like operator with given precedence.
 *
 * @param parser      the parser function
 * @param token_type  the token type of the prefix token
 * @param precedence  the precedence of the operator
 */
static void register_expression_parser(parse_expression_function parser,
                                       int token_type, unsigned precedence)
{
	expression_parser_function_t *entry = &expression_parsers[token_type];

	if(entry->parser != NULL) {
		diagnosticf("for token '%k'\n", (token_type_t)token_type);
		panic("trying to register multiple expression parsers for a token");
	}
	entry->parser     = parser;
	entry->precedence = precedence;
}

/**
 * Register a parser for an infix operator with given precedence.
 *
 * @param parser      the parser function
 * @param token_type  the token type of the infix operator
 * @param precedence  the precedence of the operator
 */
static void register_infix_parser(parse_expression_infix_function parser,
		int token_type,	unsigned precedence)
{
	expression_parser_function_t *entry = &expression_parsers[token_type];

	if(entry->infix_parser != NULL) {
		diagnosticf("for token '%k'\n", (token_type_t)token_type);
		panic("trying to register multiple infix expression parsers for a "
		      "token");
	}
	entry->infix_parser     = parser;
	entry->infix_precedence = precedence;
}

/**
 * Initialize the expression parsers.
 */
static void init_expression_parsers(void)
{
	memset(&expression_parsers, 0, sizeof(expression_parsers));

	register_infix_parser(parse_array_expression,         '[',              30);
	register_infix_parser(parse_call_expression,          '(',              30);
	register_infix_parser(parse_select_expression,        '.',              30);
	register_infix_parser(parse_select_expression,        T_MINUSGREATER,   30);
	register_infix_parser(parse_EXPR_UNARY_POSTFIX_INCREMENT,
	                                                      T_PLUSPLUS,       30);
	register_infix_parser(parse_EXPR_UNARY_POSTFIX_DECREMENT,
	                                                      T_MINUSMINUS,     30);

	register_infix_parser(parse_EXPR_BINARY_MUL,          '*',              16);
	register_infix_parser(parse_EXPR_BINARY_DIV,          '/',              16);
	register_infix_parser(parse_EXPR_BINARY_MOD,          '%',              16);
	register_infix_parser(parse_EXPR_BINARY_SHIFTLEFT,    T_LESSLESS,       16);
	register_infix_parser(parse_EXPR_BINARY_SHIFTRIGHT,   T_GREATERGREATER, 16);
	register_infix_parser(parse_EXPR_BINARY_ADD,          '+',              15);
	register_infix_parser(parse_EXPR_BINARY_SUB,          '-',              15);
	register_infix_parser(parse_EXPR_BINARY_LESS,         '<',              14);
	register_infix_parser(parse_EXPR_BINARY_GREATER,      '>',              14);
	register_infix_parser(parse_EXPR_BINARY_LESSEQUAL,    T_LESSEQUAL,      14);
	register_infix_parser(parse_EXPR_BINARY_GREATEREQUAL, T_GREATEREQUAL,   14);
	register_infix_parser(parse_EXPR_BINARY_EQUAL,        T_EQUALEQUAL,     13);
	register_infix_parser(parse_EXPR_BINARY_NOTEQUAL,
                                                    T_EXCLAMATIONMARKEQUAL, 13);
	register_infix_parser(parse_EXPR_BINARY_BITWISE_AND,  '&',              12);
	register_infix_parser(parse_EXPR_BINARY_BITWISE_XOR,  '^',              11);
	register_infix_parser(parse_EXPR_BINARY_BITWISE_OR,   '|',              10);
	register_infix_parser(parse_EXPR_BINARY_LOGICAL_AND,  T_ANDAND,          9);
	register_infix_parser(parse_EXPR_BINARY_LOGICAL_OR,   T_PIPEPIPE,        8);
	register_infix_parser(parse_conditional_expression,   '?',               7);
	register_infix_parser(parse_EXPR_BINARY_ASSIGN,       '=',               2);
	register_infix_parser(parse_EXPR_BINARY_ADD_ASSIGN,   T_PLUSEQUAL,       2);
	register_infix_parser(parse_EXPR_BINARY_SUB_ASSIGN,   T_MINUSEQUAL,      2);
	register_infix_parser(parse_EXPR_BINARY_MUL_ASSIGN,   T_ASTERISKEQUAL,   2);
	register_infix_parser(parse_EXPR_BINARY_DIV_ASSIGN,   T_SLASHEQUAL,      2);
	register_infix_parser(parse_EXPR_BINARY_MOD_ASSIGN,   T_PERCENTEQUAL,    2);
	register_infix_parser(parse_EXPR_BINARY_SHIFTLEFT_ASSIGN,
	                                                        T_LESSLESSEQUAL, 2);
	register_infix_parser(parse_EXPR_BINARY_SHIFTRIGHT_ASSIGN,
	                                                  T_GREATERGREATEREQUAL, 2);
	register_infix_parser(parse_EXPR_BINARY_BITWISE_AND_ASSIGN,
	                                                             T_ANDEQUAL, 2);
	register_infix_parser(parse_EXPR_BINARY_BITWISE_OR_ASSIGN,
	                                                            T_PIPEEQUAL, 2);
	register_infix_parser(parse_EXPR_BINARY_BITWISE_XOR_ASSIGN,
	                                                           T_CARETEQUAL, 2);

	register_infix_parser(parse_EXPR_BINARY_COMMA,        ',',               1);

	register_expression_parser(parse_EXPR_UNARY_NEGATE,           '-',      25);
	register_expression_parser(parse_EXPR_UNARY_PLUS,             '+',      25);
	register_expression_parser(parse_EXPR_UNARY_NOT,              '!',      25);
	register_expression_parser(parse_EXPR_UNARY_BITWISE_NEGATE,   '~',      25);
	register_expression_parser(parse_EXPR_UNARY_DEREFERENCE,      '*',      25);
	register_expression_parser(parse_EXPR_UNARY_TAKE_ADDRESS,     '&',      25);
	register_expression_parser(parse_EXPR_UNARY_PREFIX_INCREMENT,
	                                                          T_PLUSPLUS,   25);
	register_expression_parser(parse_EXPR_UNARY_PREFIX_DECREMENT,
	                                                          T_MINUSMINUS, 25);
	register_expression_parser(parse_sizeof,                      T_sizeof, 25);
	register_expression_parser(parse_alignof,                T___alignof__, 25);
	register_expression_parser(parse_extension,            T___extension__, 25);
	register_expression_parser(parse_builtin_classify_type,
	                                             T___builtin_classify_type, 25);
}

/**
 * Parse a asm statement constraints specification.
 */
static asm_constraint_t *parse_asm_constraints(void)
{
	asm_constraint_t *result = NULL;
	asm_constraint_t *last   = NULL;

	while(token.type == T_STRING_LITERAL || token.type == '[') {
		asm_constraint_t *constraint = allocate_ast_zero(sizeof(constraint[0]));
		memset(constraint, 0, sizeof(constraint[0]));

		if(token.type == '[') {
			eat('[');
			if(token.type != T_IDENTIFIER) {
				parse_error_expected("while parsing asm constraint",
				                     T_IDENTIFIER, 0);
				return NULL;
			}
			constraint->symbol = token.v.symbol;

			expect(']');
		}

		constraint->constraints = parse_string_literals();
		expect('(');
		constraint->expression = parse_expression();
		expect(')');

		if(last != NULL) {
			last->next = constraint;
		} else {
			result = constraint;
		}
		last = constraint;

		if(token.type != ',')
			break;
		eat(',');
	}

	return result;
}

/**
 * Parse a asm statement clobber specification.
 */
static asm_clobber_t *parse_asm_clobbers(void)
{
	asm_clobber_t *result = NULL;
	asm_clobber_t *last   = NULL;

	while(token.type == T_STRING_LITERAL) {
		asm_clobber_t *clobber = allocate_ast_zero(sizeof(clobber[0]));
		clobber->clobber       = parse_string_literals();

		if(last != NULL) {
			last->next = clobber;
		} else {
			result = clobber;
		}
		last = clobber;

		if(token.type != ',')
			break;
		eat(',');
	}

	return result;
}

/**
 * Parse an asm statement.
 */
static statement_t *parse_asm_statement(void)
{
	eat(T_asm);

	statement_t *statement          = allocate_statement_zero(STATEMENT_ASM);
	statement->base.source_position = token.source_position;

	asm_statement_t *asm_statement = &statement->asms;

	if(token.type == T_volatile) {
		next_token();
		asm_statement->is_volatile = true;
	}

	expect('(');
	asm_statement->asm_text = parse_string_literals();

	if(token.type != ':')
		goto end_of_asm;
	eat(':');

	asm_statement->inputs = parse_asm_constraints();
	if(token.type != ':')
		goto end_of_asm;
	eat(':');

	asm_statement->outputs = parse_asm_constraints();
	if(token.type != ':')
		goto end_of_asm;
	eat(':');

	asm_statement->clobbers = parse_asm_clobbers();

end_of_asm:
	expect(')');
	expect(';');
	return statement;
}

/**
 * Parse a case statement.
 */
static statement_t *parse_case_statement(void)
{
	eat(T_case);

	statement_t *statement = allocate_statement_zero(STATEMENT_CASE_LABEL);

	statement->base.source_position  = token.source_position;
	statement->case_label.expression = parse_expression();

	expect(':');

	if (! is_constant_expression(statement->case_label.expression)) {
		errorf(statement->base.source_position,
			"case label does not reduce to an integer constant");
	} else {
		/* TODO: check if the case label is already known */
		if (current_switch != NULL) {
			/* link all cases into the switch statement */
			if (current_switch->last_case == NULL) {
				current_switch->first_case =
				current_switch->last_case  = &statement->case_label;
			} else {
				current_switch->last_case->next = &statement->case_label;
			}
		} else {
			errorf(statement->base.source_position,
				"case label not within a switch statement");
		}
	}
	statement->case_label.statement = parse_statement();

	return statement;
}

/**
 * Finds an existing default label of a switch statement.
 */
static case_label_statement_t *
find_default_label(const switch_statement_t *statement)
{
	for (case_label_statement_t *label = statement->first_case;
	     label != NULL;
		 label = label->next) {
		if (label->expression == NULL)
			return label;
	}
	return NULL;
}

/**
 * Parse a default statement.
 */
static statement_t *parse_default_statement(void)
{
	eat(T_default);

	statement_t *statement = allocate_statement_zero(STATEMENT_CASE_LABEL);

	statement->base.source_position = token.source_position;

	expect(':');
	if (current_switch != NULL) {
		const case_label_statement_t *def_label = find_default_label(current_switch);
		if (def_label != NULL) {
			errorf(HERE, "multiple default labels in one switch");
			errorf(def_label->base.source_position,
				"this is the first default label");
		} else {
			/* link all cases into the switch statement */
			if (current_switch->last_case == NULL) {
				current_switch->first_case =
					current_switch->last_case  = &statement->case_label;
			} else {
				current_switch->last_case->next = &statement->case_label;
			}
		}
	} else {
		errorf(statement->base.source_position,
			"'default' label not within a switch statement");
	}
	statement->label.statement = parse_statement();

	return statement;
}

/**
 * Return the declaration for a given label symbol or create a new one.
 */
static declaration_t *get_label(symbol_t *symbol)
{
	declaration_t *candidate = get_declaration(symbol, NAMESPACE_LABEL);
	assert(current_function != NULL);
	/* if we found a label in the same function, then we already created the
	 * declaration */
	if(candidate != NULL
			&& candidate->parent_scope == &current_function->scope) {
		return candidate;
	}

	/* otherwise we need to create a new one */
	declaration_t *const declaration = allocate_declaration_zero();
	declaration->namespc       = NAMESPACE_LABEL;
	declaration->symbol        = symbol;

	label_push(declaration);

	return declaration;
}

/**
 * Parse a label statement.
 */
static statement_t *parse_label_statement(void)
{
	assert(token.type == T_IDENTIFIER);
	symbol_t *symbol = token.v.symbol;
	next_token();

	declaration_t *label = get_label(symbol);

	/* if source position is already set then the label is defined twice,
	 * otherwise it was just mentioned in a goto so far */
	if(label->source_position.input_name != NULL) {
		errorf(HERE, "duplicate label '%Y'", symbol);
		errorf(label->source_position, "previous definition of '%Y' was here",
		       symbol);
	} else {
		label->source_position = token.source_position;
	}

	statement_t *statement = allocate_statement_zero(STATEMENT_LABEL);

	statement->base.source_position = token.source_position;
	statement->label.label          = label;

	eat(':');

	if(token.type == '}') {
		/* TODO only warn? */
		errorf(HERE, "label at end of compound statement");
		return statement;
	} else {
		if (token.type == ';') {
			/* eat an empty statement here, to avoid the warning about an empty
			 * after a label.  label:; is commonly used to have a label before
			 * a }. */
			next_token();
		} else {
			statement->label.statement = parse_statement();
		}
	}

	/* remember the labels's in a list for later checking */
	if (label_last == NULL) {
		label_first = &statement->label;
	} else {
		label_last->next = &statement->label;
	}
	label_last = &statement->label;

	return statement;
}

/**
 * Parse an if statement.
 */
static statement_t *parse_if(void)
{
	eat(T_if);

	statement_t *statement          = allocate_statement_zero(STATEMENT_IF);
	statement->base.source_position = token.source_position;

	expect('(');
	statement->ifs.condition = parse_expression();
	expect(')');

	statement->ifs.true_statement = parse_statement();
	if(token.type == T_else) {
		next_token();
		statement->ifs.false_statement = parse_statement();
	}

	return statement;
}

/**
 * Parse a switch statement.
 */
static statement_t *parse_switch(void)
{
	eat(T_switch);

	statement_t *statement          = allocate_statement_zero(STATEMENT_SWITCH);
	statement->base.source_position = token.source_position;

	expect('(');
	expression_t *const expr = parse_expression();
	type_t       *      type = skip_typeref(expr->base.type);
	if (is_type_integer(type)) {
		type = promote_integer(type);
	} else if (is_type_valid(type)) {
		errorf(expr->base.source_position,
		       "switch quantity is not an integer, but '%T'", type);
		type = type_error_type;
	}
	statement->switchs.expression = create_implicit_cast(expr, type);
	expect(')');

	switch_statement_t *rem = current_switch;
	current_switch          = &statement->switchs;
	statement->switchs.body = parse_statement();
	current_switch          = rem;

	if (warning.switch_default
			&& find_default_label(&statement->switchs) == NULL) {
		warningf(statement->base.source_position, "switch has no default case");
	}

	return statement;
}

static statement_t *parse_loop_body(statement_t *const loop)
{
	statement_t *const rem = current_loop;
	current_loop = loop;

	statement_t *const body = parse_statement();

	current_loop = rem;
	return body;
}

/**
 * Parse a while statement.
 */
static statement_t *parse_while(void)
{
	eat(T_while);

	statement_t *statement          = allocate_statement_zero(STATEMENT_WHILE);
	statement->base.source_position = token.source_position;

	expect('(');
	statement->whiles.condition = parse_expression();
	expect(')');

	statement->whiles.body = parse_loop_body(statement);

	return statement;
}

/**
 * Parse a do statement.
 */
static statement_t *parse_do(void)
{
	eat(T_do);

	statement_t *statement = allocate_statement_zero(STATEMENT_DO_WHILE);

	statement->base.source_position = token.source_position;

	statement->do_while.body = parse_loop_body(statement);

	expect(T_while);
	expect('(');
	statement->do_while.condition = parse_expression();
	expect(')');
	expect(';');

	return statement;
}

/**
 * Parse a for statement.
 */
static statement_t *parse_for(void)
{
	eat(T_for);

	statement_t *statement          = allocate_statement_zero(STATEMENT_FOR);
	statement->base.source_position = token.source_position;

	expect('(');

	int      top        = environment_top();
	scope_t *last_scope = scope;
	set_scope(&statement->fors.scope);

	if(token.type != ';') {
		if(is_declaration_specifier(&token, false)) {
			parse_declaration(record_declaration);
		} else {
			statement->fors.initialisation = parse_expression();
			expect(';');
		}
	} else {
		expect(';');
	}

	if(token.type != ';') {
		statement->fors.condition = parse_expression();
	}
	expect(';');
	if(token.type != ')') {
		statement->fors.step = parse_expression();
	}
	expect(')');
	statement->fors.body = parse_loop_body(statement);

	assert(scope == &statement->fors.scope);
	set_scope(last_scope);
	environment_pop_to(top);

	return statement;
}

/**
 * Parse a goto statement.
 */
static statement_t *parse_goto(void)
{
	eat(T_goto);

	if(token.type != T_IDENTIFIER) {
		parse_error_expected("while parsing goto", T_IDENTIFIER, 0);
		eat_statement();
		return NULL;
	}
	symbol_t *symbol = token.v.symbol;
	next_token();

	declaration_t *label = get_label(symbol);

	statement_t *statement          = allocate_statement_zero(STATEMENT_GOTO);
	statement->base.source_position = token.source_position;

	statement->gotos.label = label;

	/* remember the goto's in a list for later checking */
	if (goto_last == NULL) {
		goto_first = &statement->gotos;
	} else {
		goto_last->next = &statement->gotos;
	}
	goto_last = &statement->gotos;

	expect(';');

	return statement;
}

/**
 * Parse a continue statement.
 */
static statement_t *parse_continue(void)
{
	statement_t *statement;
	if (current_loop == NULL) {
		errorf(HERE, "continue statement not within loop");
		statement = NULL;
	} else {
		statement = allocate_statement_zero(STATEMENT_CONTINUE);

		statement->base.source_position = token.source_position;
	}

	eat(T_continue);
	expect(';');

	return statement;
}

/**
 * Parse a break statement.
 */
static statement_t *parse_break(void)
{
	statement_t *statement;
	if (current_switch == NULL && current_loop == NULL) {
		errorf(HERE, "break statement not within loop or switch");
		statement = NULL;
	} else {
		statement = allocate_statement_zero(STATEMENT_BREAK);

		statement->base.source_position = token.source_position;
	}

	eat(T_break);
	expect(';');

	return statement;
}

/**
 * Check if a given declaration represents a local variable.
 */
static bool is_local_var_declaration(const declaration_t *declaration) {
	switch ((storage_class_tag_t) declaration->storage_class) {
	case STORAGE_CLASS_NONE:
	case STORAGE_CLASS_AUTO:
	case STORAGE_CLASS_REGISTER: {
		const type_t *type = skip_typeref(declaration->type);
		if(is_type_function(type)) {
			return false;
		} else {
			return true;
		}
	}
	default:
		return false;
	}
}

/**
 * Check if a given declaration represents a variable.
 */
static bool is_var_declaration(const declaration_t *declaration) {
	switch ((storage_class_tag_t) declaration->storage_class) {
	case STORAGE_CLASS_NONE:
	case STORAGE_CLASS_EXTERN:
	case STORAGE_CLASS_STATIC:
	case STORAGE_CLASS_AUTO:
	case STORAGE_CLASS_REGISTER:
	case STORAGE_CLASS_THREAD:
	case STORAGE_CLASS_THREAD_EXTERN:
	case STORAGE_CLASS_THREAD_STATIC: {
		const type_t *type = skip_typeref(declaration->type);
		if(is_type_function(type)) {
			return false;
		} else {
			return true;
		}
	}
	default:
		return false;
	}
}

/**
 * Check if a given expression represents a local variable.
 */
static bool is_local_variable(const expression_t *expression)
{
	if (expression->base.kind != EXPR_REFERENCE) {
		return false;
	}
	const declaration_t *declaration = expression->reference.declaration;
	return is_local_var_declaration(declaration);
}

/**
 * Check if a given expression represents a local variable and
 * return its declaration then, else return NULL.
 */
declaration_t *expr_is_variable(const expression_t *expression)
{
	if (expression->base.kind != EXPR_REFERENCE) {
		return NULL;
	}
	declaration_t *declaration = expression->reference.declaration;
	if (is_var_declaration(declaration))
		return declaration;
	return NULL;
}

/**
 * Parse a return statement.
 */
static statement_t *parse_return(void)
{
	eat(T_return);

	statement_t *statement          = allocate_statement_zero(STATEMENT_RETURN);
	statement->base.source_position = token.source_position;

	expression_t *return_value = NULL;
	if(token.type != ';') {
		return_value = parse_expression();
	}
	expect(';');

	const type_t *const func_type = current_function->type;
	assert(is_type_function(func_type));
	type_t *const return_type = skip_typeref(func_type->function.return_type);

	if(return_value != NULL) {
		type_t *return_value_type = skip_typeref(return_value->base.type);

		if(is_type_atomic(return_type, ATOMIC_TYPE_VOID)
				&& !is_type_atomic(return_value_type, ATOMIC_TYPE_VOID)) {
			warningf(statement->base.source_position,
			         "'return' with a value, in function returning void");
			return_value = NULL;
		} else {
			type_t *const res_type = semantic_assign(return_type,
				return_value, "'return'");
			if (res_type == NULL) {
				errorf(statement->base.source_position,
				       "cannot return something of type '%T' in function returning '%T'",
				       return_value->base.type, return_type);
			} else {
				return_value = create_implicit_cast(return_value, res_type);
			}
		}
		/* check for returning address of a local var */
		if (return_value->base.kind == EXPR_UNARY_TAKE_ADDRESS) {
			const expression_t *expression = return_value->unary.value;
			if (is_local_variable(expression)) {
				warningf(statement->base.source_position,
				         "function returns address of local variable");
			}
		}
	} else {
		if(!is_type_atomic(return_type, ATOMIC_TYPE_VOID)) {
			warningf(statement->base.source_position,
			         "'return' without value, in function returning non-void");
		}
	}
	statement->returns.value = return_value;

	return statement;
}

/**
 * Parse a declaration statement.
 */
static statement_t *parse_declaration_statement(void)
{
	statement_t *statement = allocate_statement_zero(STATEMENT_DECLARATION);

	statement->base.source_position = token.source_position;

	declaration_t *before = last_declaration;
	parse_declaration(record_declaration);

	if(before == NULL) {
		statement->declaration.declarations_begin = scope->declarations;
	} else {
		statement->declaration.declarations_begin = before->next;
	}
	statement->declaration.declarations_end = last_declaration;

	return statement;
}

/**
 * Parse an expression statement, ie. expr ';'.
 */
static statement_t *parse_expression_statement(void)
{
	statement_t *statement = allocate_statement_zero(STATEMENT_EXPRESSION);

	statement->base.source_position  = token.source_position;
	expression_t *const expr         = parse_expression();
	statement->expression.expression = expr;

	if (warning.unused_value  && !expression_has_effect(expr)) {
		warningf(expr->base.source_position, "statement has no effect");
	}

	expect(';');

	return statement;
}

/**
 * Parse a statement.
 */
static statement_t *parse_statement(void)
{
	statement_t   *statement = NULL;

	/* declaration or statement */
	switch(token.type) {
	case T_asm:
		statement = parse_asm_statement();
		break;

	case T_case:
		statement = parse_case_statement();
		break;

	case T_default:
		statement = parse_default_statement();
		break;

	case '{':
		statement = parse_compound_statement();
		break;

	case T_if:
		statement = parse_if();
		break;

	case T_switch:
		statement = parse_switch();
		break;

	case T_while:
		statement = parse_while();
		break;

	case T_do:
		statement = parse_do();
		break;

	case T_for:
		statement = parse_for();
		break;

	case T_goto:
		statement = parse_goto();
		break;

	case T_continue:
		statement = parse_continue();
		break;

	case T_break:
		statement = parse_break();
		break;

	case T_return:
		statement = parse_return();
		break;

	case ';':
		if (warning.empty_statement) {
			warningf(HERE, "statement is empty");
		}
		next_token();
		statement = NULL;
		break;

	case T_IDENTIFIER:
		if(look_ahead(1)->type == ':') {
			statement = parse_label_statement();
			break;
		}

		if(is_typedef_symbol(token.v.symbol)) {
			statement = parse_declaration_statement();
			break;
		}

		statement = parse_expression_statement();
		break;

	case T___extension__:
		/* this can be a prefix to a declaration or an expression statement */
		/* we simply eat it now and parse the rest with tail recursion */
		do {
			next_token();
		} while(token.type == T___extension__);
		statement = parse_statement();
		break;

	DECLARATION_START
		statement = parse_declaration_statement();
		break;

	default:
		statement = parse_expression_statement();
		break;
	}

	assert(statement == NULL
			|| statement->base.source_position.input_name != NULL);

	return statement;
}

/**
 * Parse a compound statement.
 */
static statement_t *parse_compound_statement(void)
{
	statement_t *statement = allocate_statement_zero(STATEMENT_COMPOUND);

	statement->base.source_position = token.source_position;

	eat('{');

	int      top        = environment_top();
	scope_t *last_scope = scope;
	set_scope(&statement->compound.scope);

	statement_t *last_statement = NULL;

	while(token.type != '}' && token.type != T_EOF) {
		statement_t *sub_statement = parse_statement();
		if(sub_statement == NULL)
			continue;

		if(last_statement != NULL) {
			last_statement->base.next = sub_statement;
		} else {
			statement->compound.statements = sub_statement;
		}

		while(sub_statement->base.next != NULL)
			sub_statement = sub_statement->base.next;

		last_statement = sub_statement;
	}

	if(token.type == '}') {
		next_token();
	} else {
		errorf(statement->base.source_position,
		       "end of file while looking for closing '}'");
	}

	assert(scope == &statement->compound.scope);
	set_scope(last_scope);
	environment_pop_to(top);

	return statement;
}

/**
 * Initialize builtin types.
 */
static void initialize_builtin_types(void)
{
	type_intmax_t    = make_global_typedef("__intmax_t__",      type_long_long);
	type_size_t      = make_global_typedef("__SIZE_TYPE__",     type_unsigned_long);
	type_ssize_t     = make_global_typedef("__SSIZE_TYPE__",    type_long);
	type_ptrdiff_t   = make_global_typedef("__PTRDIFF_TYPE__",  type_long);
	type_uintmax_t   = make_global_typedef("__uintmax_t__",     type_unsigned_long_long);
	type_uptrdiff_t  = make_global_typedef("__UPTRDIFF_TYPE__", type_unsigned_long);
	type_wchar_t     = make_global_typedef("__WCHAR_TYPE__",    type_int);
	type_wint_t      = make_global_typedef("__WINT_TYPE__",     type_int);

	type_intmax_t_ptr  = make_pointer_type(type_intmax_t,  TYPE_QUALIFIER_NONE);
	type_ptrdiff_t_ptr = make_pointer_type(type_ptrdiff_t, TYPE_QUALIFIER_NONE);
	type_ssize_t_ptr   = make_pointer_type(type_ssize_t,   TYPE_QUALIFIER_NONE);
	type_wchar_t_ptr   = make_pointer_type(type_wchar_t,   TYPE_QUALIFIER_NONE);
}

/**
 * Check for unused global static functions and variables
 */
static void check_unused_globals(void)
{
	if (!warning.unused_function && !warning.unused_variable)
		return;

	for (const declaration_t *decl = global_scope->declarations; decl != NULL; decl = decl->next) {
		if (decl->used || decl->storage_class != STORAGE_CLASS_STATIC)
			continue;

		type_t *const type = decl->type;
		const char *s;
		if (is_type_function(skip_typeref(type))) {
			if (!warning.unused_function || decl->is_inline)
				continue;

			s = (decl->init.statement != NULL ? "defined" : "declared");
		} else {
			if (!warning.unused_variable)
				continue;

			s = "defined";
		}

		warningf(decl->source_position, "'%#T' %s but not used",
			type, decl->symbol, s);
	}
}

/**
 * Parse a translation unit.
 */
static translation_unit_t *parse_translation_unit(void)
{
	translation_unit_t *unit = allocate_ast_zero(sizeof(unit[0]));

	assert(global_scope == NULL);
	global_scope = &unit->scope;

	assert(scope == NULL);
	set_scope(&unit->scope);

	initialize_builtin_types();

	while(token.type != T_EOF) {
		if (token.type == ';') {
			/* TODO error in strict mode */
			warningf(HERE, "stray ';' outside of function");
			next_token();
		} else {
			parse_external_declaration();
		}
	}

	assert(scope == &unit->scope);
	scope          = NULL;
	last_declaration = NULL;

	assert(global_scope == &unit->scope);
	check_unused_globals();
	global_scope = NULL;

	return unit;
}

/**
 * Parse the input.
 *
 * @return  the translation unit or NULL if errors occurred.
 */
translation_unit_t *parse(void)
{
	environment_stack = NEW_ARR_F(stack_entry_t, 0);
	label_stack       = NEW_ARR_F(stack_entry_t, 0);
	diagnostic_count  = 0;
	error_count       = 0;
	warning_count     = 0;

	type_set_output(stderr);
	ast_set_output(stderr);

	lookahead_bufpos = 0;
	for(int i = 0; i < MAX_LOOKAHEAD + 2; ++i) {
		next_token();
	}
	translation_unit_t *unit = parse_translation_unit();

	DEL_ARR_F(environment_stack);
	DEL_ARR_F(label_stack);

	if(error_count > 0)
		return NULL;

	return unit;
}

/**
 * Initialize the parser.
 */
void init_parser(void)
{
	init_expression_parsers();
	obstack_init(&temp_obst);

	symbol_t *const va_list_sym = symbol_table_insert("__builtin_va_list");
	type_valist = create_builtin_type(va_list_sym, type_void_ptr);
}

/**
 * Terminate the parser.
 */
void exit_parser(void)
{
	obstack_free(&temp_obst, NULL);
}
