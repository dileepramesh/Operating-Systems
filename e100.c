#include <inc/stdio.h>
#include <inc/error.h>
#include <inc/string.h>
#include <kern/pci.h>
#include <kern/e100.h>
#include <kern/pcireg.h> 
#include <kern/pmap.h>
#include <kern/picirq.h>
#include <inc/x86.h>
#include <inc/ns.h>

// Global E100 driver structure
e100_driver_t e100_driver;

// Delay routine. 'n' specifies the number of microseconds
static void
delay (int n)
{
    int i;

    for (i = 0; i < n; i++) {
	inb(0x84);
    }
}

// Handle TX interrupts
void
e100_handle_tx_int (void)
{
    uint32_t q_head;

    q_head = e100_driver.tx_head;

    // 
    // Check if the operation was successful. If so, reclaim the CB buffer. We
    // need to check the C and OK bits for this.
    //
    if (e100_driver.tx[q_head].status & E100_CBL_STATUS_OK &&
	e100_driver.tx[q_head].status & E100_CBL_STATUS_C) {
	/* Reset the CB parameters */
	e100_driver.tx[q_head].command = 0;
	memset(&e100_driver.tx[q_head].tcb_data, 0x00, E100_MAX_PACKET_SIZE);
	e100_driver.tx_head = (e100_driver.tx_head + 1) % MAX_E100_TX_SLOTS;
    }
}

// Handle RX interrupts
void
e100_handle_rx_int (void)
{
    uint32_t q_head;
    struct jif_pkt *p;

    q_head = e100_driver.rx_head;

    // Get the actual number of bytes received from the RFD
    e100_driver.rx[q_head].actual_count = 
	e100_driver.rx[q_head].actual_count & RFD_ACTUAL_COUNT_MASK;

    // Anything else needs to be done here??
}

// Generic handler for the E100 interrupts
void
e100_handle_int (void)
{
    int status;

    // Check what was the type of interrupt raised
    status = inb(e100_driver.io_base + E100_SCB_STATUS_WORD);

    // Write back the acknowledgement
    outb(e100_driver.io_base + E100_SCB_STATUS_WORD, status);

    // TX interrupts
    if (status & (E100_SCB_STATUS_CXTNO | E100_SCB_STATUS_CNA)) {
	e100_handle_tx_int();
    }

    // RX interrupts
    if (status & E100_SCB_STATUS_FR) {
	e100_handle_rx_int();
    }

    // Notify the device that the interrupt was handled
    irq_eoi();
}

// Transmit a packet
int
e100_transmit_packet (void *pkt, uint32_t pkt_size)
{
    uint32_t q_head, q_tail;
    e100_dma_tx_t *tx;
    uint8_t *pkt_data = (uint8_t *)pkt;

    // Get the TX head and tail pointers
    q_head = e100_driver.tx_head;
    q_tail = e100_driver.tx_tail;

    // Ensure that we have enough space for the packet
    if (((q_tail + 1) % MAX_E100_TX_SLOTS) == q_head) {
	cprintf("e100_transmit_packet: tx buffer full\n");
	return -E_NO_MEM;
    }

    // Get the pointer to the next CB in the CBL
    tx = &e100_driver.tx[q_tail];

    // Setup the command parameters
    tx->status = 0;
    tx->command = E100_CBL_COMMAND_TX | E100_CBL_COMMAND_I | E100_CBL_COMMAND_S;
    tx->tcb_byte_count = pkt_size;

    // Copy the packet data to CB
    memmove(&tx->tcb_data, pkt_data, pkt_size);

    // Update the tail pointer
    e100_driver.tx_tail = (e100_driver.tx_tail + 1) % MAX_E100_TX_SLOTS;

    // Activate or resume the CU based on the transmit state
    if (e100_driver.tx_state == E100_TX_STATE_IDLE) {
	// 
	// Copy the physical address of the first CB to SCB general pointer
	// offset
	//
	outl(e100_driver.io_base + E100_SCB_GENERAL_POINTER, 
	     PADDR(&e100_driver.tx[q_head]));

	// Activate the CU
	outb(e100_driver.io_base + E100_SCB_COMMAND_WORD, 
	     E100_SCB_COMMAND_CU_START);

	// Update the transmit state
	e100_driver.tx_state = E100_TX_STATE_ACTIVE;

    } else {
	// Resume the CU
	outb(e100_driver.io_base + E100_SCB_COMMAND_WORD, 
	     E100_SCB_COMMAND_CU_RESUME);
    }

    return 0;
}

