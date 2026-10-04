/* recipe.h — маршрутизатор: смотрит на блок и выбирает рецепт */
#ifndef RECIPE_H
#define RECIPE_H
#include "iscf.h"

typedef struct { float h, p, r; } Metrics;   /* энтропия, доля печатных, доля серии */

void recipe_metrics(const u8 *in, size_t n, Metrics *m);
int  recipe_is_text(const Metrics *m);
float recipe_delta_h(const u8 *in, size_t n, int step);  /* энтропия дельты с шагом */

#endif
