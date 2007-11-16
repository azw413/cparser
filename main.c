#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

#ifndef WITH_LIBCORE
#define WITH_LIBCORE
#endif

#include <libfirm/firm.h>
#include <libfirm/be.h>

#include "lexer.h"
#include "token_t.h"
#include "type_hash.h"
#include "parser.h"
#include "ast2firm.h"
#include "adt/error.h"

#define PREPROCESSOR "gcc -E"
#define LINKER       "gcc"

static int verbose;

static const ir_settings_if_conv_t *if_conv_info = NULL;

static void initialize_firm(void)
{
	be_opt_register();
	firm_init_options(NULL, 0, NULL);

	const backend_params *be_params;
	firm_parameter_t params;
	memset(&params, 0, sizeof(params));

	params.size = sizeof(params);
	params.enable_statistics = 0;
	params.initialize_local_func = uninitialized_local_var;
	params.cc_mask = 0;
	params.builtin_dbg = NULL;

	/* initialize backend */
	be_params = be_init();
	be_set_debug_retrieve(retrieve_dbg);
	params.arch_op_settings = be_params->arch_op_settings;
	if_conv_info            = be_params->if_conv_info;

	/* intialize firm itself */
	init_firm(&params);
	dbg_init(NULL, NULL, dbg_snprint);

	set_opt_constant_folding(1);
	set_opt_unreachable_code(1);
	set_opt_control_flow_straightening(1);
	set_opt_control_flow_weak_simplification(1);
	set_opt_control_flow_strong_simplification(1);
	set_opt_dead_node_elimination(1);
	set_opt_reassociation(1);
	set_opt_inline(1);
	set_opt_dyn_meth_dispatch(1);
	set_opt_normalize(1);
	set_opt_tail_recursion(1);
	set_opt_dead_method_elimination(1);
	set_opt_precise_exc_context(0);
	set_opt_loop_unrolling(0);
	set_opt_strength_red(0);
	set_opt_redundant_loadstore(1);
	set_opt_fragile_ops(0);
	set_opt_function_call(1);
	set_opt_optimize_class_casts(0);
	set_opt_suppress_downcast_optimization(0);
	set_opt_remove_confirm(1);
	set_opt_scalar_replacement(1);
	set_opt_ldst_only_null_ptr_exceptions(1);
	set_opt_alias_analysis(1);

	dump_consts_local(1);
	dump_keepalive_edges(1);
}

static void dump(ir_graph *irg, const char *suffix)
{
	dump_ir_block_graph(irg, suffix);
}

static void get_output_name(char *buf, size_t buflen, const char *inputname,
                            const char *newext)
{
	size_t last_dot = 0xffffffff;
	size_t i = 0;
	for(const char *c = inputname; *c != 0; ++c) {
		if(*c == '.')
			last_dot = i;
		++i;
	}
	if(last_dot == 0xffffffff)
		last_dot = i;

	if(last_dot >= buflen)
		panic("filename too long");
	memcpy(buf, inputname, last_dot);

	size_t extlen = strlen(newext) + 1;
	if(extlen + last_dot >= buflen)
		panic("filename too long");
	memcpy(buf+last_dot, newext, extlen);
}

static translation_unit_t *do_parsing(const char *fname, const char *input)
{
	FILE *in = fopen(fname, "r");
	if(in == NULL) {
		fprintf(stderr, "Couldn't open '%s': %s\n", fname, strerror(errno));
		exit(1);
	}

	lexer_open_stream(in, input);

	translation_unit_t *unit = parse();

	fclose(in);

	return unit;
}

static void lextest(const char *fname)
{
	FILE *in = fopen(fname, "r");
	if(in == NULL) {
		fprintf(stderr, "Couldn't open '%s': %s\n", fname, strerror(errno));
		exit(1);
	}

	lexer_open_stream(in, fname);

	do {
		lexer_next_preprocessing_token();
		print_token(stdout, &lexer_token);
		puts("");
	} while(lexer_token.type != T_EOF);

	fclose(in);
}

static void backend(const char *inputname, const char *outname)
{
	FILE *out = fopen(outname, "w");
	if(out == NULL) {
		fprintf(stderr, "couldn't open '%s' for writing: %s\n", outname,
				strerror(errno));
		exit(1);
	}

	be_main(out, inputname);

	fclose(out);
}

static void emit(const char *input_name, const char *out_name)
{
	backend(input_name, out_name);
}

static void preprocess(const char *in, const char *out)
{
	char buf[4096];

	snprintf(buf, sizeof(buf), "%s %s -o %s", PREPROCESSOR, in, out);
	if(verbose) {
		puts(buf);
	}
	int err = system(buf);
	if(err != 0) {
		fprintf(stderr, "preprocessor reported an error\n");
		exit(1);
	}
}

