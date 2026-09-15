# Dynamox HMI Calculator — Solução

Calculadora com interface homem-máquina (HMI) em terminal, escrita em C99, estruturada
como firmware embarcado: sem alocação dinâmica, sem dependências externas e com
separação estrita entre núcleo de cálculo, interface e persistência.

> O enunciado original do desafio está preservado em [`CHALLENGE.md`](./CHALLENGE.md).

---

## 1. Como compilar e executar

### Pré-requisitos

| Item | Versão mínima | Observação |
|---|---|---|
| Compilador C | GCC 4.9+ / Clang 3.5+ | Qualquer compilador C99. Não requer `-lm` |
| GNU Make | 3.81+ | No Windows: MinGW-w64, MSYS2 ou WSL |

Não há bibliotecas de terceiros. Apenas a biblioteca padrão C.

### Compilação e execução

```bash
cd hmi-calculator-challenge
make            # gera dynamox_app.exe
./dynamox_app.exe
```

No Windows (cmd/PowerShell), o executável é chamado da mesma forma:

```
make
dynamox_app.exe
```

### Testes unitários

```bash
make test
```

Compila e executa a suíte de `src/math_ops.c` de forma isolada (sem linkar a HMI nem o
logger). Saída esperada:

```
[ RUN  ] test_add
[  OK  ] test_add
...
12 testes executados, todos passaram.
```

O binário de teste retorna `0` em sucesso e aborta via `assert()` na primeira falha,
o que o torna diretamente utilizável em pipeline de CI.

### Limpeza

```bash
make clean      # remove objetos, executável e binário de teste
```

O alvo detecta o sistema operacional (`del` no Windows, `rm -f` nos demais).

---

## 2. Exemplo de sessão

```
Calculadora HMI iniciada.

=== Calculadora HMI ===
1. Somar dois numeros
2. Subtrair dois numeros
3. Multiplicar dois numeros
4. Dividir dois numeros
5. Calcular determinante de matriz
0. Sair
Escolha uma opcao: 1
Valor A: 2.5
Valor B: 3.5

>>> RESULTADO: Soma(2.5000, 3.5000) = 6.0000 <<<

Escolha uma opcao: 5
Dimensao da matriz (n): 3
Elemento [0][0]: 2
Elemento [0][1]: -3
...
Matriz capturada:
    2.00    -3.00     1.00
    2.00     0.00    -1.00
    1.00     4.00     5.00

>>> RESULTADO: Determinante = 49.0000 <<<

Escolha uma opcao: 4
Valor A: 5
Valor B: 0
[ERRO] Operacao abortada: Divisao por zero.
```

Após a sessão acima, `history.txt` contém:

```
Soma(2.5000, 3.5000) = 6.0000
Determinante(3x3) = 49.0000
```

---

## 3. Arquitetura

Três camadas com dependência estritamente unidirecional. Nenhum módulo abaixo da HMI
conhece a existência do terminal.

```
                  main.c
                    │            setup + superloop
                    ▼
                 src/hmi.c       I/O, menu, parsing, dispatch
                 ╱        ╲
                ▼          ▼
    src/math_ops.c      src/logger.c
    (núcleo puro)       (persistência)
```

| Arquivo | Responsabilidade |
|---|---|
| `main.c` | `hmi_init()` seguido de superloop `while (hmi_run() == HMI_CONTINUE)` |
| `inc/math_ops.h` · `src/math_ops.c` | Aritmética escalar e determinante. **Sem nenhum I/O** |
| `inc/hmi.h` · `src/hmi.c` | Menu, captura e validação de entrada, dispatch, tradução de códigos de erro em mensagens |
| `inc/logger.h` · `src/logger.c` | Registro persistente em `history.txt` |
| `tests/test_math.c` | 12 testes unitários sobre `math_ops` |

### Fluxo de uma operação

1. `hmi_run()` imprime o menu e lê a opção com `hmi_read_int()`.
2. Opções 1–4 vão para `hmi_dispatch_scalar()`, que localiza a operação na
   **tabela de despacho** e invoca o ponteiro de função correspondente.
3. A opção 5 vai para `hmi_handle_determinant()`, tratada à parte por ter
   assinatura divergente (matriz + dimensão em vez de dois escalares).
4. O `MathStatus` retornado decide entre imprimir resultado e registrar em log, ou
   traduzir o erro para mensagem ao usuário.

### Tabela de despacho

```c
typedef MathStatus (*ScalarOpFunc)(double, double, double *);

static const OperacaoEscalar tabela_operacoes[] = {
    {1, "Soma",          math_add},
    {2, "Subtracao",     math_sub},
    {3, "Multiplicacao", math_mul},
    {4, "Divisao",       math_div},
};
```

