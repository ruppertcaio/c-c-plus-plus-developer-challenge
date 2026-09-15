/* Garante que assert() nunca seja desativado, mesmo com -DNDEBUG. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

#include "math_ops.h"

#define EPS 1e-9

/* Compara doubles por tolerância (IEEE 754): proibido usar == em double. */
#define ASSERT_DOUBLE_EQ(esperado, obtido, epsilon)                         \
    do {                                                                    \
        double diff_ = (double)(esperado) - (double)(obtido);               \
        if (!(diff_ < (epsilon) && diff_ > -(epsilon))) {                   \
            printf("  FALHA: esperado %.12g, obtido %.12g\n",               \
                   (double)(esperado), (double)(obtido));                   \
        }                                                                   \
        assert(diff_ < (epsilon) && diff_ > -(epsilon));                    \
    } while (0)

static int tests_run = 0;

#define RUN_TEST(fn)                     \
    do {                                 \
        printf("[ RUN  ] %s\n", #fn);    \
        fn();                            \
        printf("[  OK  ] %s\n", #fn);    \
        tests_run++;                     \
    } while (0)

/* ---------- Operações escalares ---------- */

static void test_add(void)
{
    double r = 0.0;
    assert(math_add(2.5, 3.5, &r) == MATH_OK);
    ASSERT_DOUBLE_EQ(6.0, r, EPS);

    assert(math_add(-4.0, 1.5, &r) == MATH_OK);
    ASSERT_DOUBLE_EQ(-2.5, r, EPS);

    /* 0.1 + 0.2 != 0.3 bit a bit; só passa com tolerância. */
    assert(math_add(0.1, 0.2, &r) == MATH_OK);
    ASSERT_DOUBLE_EQ(0.3, r, EPS);
}

static void test_sub(void)
{
    double r = 0.0;
    assert(math_sub(10.0, 4.25, &r) == MATH_OK);
    ASSERT_DOUBLE_EQ(5.75, r, EPS);

    assert(math_sub(1.0, 3.0, &r) == MATH_OK);
    ASSERT_DOUBLE_EQ(-2.0, r, EPS);
}

static void test_mul(void)
{
    double r = 0.0;
    assert(math_mul(3.0, -2.5, &r) == MATH_OK);
    ASSERT_DOUBLE_EQ(-7.5, r, EPS);

    assert(math_mul(123.456, 0.0, &r) == MATH_OK);
    ASSERT_DOUBLE_EQ(0.0, r, EPS);
}

static void test_div(void)
{
    double r = 0.0;
    assert(math_div(7.0, 2.0, &r) == MATH_OK);
    ASSERT_DOUBLE_EQ(3.5, r, EPS);

    assert(math_div(1.0, 3.0, &r) == MATH_OK);
    ASSERT_DOUBLE_EQ(0.333333333333, r, 1e-9);
}

/* ---------- Casos de falha ---------- */

static void test_div_by_zero(void)
{
    double r = 42.0;
    assert(math_div(5.0, 0.0, &r) == MATH_ERR_DIV_BY_ZERO);
    assert(math_div(5.0, -0.0, &r) == MATH_ERR_DIV_BY_ZERO);
    /* Saída não pode ser alterada em caso de erro. */
    ASSERT_DOUBLE_EQ(42.0, r, EPS);
}

static void test_null_ptr(void)
{
    double m[1] = { 1.0 };

    assert(math_add(1.0, 2.0, NULL) == MATH_ERR_NULL_PTR);
    assert(math_sub(1.0, 2.0, NULL) == MATH_ERR_NULL_PTR);
    assert(math_mul(1.0, 2.0, NULL) == MATH_ERR_NULL_PTR);
    assert(math_div(1.0, 2.0, NULL) == MATH_ERR_NULL_PTR);
    /* NULL tem prioridade sobre divisão por zero. */
    assert(math_div(1.0, 0.0, NULL) == MATH_ERR_NULL_PTR);

    assert(math_determinant(NULL, 1, m) == MATH_ERR_NULL_PTR);
    assert(math_determinant(m, 1, NULL) == MATH_ERR_NULL_PTR);
}

/* ---------- Determinante ---------- */

static void test_det_1x1(void)
{
    double m[1] = { -7.25 };
    double det = 0.0;
    assert(math_determinant(m, 1, &det) == MATH_OK);
    ASSERT_DOUBLE_EQ(-7.25, det, EPS);
}

static void test_det_3x3(void)
{
    double m[9] = {
        2.0, -3.0,  1.0,
        2.0,  0.0, -1.0,
        1.0,  4.0,  5.0
    };
    double det = 0.0;
    assert(math_determinant(m, 3, &det) == MATH_OK);
    ASSERT_DOUBLE_EQ(49.0, det, EPS);
}

static void test_det_singular_zero_row(void)
{
    double m[9] = {
        1.0, 2.0, 3.0,
        0.0, 0.0, 0.0,
        4.0, 5.0, 6.0
    };
    double det = 99.0;
    assert(math_determinant(m, 3, &det) == MATH_ERR_SINGULAR_MATRIX);
    ASSERT_DOUBLE_EQ(0.0, det, EPS);
}

static void test_det_pivot_2x2(void)
{
    /* a[0][0] = 0: sem troca de linhas haveria divisão por zero. */
    double m[4] = {
        0.0, 1.0,
        1.0, 0.0
    };
    double det = 0.0;
    assert(math_determinant(m, 2, &det) == MATH_OK);
    ASSERT_DOUBLE_EQ(-1.0, det, EPS);
}

static void test_det_pivot_3x3(void)
{
    /* Pivô inicial nulo; troca de linha inverte o sinal. det = -3. */
    double m[9] = {
        0.0, 2.0, 1.0,
        1.0, 1.0, 1.0,
        2.0, 1.0, 3.0
    };
    double det = 0.0;
    assert(math_determinant(m, 3, &det) == MATH_OK);
    ASSERT_DOUBLE_EQ(-3.0, det, EPS);
}

static void test_det_invalid_dimension(void)
{
    double m[1] = { 1.0 };
    double det = 0.0;
    assert(math_determinant(m, 0, &det) == MATH_ERR_INVALID_DIMENSION);
    assert(math_determinant(m, -1, &det) == MATH_ERR_INVALID_DIMENSION);
    assert(math_determinant(m, MATH_OPS_MAX_MATRIX_DIM + 1, &det)
           == MATH_ERR_INVALID_DIMENSION);
}

int main(void)
{
    RUN_TEST(test_add);
    RUN_TEST(test_sub);
    RUN_TEST(test_mul);
    RUN_TEST(test_div);
    RUN_TEST(test_div_by_zero);
    RUN_TEST(test_null_ptr);
    RUN_TEST(test_det_1x1);
    RUN_TEST(test_det_3x3);
    RUN_TEST(test_det_singular_zero_row);
    RUN_TEST(test_det_pivot_2x2);
    RUN_TEST(test_det_pivot_3x3);
    RUN_TEST(test_det_invalid_dimension);

    printf("\n%d testes executados, todos passaram.\n", tests_run);
    return 0;
}
