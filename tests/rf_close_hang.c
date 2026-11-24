/* Linked into the hub_rf host build of tests/test_rf_restart_state.py with
 * -Wl,--wrap=rf_spi_close: with HUB_RF_TEST_HANG_CLOSE set, closing the radio never returns, as
 * a close that waits on a driver whose read() is stuck would. */
#include <stdlib.h>
#include <unistd.h>

void __real_rf_spi_close(void);

void __wrap_rf_spi_close(void) {
    if (getenv("HUB_RF_TEST_HANG_CLOSE"))
        for (;;) sleep(60);
    __real_rf_spi_close();
}
