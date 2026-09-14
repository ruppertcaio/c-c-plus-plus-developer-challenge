#include "math_ops.h"

MathStatus math_add(double a, double b, double *out)
{
    if (out == NULL) {
        return MATH_ERR_NULL_PTR;
    }

    *out = a + b;

    return MATH_OK;
}


MathStatus math_sub(double a, double b, double *out)
{
    if (out == NULL) {
        return MATH_ERR_NULL_PTR;
    }

    *out = a - b;

    return MATH_OK;
}


MathStatus math_mul(double a, double b, double *out)
{
    if (out == NULL) {
        return MATH_ERR_NULL_PTR;
    }

    *out = a * b;

    return MATH_OK;
}


MathStatus math_div(double a, double b, double *out)
{
    if (out == NULL) {
        return MATH_ERR_NULL_PTR;
    }

    if (b == 0.0) {
        return MATH_ERR_DIV_BY_ZERO;
    }

    *out = a / b;

    return MATH_OK;
}