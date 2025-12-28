.syntax unified
.thumb

.text
.align 2
.thumb_func
.section .isr_vector
.global __isr_vector
__isr_vector:
    .long __StackTop            /* 0: Initial Stack Pointer */
    .long Reset_Handler         /* 1: Reset Handler */
    .long NMI_Handler           /* 2: NMI Handler */
    .long HardFault_Handler     /* 3: Hard Fault Handler */
    .long MemManage_Handler     /* 4: MPU Fault Handler */
    .long BusFault_Handler      /* 5: Bus Fault Handler */
    .long UsageFault_Handler    /* 6: Usage Fault Handler */
    .long SecureFault_Handler   /* 7: Secure Fault Handler (ARMv8-M) */
    .long 0                     /* 8: Reserved */
    .long 0                     /* 9: Reserved */
    .long 0                     /* 10: Reserved */
    .long SVC_Handler           /* 11: SVCall Handler */
    .long DebugMon_Handler      /* 12: Debug Monitor Handler */
    .long 0                     /* 13: Reserved */
    .long PendSV_Handler        /* 14: PendSV Handler */
    .long SysTick_Handler       /* 15: SysTick Handler */

    /* External Interrupts - MPS2 AN505 specific */
    .long NONSEC_WATCHDOG_Handler   /* 0: Non-Secure Watchdog */
    .long S32K_TIMER_Handler        /* 1: S32K Timer */
    .long TIMER0_Handler            /* 2: Timer 0 */
    .long TIMER1_Handler            /* 3: Timer 1 */
    .long DUALTIMER_Handler         /* 4: Dual Timer */
    .long MHU0_Handler              /* 5: MHU 0 */
    .long MHU1_Handler              /* 6: MHU 1 */
    .long CRYPTOCELL_Handler        /* 7: CryptoCell */
    .long MPC_Handler               /* 8: MPC Combined */
    .long PPC_Handler               /* 9: PPC Combined */
    .long MSC_Handler               /* 10: MSC Combined */
    .long BRIDGE_ERROR_Handler      /* 11: Bridge Error */
    .long INVALID_INSTR_CACHE_Handler /* 12: Invalid Instruction Cache */
    .long 0                         /* 13: Reserved */
    .long SYS_PPU_Handler           /* 14: SYS PPU */
    .long CPU0_PPU_Handler          /* 15: CPU0 PPU */
    .long CPU1_PPU_Handler          /* 16: CPU1 PPU */
    .long CPU0_DBG_PPU_Handler      /* 17: CPU0 Debug PPU */
    .long CPU1_DBG_PPU_Handler      /* 18: CPU1 Debug PPU */
    .long CRYPT_PPU_Handler         /* 19: Crypt PPU */
    .long 0                         /* 20: Reserved */
    .long RAM0_PPU_Handler          /* 21: RAM0 PPU */
    .long RAM1_PPU_Handler          /* 22: RAM1 PPU */
    .long RAM2_PPU_Handler          /* 23: RAM2 PPU */
    .long RAM3_PPU_Handler          /* 24: RAM3 PPU */
    .long DEBUG_PPU_Handler         /* 25: Debug PPU */
    .long 0                         /* 26: Reserved */
    .long CPU0_CTI_Handler          /* 27: CPU0 CTI */
    .long CPU1_CTI_Handler          /* 28: CPU1 CTI */
    .long 0                         /* 29: Reserved */
    .long 0                         /* 30: Reserved */
    .long 0                         /* 31: Reserved */
    /* GPIO Interrupts */
    .long GPIO0_0_Handler           /* 32: GPIO0 Pin 0 */
    .long GPIO0_1_Handler           /* 33: GPIO0 Pin 1 */
    .long GPIO0_2_Handler           /* 34: GPIO0 Pin 2 */
    .long GPIO0_3_Handler           /* 35: GPIO0 Pin 3 */
    .long GPIO0_4_Handler           /* 36: GPIO0 Pin 4 */
    .long GPIO0_5_Handler           /* 37: GPIO0 Pin 5 */
    .long GPIO0_6_Handler           /* 38: GPIO0 Pin 6 */
    .long GPIO0_7_Handler           /* 39: GPIO0 Pin 7 */
    .long GPIO0_8_Handler           /* 40: GPIO0 Pin 8 */
    .long GPIO0_9_Handler           /* 41: GPIO0 Pin 9 */
    .long GPIO0_10_Handler          /* 42: GPIO0 Pin 10 */
    .long GPIO0_11_Handler          /* 43: GPIO0 Pin 11 */
    .long GPIO0_12_Handler          /* 44: GPIO0 Pin 12 */
    .long GPIO0_13_Handler          /* 45: GPIO0 Pin 13 */
    .long GPIO0_14_Handler          /* 46: GPIO0 Pin 14 */
    .long GPIO0_15_Handler          /* 47: GPIO0 Pin 15 */
    .long GPIO1_0_Handler           /* 48: GPIO1 Pin 0 */
    .long GPIO1_1_Handler           /* 49: GPIO1 Pin 1 */
    .long GPIO1_2_Handler           /* 50: GPIO1 Pin 2 */
    .long GPIO1_3_Handler           /* 51: GPIO1 Pin 3 */
    .long GPIO1_4_Handler           /* 52: GPIO1 Pin 4 */
    .long GPIO1_5_Handler           /* 53: GPIO1 Pin 5 */
    .long GPIO1_6_Handler           /* 54: GPIO1 Pin 6 */
    .long GPIO1_7_Handler           /* 55: GPIO1 Pin 7 */
    .long GPIO1_8_Handler           /* 56: GPIO1 Pin 8 */
    .long GPIO1_9_Handler           /* 57: GPIO1 Pin 9 */
    .long GPIO1_10_Handler          /* 58: GPIO1 Pin 10 */
    .long GPIO1_11_Handler          /* 59: GPIO1 Pin 11 */
    .long GPIO1_12_Handler          /* 60: GPIO1 Pin 12 */
    .long GPIO1_13_Handler          /* 61: GPIO1 Pin 13 */
    .long GPIO1_14_Handler          /* 62: GPIO1 Pin 14 */
    .long GPIO1_15_Handler          /* 63: GPIO1 Pin 15 */
    /* UART Interrupts */
    .long UART0_RX_Handler          /* 64: UART0 RX */
    .long UART0_TX_Handler          /* 65: UART0 TX */
    .long UART0_Combined_Handler    /* 66: UART0 Combined */
    .long UART1_RX_Handler          /* 67: UART1 RX */
    .long UART1_TX_Handler          /* 68: UART1 TX */
    .long UART1_Combined_Handler    /* 69: UART1 Combined */

