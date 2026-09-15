#include <stdio.h>
#include "logger.h"

/*
 * Estratégia open -> write -> close a cada chamada:
 * - Não mantemos um FILE* global aberto. Se o processo morrer (crash, reset,
 *   queda de energia), um handle aberto pode perder o conteúdo ainda retido
 *   no buffer da libc. Fechando a cada registro, a janela de perda cai para
 *   no máximo a mensagem em curso.
 * - fclose() força o flush do buffer da stdio para o sistema operacional,
 *   então cada registro sai da RAM da aplicação antes da função retornar.
 * - O custo (syscalls extras por chamada) é aceitável: auditoria é evento
 *   raro, disparado por ação humana, não um caminho de tempo real.
 * Ressalva: fclose entrega ao SO, não ao disco. Garantia contra queda de
 * energia exige fsync()/_commit() antes do fclose, se o requisito surgir.
 */
void logger_append(const char *mensagem) {
    if (mensagem == NULL) {
        return;
    }

    FILE *arquivo = fopen(LOGGER_FILE_PATH, "a");
    if (arquivo == NULL) {
        /* Falha de log não derruba a aplicação: reporta e segue */
        perror("[ERRO] logger: falha ao abrir " LOGGER_FILE_PATH);
        return;
    }

    fprintf(arquivo, "%s\n", mensagem);

    /* fclose é onde o flush acontece: erro aqui significa registro perdido */
    if (fclose(arquivo) != 0) {
        perror("[ERRO] logger: falha ao gravar " LOGGER_FILE_PATH);
    }
}
