/* A DEVICE_CMD_REQUEST for a screen button the Hub does not know falls back to reading a device id
 * and a command from the frame. The command was copied with 16 bytes from payload[12], 3 bytes
 * past the 25-byte payload. Runs the real hub_rf frame handler (main renamed) on a frame without a
 * terminator in its payload; built with AddressSanitizer by run_mock_suite.sh, so a read past the
 * frame aborts the test. */
#undef main
#include <stdio.h>
#include <string.h>
#include "rf_state.h"
#include "rf_spi.h"

int main(void) {
    rf_frame_t f;
    memset(&f, 0, sizeof(f));
    f.report_id = 0x20;
    f.dev_slot = 1;
    f.sub_id = 0x12;
    f.hdr_flag = 0x41;
    f.seq = 1;
    f.msg_type = RF_MSG_DEVICE_CMD_REQUEST;
    /* No screen button map exists, and the payload has no NUL anywhere */
    memset(f.payload, 'A', sizeof(f.payload));
    memcpy(f.payload, "dev1234567", 10);

    handle_rf_frame(&f);

    printf("[+] test_rf_device_cmd_frame: unknown screen button handled within the frame\n");
    return 0;
}
