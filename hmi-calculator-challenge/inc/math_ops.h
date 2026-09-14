#ifndef MATH_OPS_H
#define MATH_OPS_H

/**
 * @brief Códigos de status das operações matemáticas.
 */
typedef enum {
    MATH_OK = 0,           /**< Operação concluída com sucesso. */
    MATH_ERR_NULL_PTR,     /**< Ponteiro de saída (out) é NULL. */
    MATH_ERR_DIV_BY_ZERO   /**< Divisor igual a zero. */
} MathStatus;

/*
 * Operações escalares cegas: sem I/O, sem estado global,
 * resultado sempre por referência em 'out'.
 */
MathStatus math_add(double a, double b, double *out);
MathStatus math_sub(double a, double b, double *out);
MathStatus math_mul(double a, double b, double *out);
MathStatus math_div(double a, double b, double *out);

#endif // MATH_OPS_H