// Receive a packet
int
e100_receive_packet (void *pkt_buf)
{
    uint32_t q_head;
    e100_dma_rx_t *rx;
    struct jif_pkt *pkt = (struct jif_pkt *)pkt_buf;

    // Get the RX head
    q_head = e100_driver.rx_head;

    // Get the pointer to the current RFD
    rx = &e100_driver.rx[q_head]; 

    // See if we have a packet to send back
    if ((rx->status & E100_RFD_STATUS_OK) && 
	(rx->status & E100_RFD_STATUS_C)) {

	// Mask out the F and EOF bits
	rx->actual_count = rx->actual_count & RFD_ACTUAL_COUNT_MASK;

	// Copy over the contents to the buffer passed by the caller
 	pkt->jp_len = rx->actual_count;
	memmove(pkt->jp_data, rx->data, pkt->jp_len);

	// Reset the RFD
	rx->status = 0;
	rx->command = 0;
	rx->size = E100_MAX_PACKET_SIZE;

	// Update the head pointer
	e100_driver.rx_head = (e100_driver.rx_head + 1) % MAX_E100_RX_SLOTS; 

	return 0;

    } else {
	// No luck. Ask the caller to retry.
	return -E_NO_PKT;
    }
   
    return 0;
}

//
// E100 Attach function
//
int
e100_attach (struct pci_func *pcif)
{
    int i;
    uint32_t next;

    // Enable the E100 device
    pci_func_enable(pcif);
    delay(4);
    
    // Initialize the driver structure
    e100_driver.mem_base = pcif->reg_base[0];
    e100_driver.io_base = pcif->reg_base[1];

    // Reset the device
    outl(e100_driver.io_base + E100_PORT, 0);
    delay(4);
    
    // 
    // Setup the Transmit DMA ring. This is implemented as an array of struct
    // e100_dma_tx_t.
    //
    for (i = 0; i < MAX_E100_TX_SLOTS; i++) {

	// Get the index of the next CB in the list
	next = (i + 1) % MAX_E100_TX_SLOTS;

	// Zero out the CB block
	memset(&e100_driver.tx[i], 0, sizeof(e100_dma_tx_t));

	// Initialize the contents
	e100_driver.tx[i].link = PADDR(&e100_driver.tx[next]);
	e100_driver.tx[i].tbd_array_addr = 0xffffffff;
	e100_driver.tx[i].threshold = 0xE0;
    }

    // Initialize the transmit queue parameters
    e100_driver.tx_head = e100_driver.tx_tail = 0;

    // Set the initial states of the CU
    e100_driver.tx_state = E100_TX_STATE_IDLE;

    // 
    // Setup the Receive DMA ring. This is implemented as an array of struct
    // e100_dma_rx_t.
    //
    for (i = 0; i < MAX_E100_RX_SLOTS; i++) {

	// Get the index of the next RFD in the list
	next = (i + 1) % MAX_E100_RX_SLOTS;

	// Zero out the RFD block
	memset(&e100_driver.rx[i], 0, sizeof(e100_dma_rx_t));

	// Initialize the contents
	e100_driver.rx[i].status = 0;
	e100_driver.rx[i].command = 0;
	e100_driver.rx[i].size = E100_MAX_PACKET_SIZE;
	e100_driver.rx[i].link = PADDR(&e100_driver.rx[next]);
    }

    // 
    // Start the RU. Copy the physical address of the first RFD to SCB general 
    // pointer offset
    //
    outl(e100_driver.io_base + E100_SCB_GENERAL_POINTER,
	 PADDR(&e100_driver.rx[0]));

    outb(e100_driver.io_base + E100_SCB_COMMAND_WORD, 
	 E100_SCB_COMMAND_RU_START);

    // Initialize the receive queue parameters
    e100_driver.rx_head = 0;
    e100_driver.rx_tail = MAX_E100_RX_SLOTS - 1;

    // Set the initial states of the CU and RU
    e100_driver.tx_state = E100_TX_STATE_IDLE;
    e100_driver.rx_state = E100_RX_STATE_READY;

    // All done. Enable the E100 interrupts.
    irq_setmask_8259A(irq_mask_8259A & ~(1 << pcif->irq_line));

    return 0;
}

/* End of File */
