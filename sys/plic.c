// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "misc.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 


struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT]; /**< Interrupt Priorities registers */
		char _reserved_priority[0x1000];
	};

	union {
		uint32_t pending[PLIC_SRC_CNT/32]; /**< Interrupt Pending Bits registers */
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32]; /**< Interrupt Enables registers */
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;	/**< Priority Thresholds registers */
				uint32_t claim;	/**< Interrupt Claim/Completion registers */
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs*)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);

static int plic_source_pending(uint_fast32_t srcno);

static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);

static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);

static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);


static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 0). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;

	// Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno <= PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//

/* Function Interface:
	static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
	Inputs: uint_fast32_t srcno - interrupt source number
			uint_fast32_t level - priority level to assign to the source
	Outputs: None
	Description: Sets the interrupt priority level for the specified PLIC source number.
	Side Effects: - Writes to the PLIC priority register array
*/

// Sets the priority level for the given interrupt source in the PLIC.
static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
	PLIC.priority[srcno] = level;
}

/* Function Interface:
	static inline int plic_set_source_pending(uint_fast32_t srcno)
	Inputs: uint_fast32_t srcno - interrupt source number
	Outputs: returns 1 if the interrupt source is pending, 0 otherwise
	Description: Checks the PLIC pending array to determine whether an interrupt source has a pending interrupt.
	Side Effects: - Reads from PLIC pending register array
*/

// Returns 1 if the given interrupt source is pending, 0 otherwise.
static inline int plic_source_pending(uint_fast32_t srcno) {
	uint_fast32_t num = PLIC.pending[srcno/32];
	return (num >> (srcno%32)) & 1U;
}

/* Function Interface:
	static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
	Inputs: uint_fast32_t ctxno - interrupt context number
			uint_fast32_t srcno - interrupt source number
	Outputs: None
	Description: Enables the given interrupt source for the specified interrupt context by
	setting the corresponding bit in the PLIC enable register.
	Side Effects: - Modifies PLIC enable register for the given context
*/

// Enables the given interrupt source for a specific interrupt context.
static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) {
	PLIC.enable[ctxno][srcno/32] |= (1U << (srcno % 32));
}

/* Function Interface:
	static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid)
	Inputs: uint_fast32_t ctxno - interrupt context number
			uint_fast32_t srcid - interrupt source id
	Outputs: None
	Description: Disables the given interrupt source for the specified interrupt context by
	clearing its corresponding bit in the PLIC enable register.
	Side Effects: - Modifies PLIC enable register for the given context
*/

// Disables the given interrupt source for a specific interrupt context.
static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) {
	PLIC.enable[ctxno][srcid / 32] &= ~(1U << (srcid % 32));
}

/* Function Interface:
	static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
	Inputs: uint_fast32_t ctxno - interrupt context number
			uint_fast32_t level - interrupt priority threshold to set for the context
	Outputs: None
	Description: Sets the interrupt priority threshold for the specified PLIC context.
	Only interrupts with priority greater than this threshold will be forwarded.
	Side Effects: - Writes to the PLIC context threshold register
*/

// Sets the interrupt priority threshold for the specified context.
// Only interrupts above this level will be processed.
static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level) {
	PLIC.ctx[ctxno].threshold = level;
}

/* Function Interface:
	static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
	Inputs: uint_fast32_t ctxno - interrupt context number
	Outputs: returns the interrupt source number of the highest-priority pending interrupt for the context
	Description: Claims (acknowledges) a pending interrupt for the given PLIC context by reading the claim register
	Side Effects: - Reads and acknowledges a pending interrupt from the PLIC
*/

// Claims the highest-priority pending interrupt for the specified context and returns its source ID.
static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
	return PLIC.ctx[ctxno].claim;
}

/* Function Interface:
	static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
	Inputs: uint_fast32_t ctxno - interrupt context number
			uint_fast32_t srcno - interrupt source number
	Outputs: None
	Description: Signals completion of a previously claimed interrupt for the given context by
	writing the source number to the PLIC claim/complete register.
	Side Effects: - Modifies PLIC context's claim/complete register
*/

// Marks the given interrupt source as completed for the specified context.
static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
	PLIC.ctx[ctxno].claim = srcno;
}

/* Function Interface:
	static void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
	Inputs: uint_fast32_t ctxno - interrupt context number
	Outputs: None
	Description: Enables all interrupt sources for the specified PLIC context.
	writing the source number to the PLIC claim/complete register.
	Side Effects: - Modifies PLIC enable register array for the given context
*/

// Enables all interrupt sources for a given context.
static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
	for (int i = 0; i < 32; i++) {
		PLIC.enable[ctxno][i] = 0xFFFFFFFF;
	}
}

/* Function Interface:
	static void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
	Inputs: uint_fast32_t ctxno - interrupt context number
	Outputs: None
	Description: Disables all interrupt sources for the specified PLIC context.
	writing the source number to the PLIC claim/complete register.
	Side Effects: - Modifies PLIC enable register array for the given context
*/

// Disables all interrupt sources for a given context.
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
	for (int i = 0; i < 32; i++) {
		PLIC.enable[ctxno][i] = 0x00000000;
	}
}
