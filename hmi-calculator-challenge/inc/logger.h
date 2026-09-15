#ifndef LOGGER_H
#define LOGGER_H

/**
 * @brief Arquivo de auditoria persistente (simula memória não volátil).
 */
#define LOGGER_FILE_PATH "history.txt"

/**
 * @brief Anexa uma linha de auditoria ao arquivo de histórico.
 *
 * @param mensagem String terminada em '\0'. Uma quebra de linha é adicionada ao final.
 */
void logger_append(const char *mensagem);

#endif // LOGGER_H
