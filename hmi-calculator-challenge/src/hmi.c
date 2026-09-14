#include <stdio.h>
#include "hmi.h"

void hmi_init(void) {
    printf("Teste init\n");
}

void hmi_run(void) {
    // Menu interativo
    printf("Teste run\n");
    getchar(); // "Pausa" para ver a saída antes de encerrar
}