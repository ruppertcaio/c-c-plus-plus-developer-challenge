#include "math_ops.h"
#include <stddef.h>

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


/* Helper estático para dispensar a dependência da <math.h> (libm) */
static double abs_val(double value) {
    return (value < 0.0) ? -value : value;
}

MathStatus math_determinant(double *matrix, int n, double *out) {
    if (matrix == NULL || out == NULL) {
        return MATH_ERR_NULL_PTR;
    }
    if (n <= 0 || n > MATH_OPS_MAX_MATRIX_DIM) {
        return MATH_ERR_INVALID_DIMENSION;
    }

    const double EPSILON = 1e-12;

    if (n == 1) {
        *out = matrix[0];
        return MATH_OK;
    }

    double det_sinal = 1.0;

    /* Eliminação de Gauss, com pivotamento parcial. */
    for (int pivot = 0; pivot < n - 1; pivot++) {
        int max_row = pivot;
        double max_val = abs_val(matrix[pivot * n + pivot]);

        for (int r = pivot + 1; r < n; r++) {
            double val = abs_val(matrix[r * n + pivot]);
            if (val > max_val) {
                max_val = val;
                max_row = r;
            }
        }

        if (max_val < EPSILON) {
            *out = 0.0;
            return MATH_ERR_SINGULAR_MATRIX;
        }

        if (max_row != pivot) {
            for (int c = 0; c < n; c++) {
                double tmp = matrix[pivot * n + c];
                matrix[pivot * n + c] = matrix[max_row * n + c];
                matrix[max_row * n + c] = tmp;
            }
            det_sinal = -det_sinal;
        }

        for (int r = pivot + 1; r < n; r++) {
            double fator = matrix[r * n + pivot] / matrix[pivot * n + pivot];
            for (int c = pivot; c < n; c++) {
                matrix[r * n + c] -= fator * matrix[pivot * n + c];
            }
        }
    }

    if (abs_val(matrix[(n - 1) * n + (n - 1)]) < EPSILON) {
        *out = 0.0;
        return MATH_ERR_SINGULAR_MATRIX;
    }

    double det = det_sinal;
    for (int i = 0; i < n; i++) {
        det *= matrix[i * n + i];
    }

    *out = det;
    return MATH_OK;
}