.text
.thumb

/* Reset Handler */
.global Reset_Handler
.type Reset_Handler, %function
.thumb_func
Reset_Handler:
    /* Enable FPU: set CP10 and CP11 to full access in CPACR */
    movw r0, #0xED88            /* CPACR address low */
    movt r0, #0xE000            /* CPACR address high */
    ldr r1, [r0]
    orr r1, r1, #(0xF << 20)    /* Set CP10 and CP11 to full access */
    str r1, [r0]
    dsb                         /* Data Synchronization Barrier */
    isb                         /* Instruction Synchronization Barrier */

    bl _mainCRTStartup
    /* If main returns, loop forever */
.Lloop_forever:
    b .Lloop_forever
.size Reset_Handler, . - Reset_Handler

.type HardFault_Handler, %function
.thumb_func
HardFault_Handler:
    bkpt #0
label:
    b label

/* Default handler for all other interrupts */
.macro def_irq_handler handler_name
.weak \handler_name
.type \handler_name, %function
.thumb_func
\handler_name:
    b \handler_name
.size \handler_name, . - \handler_name
.endm

/* Core Exception Handlers */
def_irq_handler NMI_Handler
def_irq_handler MemManage_Handler
def_irq_handler BusFault_Handler
def_irq_handler UsageFault_Handler
def_irq_handler SecureFault_Handler
def_irq_handler SVC_Handler
def_irq_handler DebugMon_Handler
def_irq_handler PendSV_Handler
def_irq_handler SysTick_Handler

/* External Interrupt Handlers */
def_irq_handler NONSEC_WATCHDOG_Handler
def_irq_handler S32K_TIMER_Handler
def_irq_handler TIMER0_Handler
def_irq_handler TIMER1_Handler
def_irq_handler DUALTIMER_Handler
def_irq_handler MHU0_Handler
def_irq_handler MHU1_Handler
def_irq_handler CRYPTOCELL_Handler
def_irq_handler MPC_Handler
def_irq_handler PPC_Handler
def_irq_handler MSC_Handler
def_irq_handler BRIDGE_ERROR_Handler
def_irq_handler INVALID_INSTR_CACHE_Handler
def_irq_handler SYS_PPU_Handler
def_irq_handler CPU0_PPU_Handler
def_irq_handler CPU1_PPU_Handler
def_irq_handler CPU0_DBG_PPU_Handler
def_irq_handler CPU1_DBG_PPU_Handler
def_irq_handler CRYPT_PPU_Handler
def_irq_handler RAM0_PPU_Handler
def_irq_handler RAM1_PPU_Handler
def_irq_handler RAM2_PPU_Handler
def_irq_handler RAM3_PPU_Handler
def_irq_handler DEBUG_PPU_Handler
def_irq_handler CPU0_CTI_Handler
def_irq_handler CPU1_CTI_Handler

/* GPIO Interrupt Handlers */
def_irq_handler GPIO0_0_Handler
def_irq_handler GPIO0_1_Handler
def_irq_handler GPIO0_2_Handler
def_irq_handler GPIO0_3_Handler
def_irq_handler GPIO0_4_Handler
def_irq_handler GPIO0_5_Handler
def_irq_handler GPIO0_6_Handler
def_irq_handler GPIO0_7_Handler
def_irq_handler GPIO0_8_Handler
def_irq_handler GPIO0_9_Handler
def_irq_handler GPIO0_10_Handler
def_irq_handler GPIO0_11_Handler
def_irq_handler GPIO0_12_Handler
def_irq_handler GPIO0_13_Handler
def_irq_handler GPIO0_14_Handler
def_irq_handler GPIO0_15_Handler
def_irq_handler GPIO1_0_Handler
def_irq_handler GPIO1_1_Handler
def_irq_handler GPIO1_2_Handler
def_irq_handler GPIO1_3_Handler
def_irq_handler GPIO1_4_Handler
def_irq_handler GPIO1_5_Handler
def_irq_handler GPIO1_6_Handler
def_irq_handler GPIO1_7_Handler
def_irq_handler GPIO1_8_Handler
def_irq_handler GPIO1_9_Handler
def_irq_handler GPIO1_10_Handler
def_irq_handler GPIO1_11_Handler
def_irq_handler GPIO1_12_Handler
def_irq_handler GPIO1_13_Handler
def_irq_handler GPIO1_14_Handler
def_irq_handler GPIO1_15_Handler

/* UART Interrupt Handlers */
def_irq_handler UART0_RX_Handler
def_irq_handler UART0_TX_Handler
def_irq_handler UART0_Combined_Handler
def_irq_handler UART1_RX_Handler
def_irq_handler UART1_TX_Handler
def_irq_handler UART1_Combined_Handler
