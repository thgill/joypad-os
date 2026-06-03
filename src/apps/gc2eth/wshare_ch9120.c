#include "wshare_ch9120.h"

UCHAR CH9120_LOCAL_IP[4] = {192, 168, 1, 250};    // LOCAL IP (was .200)
UCHAR CH9120_GATEWAY[4] = {192, 168, 1, 1};      // GATEWAY
UCHAR CH9120_SUBNET_MASK[4] = {255, 255, 255, 0}; // SUBNET MASK
UCHAR CH9120_TARGET_IP[4] = {192, 168, 1, 159};   // TARGET_IP = Mac
UWORD CH9120_PORT1 = 54970;                       // LOCAL PORT1 = 0xD6BA
UWORD CH9120_TARGET_PORT = 54970;                 // TARGET PORT = probe
UDOUBLE CH9120_BAUD_RATE = 921600;                // BAUD RATE

UCHAR tx[8] = {0x57, 0xAB};

/******************************************************************************
function:	Send four bytes
parameter:
    data: parameter
    command: command code
Info:  Set mode, enable port, clear serial port, switch DHCP, switch port 2
******************************************************************************/
void CH9120_TX_4_bytes(UCHAR data, int command)
{
    for (int i = 2; i < 4; i++)
    {
        if (i == 2)
            tx[i] = command;
        else
            tx[i] = data;
    }
    DEV_Delay_ms(10);
    for (int o = 0; o < 4; o++)
        uart_putc(UART_ID1, tx[o]);
    DEV_Delay_ms(10);
    for (int i = 2; i < 4; i++)
        tx[i] = 0;
}

/******************************************************************************
function:	Send five bytes
parameter:
    data: parameter
    command: command code
Info:  Set the local port and target port
******************************************************************************/
void CH9120_TX_5_bytes(UWORD data, int command)
{
    UCHAR Port[2];
    Port[0] = data & 0xff;
    Port[1] = data >> 8;
    for (int i = 2; i < 5; i++)
    {
        if (i == 2)
            tx[i] = command;
        else
            tx[i] = Port[i - 3];
    }
    DEV_Delay_ms(10);
    for (int o = 0; o < 5; o++)
        uart_putc(UART_ID1, tx[o]);
    DEV_Delay_ms(10);
    for (int i = 2; i < 5; i++)
        tx[i] = 0;
}
/******************************************************************************
function:	Send seven bytes
parameter:
    data: parameter
    command: command code
Info:  Set the IP address, subnet mask, gateway,
******************************************************************************/
void CH9120_TX_7_bytes(UCHAR data[], int command)
{
    for (int i = 2; i < 7; i++)
    {
        if (i == 2)
            tx[i] = command;
        else
            tx[i] = data[i - 3];
    }
    DEV_Delay_ms(10);
    for (int o = 0; o < 7; o++)
        uart_putc(UART_ID1, tx[o]);
    DEV_Delay_ms(10);
    for (int i = 2; i < 7; i++)
        tx[i] = 0;
}

/******************************************************************************
function:	CH9120_TX_BAUD
parameter:
    data: parameter
    command: command code
Info:  Set baud rate
******************************************************************************/
void CH9120_TX_BAUD(UDOUBLE data, int command)
{
    UCHAR Port[4];
    Port[0] = (data & 0xff);
    Port[1] = (data >> 8) & 0xff;
    Port[2] = (data >> 16) & 0xff;
    Port[3] = data >> 24;

    for (int i = 2; i < 7; i++)
    {
        if (i == 2)
            tx[i] = command;
        else
            tx[i] = Port[i - 3];
    }
    DEV_Delay_ms(10);
    for (int o = 0; o < 7; o++)
        uart_putc(UART_ID1, tx[o]);
    DEV_Delay_ms(10);
    for (int i = 2; i < 7; i++)
        tx[i] = 0;
}

/******************************************************************************
function:	CH9120_Eed
parameter:
Info:  Updating configuration Parameters
******************************************************************************/
void CH9120_Eed()
{
    tx[2] = 0x0d;
    uart_puts(UART_ID1, tx);
    DEV_Delay_ms(200);
    tx[2] = 0x0e;
    uart_puts(UART_ID1, tx);
    DEV_Delay_ms(200);
    tx[2] = 0x5e;
    uart_puts(UART_ID1, tx);
}

/**
 * delay x ms
**/
void DEV_Delay_ms(UDOUBLE xms)
{
    sleep_ms(xms);
}

void DEV_Delay_us(UDOUBLE xus)
{
    sleep_us(xus);
}

