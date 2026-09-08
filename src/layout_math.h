#pragma once

void split_span(int span, const float *facts, int n, int *out);

int center_axis(int origin, int span, int size);

int clamp_axis(int origin, int span, int pos, int size);
