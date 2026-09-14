#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hmi.h"
#include "math_ops.h"

#define HMI_INPUT_BUFFER_SIZE 64

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
        buffer[len - 1] = '\0'; 
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
        long value = strtol(buffer, &endptr, 10);

        if (endptr == buffer || *endptr != '\0') {
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
        double value = strtod(buffer, &endptr);

        if (endptr == buffer || *endptr != '\0') {
            printf("[ERRO] Entrada invalida. Digite um numero (ex: 3.14).\n");
            continue;
        }

        *out = value;
        return 1;
    }
}

/* Preenche o buffer da matriz abstraindo a complexidade de I/O do usuário */
static void hmi_read_matrix(double *matrix, int n) {
    char prompt[48];
    for (int r = 0; r < n; r++) {
        for (int c = 0; c < n; c++) {
            snprintf(prompt, sizeof(prompt), "Elemento [%d][%d]: ", r, c);
            hmi_read_double(prompt, &matrix[r * n + c]);
        }
    }
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

void hmi_init(void) {
    printf("Calculadora HMI iniciada.\n");
}

void hmi_run(void) {
    /* 
     * Memória estática alocada no Data Segment. 
     * Evita estouro de Stack (VLA) e dispensa alocação dinâmica (Heap).
     */
    static double matriz[MATH_OPS_MAX_MATRIX_DIM * MATH_OPS_MAX_MATRIX_DIM];
    int opcao;

    hmi_print_menu();
    if (!hmi_read_int("Escolha uma opcao: ", &opcao)) {
        return; 
    }

    switch (opcao) {
        case 1:
        case 2:
        case 3:
        case 4: {
            double a, b;
            hmi_read_double("Valor A: ", &a);
            hmi_read_double("Valor B: ", &b);
            printf("Valores capturados: A=%.4f, B=%.4f\n", a, b);
            printf("[Aguardando Integracao HMI-Math via Dispatch Table]\n");
            break;
        }
        case 5: {
            int n;
            hmi_read_int("Dimensao da matriz (n): ", &n);

            if (n <= 0 || n > MATH_OPS_MAX_MATRIX_DIM) {
                printf("[ERRO] Dimensao fora do intervalo (1..%d).\n", MATH_OPS_MAX_MATRIX_DIM);
                break;
            }

            hmi_read_matrix(matriz, n);
            printf("\nMatriz capturada:\n");
            hmi_print_matrix(matriz, n);
            printf("[Aguardando Integracao HMI-Math via Dispatch Table]\n");
            break;
        }
        case 0:
            printf("Encerrando...\n");
            exit(0);
        default:
            printf("[ERRO] Opcao invalida.\n");
            break;
    }
}