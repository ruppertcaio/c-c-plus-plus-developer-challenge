#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "hmi.h"
#include "math_ops.h"

#define HMI_INPUT_BUFFER_SIZE 64
#define NUM_OPERACOES (sizeof(tabela_operacoes) / sizeof(tabela_operacoes[0]))

typedef MathStatus (*ScalarOpFunc)(double, double, double *);

typedef struct {
    int id;
    const char *nome;
    ScalarOpFunc func;
} OperacaoEscalar;

static const OperacaoEscalar tabela_operacoes[] = {
    {1, "Soma",          math_add},
    {2, "Subtracao",     math_sub},
    {3, "Multiplicacao", math_mul},
    {4, "Divisao",       math_div},
};

/* Drena o stream de entrada para evitar falhas de segmentação ou loops de sujeira */
static void hmi_flush_stdin_line(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {
        /* descarta */
    }
}

/* Captura uma linha completa, garantindo o esvaziamento do buffer do SO */
static int hmi_read_line(char *buffer, size_t size) {
    if (fgets(buffer, (int)size, stdin) == NULL) {
        return 0;
    }
    
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[--len] = '\0';
        /* Descarta o CR de entradas com terminador CRLF (arquivos/pipes Windows) */
        if (len > 0 && buffer[len - 1] == '\r') {
            buffer[len - 1] = '\0';
        }
    } else if (len == size - 1) {
        hmi_flush_stdin_line();
    }
    return 1;
}

/* Parser blindado para inteiros */
int hmi_read_int(const char *prompt, int *out) {
    char buffer[HMI_INPUT_BUFFER_SIZE];
    for (;;) {
        printf("%s", prompt);
        if (!hmi_read_line(buffer, sizeof(buffer))) {
            return 0;
        }

        char *endptr;
        errno = 0;
        long value = strtol(buffer, &endptr, 10);

        if (endptr == buffer || *endptr != '\0' || errno == ERANGE
            || value < INT_MIN || value > INT_MAX) {
            printf("[ERRO] Entrada invalida. Digite um numero inteiro.\n");
            continue;
        }

        *out = (int)value;
        return 1;
    }
}

/* Parser blindado para ponto flutuante */
int hmi_read_double(const char *prompt, double *out) {
    char buffer[HMI_INPUT_BUFFER_SIZE];
    for (;;) {
        printf("%s", prompt);
        if (!hmi_read_line(buffer, sizeof(buffer))) {
            return 0;
        }

        char *endptr;
        errno = 0;
        double value = strtod(buffer, &endptr);

        if (endptr == buffer || *endptr != '\0' || errno == ERANGE) {
            printf("[ERRO] Entrada invalida. Digite um numero (ex: 3.14).\n");
            continue;
        }

        *out = value;
        return 1;
    }
}

/* Preenche o buffer da matriz abstraindo a complexidade de I/O do usuário */
static int hmi_read_matrix(double *matrix, int n) {
    char prompt[48];
    for (int r = 0; r < n; r++) {
        for (int c = 0; c < n; c++) {
            snprintf(prompt, sizeof(prompt), "Elemento [%d][%d]: ", r, c);
            if (!hmi_read_double(prompt, &matrix[r * n + c])) {
                return 0;
            }
        }
    }
    return 1;
}

static void hmi_print_matrix(const double *matrix, int n) {
    for (int r = 0; r < n; r++) {
        for (int c = 0; c < n; c++) {
            printf("%8.2f ", matrix[r * n + c]);
        }
        printf("\n");
    }
}

static void hmi_print_menu(void) {
    printf("\n=== Calculadora HMI ===\n");
    printf("1. Somar dois numeros\n");
    printf("2. Subtrair dois numeros\n");
    printf("3. Multiplicar dois numeros\n");
    printf("4. Dividir dois numeros\n");
    printf("5. Calcular determinante de matriz\n");
    printf("0. Sair\n");
}

/* Tradução dos códigos de erro de hardware/matemática para a interface */
static void hmi_print_math_status(MathStatus status) {
    switch (status) {
        case MATH_OK:
            break;
        case MATH_ERR_NULL_PTR:
            printf("[ERRO] Falha interna: ponteiro nulo na chamada.\n");
            break;
        case MATH_ERR_DIV_BY_ZERO:
            printf("[ERRO] Operacao abortada: Divisao por zero.\n");
            break;
        case MATH_ERR_SINGULAR_MATRIX:
            printf("[AVISO] Matriz singular detectada (Determinante nulo).\n");
            break;
        case MATH_ERR_INVALID_DIMENSION:
            printf("[ERRO] Dimensao de matriz invalida.\n");
            break;
        default:
            printf("[ERRO] Falha de processamento desconhecida.\n");
            break;
    }
}

/* Executor genérico via Dispatch Table (Complexidade O(1) de acoplamento) */
static void hmi_dispatch_scalar(int opcao) {
    const OperacaoEscalar *op = NULL;
    for (size_t i = 0; i < NUM_OPERACOES; i++) {
        if (tabela_operacoes[i].id == opcao) {
            op = &tabela_operacoes[i];
            break;
        }
    }

    if (op == NULL) {
        printf("[ERRO] Operacao nao registrada na tabela.\n");
        return;
    }

    double a, b, resultado;
    if (!hmi_read_double("Valor A: ", &a) || !hmi_read_double("Valor B: ", &b)) {
        return;
    }

    MathStatus status = op->func(a, b, &resultado);
    
    if (status == MATH_OK) {
        printf("\n>>> RESULTADO: %s(%.4f, %.4f) = %.4f <<<\n", op->nome, a, b, resultado);
    } else {
        hmi_print_math_status(status);
    }
}

/* Tratamento isolado para o Determinante (Assinatura divergente da tabela) */
static void hmi_handle_determinant(void) {
    static double matriz[MATH_OPS_MAX_MATRIX_DIM * MATH_OPS_MAX_MATRIX_DIM];
    int n;

    if (!hmi_read_int("Dimensao da matriz (n): ", &n)) {
        return;
    }

    if (n <= 0 || n > MATH_OPS_MAX_MATRIX_DIM) {
        printf("[ERRO] Dimensao fora do intervalo permitido (1..%d).\n", MATH_OPS_MAX_MATRIX_DIM);
        return;
    }

    if (!hmi_read_matrix(matriz, n)) {
        return;
    }

    printf("\nMatriz capturada:\n");
    hmi_print_matrix(matriz, n);
    
    double det;
    MathStatus status = math_determinant(matriz, n, &det);
    
    if (status == MATH_OK || status == MATH_ERR_SINGULAR_MATRIX) {
        printf("\n>>> RESULTADO: Determinante = %.4f <<<\n", det);
    }
    if (status != MATH_OK) {
        hmi_print_math_status(status);
    }
}

void hmi_init(void) {
    printf("Calculadora HMI iniciada.\n");
}

HmiStatus hmi_run(void) {
    int opcao;
    hmi_print_menu();
    if (!hmi_read_int("Escolha uma opcao: ", &opcao)) {
        return HMI_EXIT;
    }

    switch (opcao) {
        case 1:
        case 2:
        case 3:
        case 4:
            hmi_dispatch_scalar(opcao);
            break;
        case 5:
            hmi_handle_determinant();
            break;
        case 0:
            printf("Encerrando firmware...\n");
            return HMI_EXIT;
        default:
            printf("[ERRO] Opcao invalida.\n");
            break;
    }
    return HMI_CONTINUE;
}