A tabela é `const` — em um alvo embarcado real ela reside em Flash, não em RAM. O
executor `hmi_dispatch_scalar()` é genérico: desconhece qual operação está executando.

---

## 4. Decisões de projeto

**Contrato de erro por `enum` + saída por ponteiro.** Toda função de `math_ops` retorna
`MathStatus` e escreve o resultado em `*out`. Isso separa "deu certo?" de "qual o valor?",
propaga a falha sem variável global de erro e sem exceções, e deixa a política de
tratamento com o chamador — que é quem tem contexto para decidir. Em caso de erro,
`*out` **não é modificado** (comportamento coberto por teste).

**Zero alocação dinâmica.** O buffer da matriz é `static double[10*10]`, dimensionado em
tempo de compilação por `MATH_OPS_MAX_MATRIX_DIM`. Não há `malloc` nem VLA em nenhum
ponto do projeto. O consumo de memória é determinístico e conhecido no link — requisito
típico de firmware, onde fragmentação de heap e estouro de pilha são falhas de campo.

**Sem dependência de `<math.h>`.** O único uso seria valor absoluto, resolvido por um
helper `static` de duas linhas. Evita linkar `libm` e reduz o footprint do binário.

**Pivotamento parcial no cálculo do determinante.** Não é refinamento numérico opcional:
sem troca de linhas, uma matriz perfeitamente válida como `[[0,1],[1,0]]` causaria divisão
por zero no primeiro pivô. A troca inverte o sinal do determinante, contabilizado em
`det_sinal`. Complexidade `O(n³)`, contra `O(n!)` da expansão por cofatores — a diferença
entre viável e inviável já em `n = 10`.

**Logger com `open → write → close` a cada registro.** Não se mantém um `FILE*` global
aberto. Se o processo morrer, um handle aberto pode perder o conteúdo retido no buffer da
libc; fechando a cada registro, a janela de perda cai para no máximo a mensagem em curso.
O custo em syscalls é aceitável porque auditoria é evento disparado por ação humana, não
caminho de tempo real. Falha de log **não derruba a aplicação**: reporta e segue.

**Captura de entrada por `fgets` + `strtol`/`strtod`.** Nunca `scanf("%d")`. A validação
checa `endptr == buffer` (nada consumido), `*endptr != '\0'` (lixo após o número),
`errno == ERANGE` (overflow) e os limites de `int`. Linhas maiores que o buffer têm o
resto drenado de `stdin`, o que elimina o clássico laço infinito realimentado por sujeira
no stream. Terminadores CRLF são tratados, então a aplicação aceita entrada via pipe ou
arquivo vindo de qualquer plataforma.

---

## 5. Tratamento de erros

| Código | Condição | Resposta da HMI |
|---|---|---|
| `MATH_OK` | Sucesso | Imprime resultado e registra em `history.txt` |
| `MATH_ERR_NULL_PTR` | Ponteiro de saída nulo | Erro interno reportado |
| `MATH_ERR_DIV_BY_ZERO` | Divisor igual a zero (inclui `-0.0`) | Operação abortada, volta ao menu |
| `MATH_ERR_SINGULAR_MATRIX` | Pivô nulo em coluna inteira | Aviso de matriz singular, determinante nulo |
| `MATH_ERR_INVALID_DIMENSION` | `n <= 0` ou `n > 10` | Dimensão rejeitada antes da leitura da matriz |

Entradas não numéricas nunca chegam à camada de cálculo: são rejeitadas no parser, que
repete o prompt.

---

## 6. Cobertura dos requisitos

### Requisitos funcionais

| Requisito | Estado |
|---|---|
| HMI para escolher e executar operações | Atendido — menu em terminal |
| Entrada de valores únicos | Atendido — operações 1 a 4 |
| Entrada de array de valores | Parcial — apenas via matriz do determinante |
| Ao menos duas operações | Atendido — cinco operações |
| Seleção do tipo de operação | Atendido |
| Cálculo do determinante | Atendido — Gauss com pivotamento parcial |

### Requisitos técnicos

| Requisito | Estado |
|---|---|
| Uso de C | Atendido — C99, sem extensões |
| Arquivo `main.c` | Atendido, na raiz do projeto |
| Organização em bibliotecas | Atendido — três módulos com header público em `inc/` |

### Bônus

| Requisito | Estado |
|---|---|
| Criar novos tipos de operação | Parcial — tabela de despacho cobre operações escalares |
| Log persistente | Atendido — `history.txt` |
| Determinante com dimensão escolhida pelo usuário | Atendido — `n` de 1 a 10 |
| Detecção e tratamento de erros de entrada | Atendido — parser blindado e enum de status |

---

## 7. Testes

`tests/test_math.c` cobre `math_ops` de forma isolada. São 12 casos agrupados em:

