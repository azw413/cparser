//
// Created by awhaley on 13/07/2020.
//

#include <ast/ast_t.h>

#ifndef CPARSER_INFERENCE_H
#define CPARSER_INFERENCE_H

int infer_types(translation_unit_t *unit, void *env);

#endif //CPARSER_INFERENCE_H
