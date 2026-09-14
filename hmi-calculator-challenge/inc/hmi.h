#ifndef HMI_H
#define HMI_H

typedef enum {
    HMI_CONTINUE,
    HMI_EXIT
} HmiStatus;

// Inicializador
void hmi_init(void);

// Rotina principal
HmiStatus hmi_run(void);

#endif // HMI_H