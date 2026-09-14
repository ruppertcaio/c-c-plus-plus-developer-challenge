#include "hmi.h"

int main(void) {
    // Setup
    hmi_init();

    // Loop
    while (hmi_run() == HMI_CONTINUE) { 
    }

    return 0;
}