- **Aritmética escalar** — incluindo `0.1 + 0.2`, que só passa sob comparação por
  tolerância, documentando explicitamente que nunca se compara `double` com `==`.
- **Contrato de falha** — divisão por zero (inclusive `-0.0`), ponteiro nulo em todas as
  funções, precedência de `NULL` sobre divisão por zero, e verificação de que a saída
  permanece intacta quando há erro.
- **Determinante** — caso 1×1, caso 3×3 de referência, matriz singular por linha nula,
  e dois casos de **pivô inicial nulo** (2×2 e 3×3) que falhariam sem pivotamento parcial.
- **Validação de dimensão** — `n = 0`, `n` negativo e `n` acima do máximo.

A suíte usa `#undef NDEBUG`, garantindo que os `assert()` permaneçam ativos mesmo se o
build definir `NDEBUG`.

---

## 8. Limitações conhecidas

Documentadas deliberadamente, em ordem de relevância técnica:

1. **`math_determinant()` destrói a matriz do chamador.** A triangularização é feita
   in-place; o buffer retorna alterado. Hoje a HMI contorna imprimindo a matriz antes da
   chamada. O parâmetro deveria explicitar o contrato `[in,out]` ou receber um buffer de
   trabalho separado.
2. **Critério de singularidade é absoluto.** O limiar `1e-12` não é escalado pela magnitude
   da matriz. Uma matriz como `diag(1e-13, 1e-13)`, não singular, é classificada como
   singular. O critério correto é relativo: `DBL_EPSILON * n * max|a_ij|`.
3. **`det = 0` é tratado como erro.** `MATH_ERR_SINGULAR_MATRIX` classifica como falha um
   resultado matematicamente legítimo. Efeito colateral: determinantes nulos não entram no
   `history.txt`.
4. **O menu não deriva da tabela de despacho.** As opções são `printf` literais e o
   `switch` de `hmi_run()` é explícito, então adicionar uma operação exige edição em três
   pontos — o que limita o bônus de extensibilidade.
5. **O log não tem carimbo de tempo nem registra falhas.** Registra apenas operações
   bem-sucedidas, sem referência temporal, e não possui limite de tamanho ou rotação.
6. **Sem cancelamento na captura de entrada.** Os parsers repetem indefinidamente até
   receberem valor válido ou `EOF`; não há tecla de retorno ao menu no meio da leitura de
   uma matriz.
7. **`hmi_read_int()` e `hmi_read_double()` não são `static`.** São usadas apenas dentro
   de `hmi.c` e deveriam ter ligação interna.

---

## 9. Implementações futuras

**Curto prazo — fecha lacunas dos requisitos**

- Generalizar a tabela de despacho para comportar aridades distintas
  (`{id, nome, tipo, união de handlers}`), trazendo o determinante para dentro dela e
  gerando o menu por iteração sobre a tabela. Adicionar uma operação passa a ser uma
  linha em um array.
- Incluir operação vetorial sobre array de valores. A escolha natural é **RMS**, de
  mesmo custo computacional que a média e diretamente pertinente a análise de vibração.
- Carimbo de tempo ISO-8601 nos registros e log das operações que falharam — uma
  auditoria sem tempo e sem falhas registra apenas metade dos eventos relevantes.

**Médio prazo — robustez numérica e de contrato**

- Critério de singularidade relativo à norma da matriz.
- Reclassificação de `det = 0` como resultado, não como erro.
- Contrato explícito de buffer de trabalho em `math_determinant()`.
- Para `n` grande, acumular `log|pivô|` e sinal em vez do produto direto dos pivôs,
  evitando overflow e underflow.

**Longo prazo — qualidade de processo**

- Injeção do stream de entrada (`FILE *`) nos parsers, tornando a camada HMI testável
  com `fmemopen` e permitindo automatizar os casos de erro de entrada.
- Pipeline de CI executando `make test` a cada push.
- Abstração da camada de persistência em interface, para substituir `history.txt` por
  memória não volátil (EEPROM, flash externa) sem tocar na HMI.
- `fsync()`/`_commit()` antes do `fclose()` caso o requisito passe a exigir garantia
  contra queda de energia — `fclose` entrega ao sistema operacional, não à mídia.

---

## 10. Estrutura do projeto

```
hmi-calculator-challenge/
├── Makefile
├── README.md
├── CHALLENGE.md          enunciado original
├── main.c
├── inc/
│   ├── hmi.h
│   ├── logger.h
│   └── math_ops.h
├── src/
│   ├── hmi.c
│   ├── logger.c
│   └── math_ops.c
└── tests/
    └── test_math.c
```

Artefatos de build (`*.o`, `*.exe`) e o arquivo `history.txt` gerado em execução não são
versionados.
