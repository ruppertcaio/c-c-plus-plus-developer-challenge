#ifndef MATH_OPS_H
#define MATH_OPS_H

/**
 * @brief Códigos de status das operações matemáticas.
 */
typedef enum {
    MATH_OK = 0,                /**< Operação concluída com sucesso. */
    MATH_ERR_NULL_PTR,          /**< Ponteiro de saída (out) é NULL. */
    MATH_ERR_DIV_BY_ZERO,       /**< Divisor igual a zero. */
    MATH_ERR_SINGULAR_MATRIX,   /*< Pivô nulo em toda a coluna: matriz singular (det = 0). */
    MATH_ERR_INVALID_DIMENSION  /*< n <= 0 ou n > MATH_OPS_MAX_MATRIX_DIM. */
} MathStatus;

/**
 * @brief Dimensão máxima de matriz suportada pelo módulo.
 */
#define MATH_OPS_MAX_MATRIX_DIM 10

MathStatus math_add(double a, double b, double *out);
MathStatus math_sub(double a, double b, double *out);
MathStatus math_mul(double a, double b, double *out);
MathStatus math_div(double a, double b, double *out);

/**
 * @brief Calcula o determinante de uma matriz n x n via Eliminação de Gauss.
 */
MathStatus math_determinant(double *matrix, int n, double *out);

#endif // MATH_OPS_H