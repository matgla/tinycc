.syntax unified
.thumb

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