/******************************************************************************
function:	CH9120_init
parameter:
Info:  Initialize CH9120
******************************************************************************/
void CH9120_init(void)
{
    // Keep this call — even though pico_enable_stdio_uart=0 makes it a
    // mostly-no-op at compile time, removing it broke chip detection in
    // testing. Treat as a load-bearing artifact of Waveshare's demo;
    // do not delete without proving on-board that detection still works.
    stdio_init_all();

    uart_init(UART_ID1, Inti_BAUD_RATE);
    gpio_set_function(UART_TX_PIN1, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN1, GPIO_FUNC_UART);

    gpio_init(CFG_PIN);
    gpio_init(RES_PIN);
    gpio_set_dir(CFG_PIN, GPIO_OUT);
    gpio_set_dir(RES_PIN, GPIO_OUT);

    gpio_put(CFG_PIN, 0);
    gpio_put(RES_PIN, 1);

    DEV_Delay_ms(100);
    CH9120_TX_4_bytes(TCP_CLIENT, Mode1); //Mode
    DEV_Delay_ms(100);
    CH9120_TX_7_bytes(CH9120_LOCAL_IP, LOCAL_IP); //LOCALIP
    DEV_Delay_ms(100);
    CH9120_TX_7_bytes(CH9120_SUBNET_MASK, SUBNET_MASK); //SUBNET MASK
    DEV_Delay_ms(100);
    CH9120_TX_7_bytes(CH9120_GATEWAY, GATEWAY); //GATEWAY
    DEV_Delay_ms(100);
    CH9120_TX_7_bytes(CH9120_TARGET_IP, TARGET_IP1); //TARGET IP
    DEV_Delay_ms(100);

    CH9120_TX_5_bytes(CH9120_PORT1, LOCAL_PORT1); //Local port
    DEV_Delay_ms(100);
    CH9120_TX_5_bytes(CH9120_TARGET_PORT, TARGET_PORT1); //Target Port
    DEV_Delay_ms(100);
    CH9120_TX_BAUD(CH9120_BAUD_RATE, UART1_BAUD1);//Port 1 baud rate
    DEV_Delay_ms(100);

    // Chip-side TCP forwarding tuning. Dolphin's SI device does NOT loop
    // its recv() — when our chip splits a 3-byte STATUS response across
    // TCP packets, Dolphin gets recv()=1 and logs
    //   "expected_response_length(3) != actual_response_length(1)"
    // …then Madden retries the SI command instead of progressing. So we
    // MUST batch multi-byte responses into single TCP packets.
    //
    // PKT_LEN=5 (covers our biggest response = 5-byte READ). Multi-byte
    // responses fit and trigger forwarding immediately. PKT_TIMEOUT=1
    // (5ms) acts as the fallback for 1-byte WRITE acks. 5ms minimum
    // per ack means body upload pays ~20s for 4000 WRITEs — slower than
    // we want, but the only way to satisfy Dolphin's single-shot recv().
    CH9120_TX_BAUD((UDOUBLE)1, 0x23);    // CMD_PKT_TIMEOUT = 1 (5ms fallback)
    DEV_Delay_ms(100);
    CH9120_TX_BAUD((UDOUBLE)5, 0x25);    // CMD_PKT_LEN = 5 (covers max response)
    DEV_Delay_ms(100);

    CH9120_Eed();
    DEV_Delay_ms(500);
    gpio_put(CFG_PIN, 1);

    uart_set_baudrate(UART_ID1, Transport_BAUD_RATE);

    // CRITICAL: enable 32-byte HW FIFO. Pico-SDK's uart_init leaves the
    // FIFO disabled (1-byte holding register), which at 921600 baud means
    // any non-trivial work between polls (joybus xfer ~250µs ≈ 23 byte-
    // times) overruns RX. Multiboot streams 13K WRITEs back-to-back, so
    // a single missed byte desyncs Kawasedo decryption and the GBA never
    // accepts the bootblock. With the 32-byte FIFO we have ~350 µs of
    // headroom per drain — more than enough.
    uart_set_fifo_enabled(UART_ID1, true);

    while (uart_is_readable(UART_ID1))
        {
            UBYTE ch1 = uart_getc(UART_ID1);
        }
}


/******************************************************************************
function:	RX_TX
parameter:
Info:  Serial port 1 and serial port 2 receive and dispatch
******************************************************************************/
void RX_TX()
{
    while (1)
    {
        while (uart_is_readable(UART_ID1))
        {
            UBYTE ch1 = uart_getc(UART_ID1);
            if (uart_is_writable(UART_ID1))
            {
                uart_putc(UART_ID1, ch1);
            }
        }
    }
}