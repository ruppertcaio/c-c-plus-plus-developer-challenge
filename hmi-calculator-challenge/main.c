#include "hmi.h"

int main(void) {
    // Setup
    hmi_init();

    // Loop
    while (1) {
        hmi_run(); 
    }

    return 0;
}