//
// Created by awhaley on 13/07/2020.
//

#include "inference.h"
#include "ast/symbol_t.h"
#include "ast/type_t.h"
#include "ast/entity_t.h"
#include "ast/walk.h"



bool is_inferred_type(type_t * type)
{
    if (type->kind == TYPE_ATOMIC)
    {
        if (type->atomic.akind == ATOMIC_TYPE_INFER) return true;
    }
    return false;
}


int infer_in_variable(variable_t * var)
{
    if (is_inferred_type(var->base.type))
    {
        printf("Inferring type for: %s", var->base.base.symbol->string);
        if (var->initializer)
        {
            if (var->initializer->kind == INITIALIZER_VALUE)
            {
               if (var->initializer->value.value->kind == EXPR_UNARY_CAST)
               {
                   // Copy type
                   var->base.type = var->initializer->value.value->unary.value->base.type;
                   printf(" %d\n", var->initializer->value.value->unary.value->base.type->atomic.akind);
               }
            }

        }
        else
        {
            // Error no initialiser maybe
        }
    }

}

void infer_in_statements(statement_t * statement, void *env)
{
    switch (statement->kind)
    {
        case STATEMENT_DECLARATION:
            if (statement->declaration.declarations_begin->kind == ENTITY_VARIABLE) {
                return infer_in_variable(&statement->declaration.declarations_begin->variable);
            }
            break;

    }
}


int infer_types(translation_unit_t *unit, void *env)
{

    walk_translation_unit(unit,
                               NULL,
                               infer_in_statements,
                               NULL,
                               env);


   return 0;
}