static void link(const char *in, const char *out)
{
	char buf[4096];

	snprintf(buf, sizeof(buf), "%s %s -o %s", LINKER, in, out);
	if(verbose) {
		puts(buf);
	}
	int err = system(buf);
	if(err != 0) {
		fprintf(stderr, "linker reported an error\n");
		exit(1);
	}
}

static void assemble(const char *in, const char *out)
{
	char buf[4096];

	snprintf(buf, sizeof(buf), "%s %s -c -o %s", LINKER, in, out);
	if(verbose) {
		puts(buf);
	}
	int err = system(buf);
	if(err != 0) {
		fprintf(stderr, "assembler reported an error\n");
		exit(1);
	}
}

static void create_firm_prog(translation_unit_t *unit)
{
	translation_unit_to_firm(unit);

	//dump_globals_as_text(dump_verbosity_max, "-globals");

	int n_irgs = get_irp_n_irgs();
	for(int i = 0; i < n_irgs; ++i) {
		ir_graph *const irg = get_irp_irg(i);
		dump(irg, "-start");
	}
}

static void optimize(void)
{
	int         arr_len;
	ir_entity **keep_methods;

	cgana(&arr_len, &keep_methods);
	gc_irgs(arr_len, keep_methods);
	free(keep_methods);

	optimize_funccalls(0);

	for(int i = 0; i < get_irp_n_irgs(); ++i) {
		ir_graph *irg = get_irp_irg(i);
		place_code(irg);
		dump(irg, "-place");
		optimize_graph_df(irg);
		dump(irg, "-localopt");
		optimize_cf(irg);
		dump(irg, "-cf");
	}
}

void write_fluffy_decls(translation_unit_t *unit);

typedef enum compile_mode_t {
	Compile,
	CompileAssemble,
	CompileAssembleLink,
	LexTest,
	PrintAst,
	PrintFluffy
} compile_mode_t;

static void usage(const char *argv0)
{
	fprintf(stderr, "Usage %s input [-o output] [-c]\n", argv0);
}

int main(int argc, char **argv)
{
	initialize_firm();

	init_symbol_table();
	init_tokens();
	init_lexer();
	init_types();
	init_typehash();
	init_ast();
	init_parser();
	init_ast2firm();

	const char *input   = NULL;
	const char *outname = NULL;
	compile_mode_t mode = CompileAssembleLink;

	for(int i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		if(strcmp(arg, "-o") == 0) {
			++i;
			if(i >= argc) {
				usage(argv[0]);
				return 1;
			}
			outname = argv[i];
		} else if(strcmp(arg, "-c") == 0) {
			mode = CompileAssemble;
		} else if(strcmp(arg, "-S") == 0) {
			mode = Compile;
		} else if(strcmp(arg, "--lextest") == 0) {
			mode = LexTest;
		} else if(strcmp(arg, "--print-ast") == 0) {
			mode = PrintAst;
		} else if(strcmp(arg, "--print-fluffy") == 0) {
			mode = PrintFluffy;
		} else if(strcmp(arg, "-v") == 0) {
			verbose = 1;
		} else if(arg[0] == '-') {
			usage(argv[0]);
			return 1;
		} else {
			if(input != NULL) {
				fprintf(stderr, "Error: multiple input files specified\n");
				usage(argv[0]);
				return 1;
			} else {
				input = arg;
			}
		}
	}

	if(input == NULL) {
		fprintf(stderr, "%s: no input files\n", argv[0]);
		return 1;
	}

	if(mode == LexTest) {
		lextest(input);
		return 0;
	}

	const char *tmpfile = tmpnam(NULL);
	preprocess(input, tmpfile);

	translation_unit_t *unit = do_parsing(tmpfile, input);
	if(unit == NULL)
		return 1;

	if(mode == PrintAst) {
		print_ast(unit);
		return 0;
	}
	if(mode == PrintFluffy) {
		ast_set_output(stdout);
		write_fluffy_decls(unit);
	}

	char outsname[4096];
	const char *sname = NULL;
	if(mode == Compile) {
		sname = outname;
	}
	if(sname == NULL) {
		get_output_name(outsname, sizeof(outsname), input, ".s");
		sname = outsname;
	}

	create_firm_prog(unit);
	optimize();
	emit(input, sname);

	if(mode == CompileAssemble) {
		char outoname[4096];
		const char *oname = outname;
		if(oname == NULL) {
			get_output_name(outoname, sizeof(outoname), input, ".o");
			oname = outoname;
		}
		assemble(sname, oname);
	} else {
		assert(mode == CompileAssembleLink);

		if(outname == NULL)
			outname = "a.out";

		link(sname, outname);
	}

	exit_ast2firm();
	exit_parser();
	exit_ast();
	exit_typehash();
	exit_types();
	exit_lexer();
	exit_tokens();
	exit_symbol_table();
	return 0;
}
