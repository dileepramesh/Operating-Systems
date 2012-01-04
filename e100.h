#ifndef JOS_KERN_E100_H
#define JOS_KERN_E100_H

#include <kern/pci.h>

/* Defines */

/* Vendor and Device IDs for E100 */
#define E100_VENDOR_ID			0x8086
#define E100_DEVICE_ID			0x1209

/* Maximum size of the transmit and receive DMA rings */
#define MAX_E100_TX_SLOTS		20
#define MAX_E100_RX_SLOTS		20

/* Transmit States */
#define E100_TX_STATE_IDLE		0x0
#define E100_TX_STATE_ACTIVE		0x1

/* Receive States */
#define E100_RX_STATE_IDLE		0x0
#define E100_RX_STATE_READY		0x1

/* Offsets in the CSR for the SCB and Port blocks */
#define E100_SCB_STATUS_WORD		0x0001
#define E100_SCB_COMMAND_WORD		0x0002
#define E100_SCB_GENERAL_POINTER	0x0004
#define E100_PORT			0x0008

/* Different commands that can be issued via the SCB command block */
#define E100_SCB_COMMAND_RU_START	0X1
#define E100_SCB_COMMAND_RU_RESUME	0X2
#define E100_SCB_COMMAND_CU_START	0X10
#define E100_SCB_COMMAND_CU_RESUME	0X20

/* Different commands/Flags that can be issued for a given CBL */
#define E100_CBL_COMMAND_TX		0x4
#define E100_CBL_COMMAND_I		0x2000
#define E100_CBL_COMMAND_S		0x4000

/* SCB status flags */
#define E100_SCB_STATUS_RNR		0x10
#define E100_SCB_STATUS_CNA		0x20
#define E100_SCB_STATUS_FR		0x40
#define E100_SCB_STATUS_CXTNO		0x80

/* CB status flags */
#define E100_CBL_STATUS_OK		0x2000
#define E100_CBL_STATUS_C		0x8000

/* RFA commands */
#define E100_RFA_COMMAND_S		0x4000

/* RFD status flags */
#define E100_RFD_STATUS_OK		0x2000
#define E100_RFD_STATUS_C		0x8000

/* Mask for the actual number of bytes received by the driver */
#define RFD_ACTUAL_COUNT_MASK		0x3FFF

/* Maximum packet size is same as that of the maximum ethernet packet size */
#define E100_MAX_PACKET_SIZE		1518

/* Data Structures */

typedef struct e100_dma_rx_ {
    volatile uint16_t	status;
    volatile uint16_t	command;
    volatile uint32_t	link;
    volatile uint32_t	reserved;
    volatile uint16_t	actual_count;
    volatile uint16_t	size;
    uint8_t		data[E100_MAX_PACKET_SIZE];
    uint8_t		padding[18]; /* To make it word aligned! */
} e100_dma_rx_t;

typedef struct e100_dma_tx_ {
    volatile uint16_t	status;
    volatile uint16_t	command;
    volatile uint32_t	link;
    volatile uint32_t	tbd_array_addr;
    volatile uint16_t	tcb_byte_count;
    volatile uint8_t	threshold;
    volatile uint8_t	tbd_count;
    uint8_t		tcb_data[E100_MAX_PACKET_SIZE];
    uint8_t		padding[18]; /* To make it word aligned! */
} e100_dma_tx_t;

typedef struct e100_driver_ {
    uint32_t	    mem_base;
    uint32_t	    io_base;
    e100_dma_tx_t   tx[MAX_E100_TX_SLOTS];
    e100_dma_rx_t   rx[MAX_E100_RX_SLOTS];
    uint8_t	    tx_state;
    uint8_t	    rx_state;
    uint32_t	    tx_head;
    uint32_t	    tx_tail;
    uint32_t	    rx_head;
    uint32_t	    rx_tail;
} e100_driver_t;

void e100_handle_int (void);
int e100_transmit_packet (void *pkt_data, uint32_t pkt_size);
int e100_receive_packet (void *pkt_buf);
int e100_attach (struct pci_func *pcif);

#endif	// JOS_KERN_E100_H
