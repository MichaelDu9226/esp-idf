/*
 * SPDX-FileCopyrightText: 2020 Amazon.com, Inc. or its affiliates
 *
 * SPDX-License-Identifier: MIT
 *
 * SPDX-FileContributor: 2016-2023 Espressif Systems (Shanghai) CO LTD
 */
/*
 * FreeRTOS Kernel V10.4.3
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 * 1 tab == 4 spaces!
 */

/*-----------------------------------------------------------------------
 * Implementation of functions defined in portable.h for the RISC-V port.
 *----------------------------------------------------------------------*/

#include "sdkconfig.h"
#include <string.h>
#include "soc/soc_caps.h"
#include "soc/periph_defs.h"
#include "soc/system_reg.h"
#include "hal/systimer_hal.h"
#include "hal/systimer_ll.h"
#include "riscv/rvruntime-frames.h"
#include "riscv/rv_utils.h"
#include "riscv/interrupt.h"
#include "esp_private/crosscore_int.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "FreeRTOS.h"       /* This pulls in portmacro.h */
#include "task.h"
#include "portmacro.h"
#include "port_systick.h"
#include "esp_memory_utils.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "soc/hp_system_reg.h"
#endif

_Static_assert(portBYTE_ALIGNMENT == 16, "portBYTE_ALIGNMENT must be set to 16");
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
/**
 * offsetof() can not be used in asm code. Then we need make sure that
 * PORT_OFFSET_PX_STACK and PORT_OFFSET_PX_END_OF_STACK have expected values.
 * Macro used in the portasm.S instead of variables to save at least 4 instruction calls
 * which accessing DRAM memory. This optimization saves CPU time in the interrupt handling.
 */

_Static_assert(offsetof( StaticTask_t, pxDummy6 ) == PORT_OFFSET_PX_STACK);
_Static_assert(offsetof( StaticTask_t, pxDummy8 ) == PORT_OFFSET_PX_END_OF_STACK);
#endif // CONFIG_ESP_SYSTEM_HW_STACK_GUARD

/* ---------------------------------------------------- Variables ------------------------------------------------------
 *
 * ------------------------------------------------------------------------------------------------------------------ */

volatile UBaseType_t port_xSchedulerRunning[portNUM_PROCESSORS] = {0}; // Indicates whether scheduler is running on a per-core basis
volatile UBaseType_t port_uxInterruptNesting[portNUM_PROCESSORS] = {0};  // Interrupt nesting level. Increased/decreased in portasm.c
volatile UBaseType_t port_uxCriticalNesting[portNUM_PROCESSORS] = {0};
volatile UBaseType_t port_uxOldInterruptState[portNUM_PROCESSORS] = {0};
volatile UBaseType_t xPortSwitchFlag[portNUM_PROCESSORS] = {0};

/*
*******************************************************************************
* Interrupt stack. The size of the interrupt stack is determined by the config
* parameter "configISR_STACK_SIZE" in FreeRTOSConfig.h
*******************************************************************************
*/
__attribute__((aligned(16))) StackType_t xIsrStack[portNUM_PROCESSORS][configISR_STACK_SIZE];
StackType_t *xIsrStackTop[portNUM_PROCESSORS] = {0};

/* ------------------------------------------------ FreeRTOS Portable --------------------------------------------------
 * - Provides implementation for functions required by FreeRTOS
 * - Declared in portable.h
 * ------------------------------------------------------------------------------------------------------------------ */

// ----------------- Scheduler Start/End -------------------

BaseType_t xPortStartScheduler(void)
{
    /* Initialize all kernel state tracking variables */
    BaseType_t coreID = xPortGetCoreID();
    port_uxInterruptNesting[coreID] = 0;
    port_uxCriticalNesting[coreID] = 0;
    port_xSchedulerRunning[coreID] = 0;

    /* Initialize ISR Stack top */
    for (int i = 0; i < portNUM_PROCESSORS; i++) {
        xIsrStackTop[i] = &xIsrStack[i][0] + (configISR_STACK_SIZE & (~((portPOINTER_SIZE_TYPE)portBYTE_ALIGNMENT_MASK)));
    }

    /* Setup the hardware to generate the tick. */
    vPortSetupTimer();

#if !SOC_INT_CLIC_SUPPORTED
    esprv_intc_int_set_threshold(1); /* set global INTC masking level */
#else
    esprv_intc_int_set_threshold(0); /* set global CLIC masking level. When CLIC is supported, all interrupt priority levels less than or equal to the threshold level are masked. */
#endif /* !SOC_INT_CLIC_SUPPORTED */
    rv_utils_intr_global_enable();

    vPortYield();

    /* Should not get here */
    return pdTRUE;
}

void vPortEndScheduler(void)
{
    /* very unlikely this function will be called, so just trap here */
    abort();
}

// ------------------------ Stack --------------------------

/**
 * @brief Align stack pointer in a downward growing stack
 *
 * This macro is used to round a stack pointer downwards to the nearest n-byte boundary, where n is a power of 2.
 * This macro is generally used when allocating aligned areas on a downward growing stack.
 */
#define STACKPTR_ALIGN_DOWN(n, ptr)     ((ptr) & (~((n)-1)))

/**
 * @brief Allocate and initialize GCC TLS area
 *
 * This function allocates and initializes the area on the stack used to store GCC TLS (Thread Local Storage) variables.
 * - The area's size is derived from the TLS section's linker variables, and rounded up to a multiple of 16 bytes
 * - The allocated area is aligned to a 16-byte aligned address
 * - The TLS variables in the area are then initialized
 *
 * Each task access the TLS variables using the THREADPTR register plus an offset to obtain the address of the variable.
 * The value for the THREADPTR register is also calculated by this function, and that value should be use to initialize
 * the THREADPTR register.
 *
 * @param[in] uxStackPointer Current stack pointer address
 * @param[out] ret_threadptr_reg_init Calculated THREADPTR register initialization value
 * @return Stack pointer that points to the TLS area
 */
FORCE_INLINE_ATTR UBaseType_t uxInitialiseStackTLS(UBaseType_t uxStackPointer, uint32_t *ret_threadptr_reg_init)
{
    /*
    TLS layout at link-time, where 0xNNN is the offset that the linker calculates to a particular TLS variable.

    LOW ADDRESS
            |---------------------------|   Linker Symbols
            | Section                   |   --------------
            | .flash.rodata             |
         0x0|---------------------------| <- _flash_rodata_start
          ^ | Other Data                |
          | |---------------------------| <- _thread_local_start
          | | .tbss                     | ^
          V |                           | |
      0xNNN | int example;              | | tls_area_size
            |                           | |
            | .tdata                    | V
            |---------------------------| <- _thread_local_end
            | Other data                |
            | ...                       |
            |---------------------------|
    HIGH ADDRESS
    */
    // Calculate TLS area size and round up to multiple of 16 bytes.
    extern char _thread_local_start, _thread_local_end, _flash_rodata_start;
    const uint32_t tls_area_size = ALIGNUP(16, (uint32_t)&_thread_local_end - (uint32_t)&_thread_local_start);
    // TODO: check that TLS area fits the stack

    // Allocate space for the TLS area on the stack. The area must be aligned to 16-bytes
    uxStackPointer = STACKPTR_ALIGN_DOWN(16, uxStackPointer - (UBaseType_t)tls_area_size);
    // Initialize the TLS area with the initialization values of each TLS variable
    memcpy((void *)uxStackPointer, &_thread_local_start, tls_area_size);

    /*
    Calculate the THREADPTR register's initialization value based on the link-time offset and the TLS area allocated on
    the stack.

    HIGH ADDRESS
            |---------------------------|
            | .tdata (*)                |
          ^ | int example;              |
          | |                           |
          | | .tbss (*)                 |
          | |---------------------------| <- uxStackPointer (start of TLS area)
    0xNNN | |                           | ^
          | |                           | |
          |             ...               | _thread_local_start - _rodata_start
          | |                           | |
          | |                           | V
          V |                           | <- threadptr register's value

    LOW ADDRESS
    */
    *ret_threadptr_reg_init = (uint32_t)uxStackPointer - ((uint32_t)&_thread_local_start - (uint32_t)&_flash_rodata_start);
    return uxStackPointer;
}

#if CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER
static void vPortTaskWrapper(TaskFunction_t pxCode, void *pvParameters)
{
    __asm__ volatile(".cfi_undefined ra");  // tell to debugger that it's outermost (inital) frame
    extern void __attribute__((noreturn)) panic_abort(const char *details);
    static char DRAM_ATTR msg[80] = "FreeRTOS: FreeRTOS Task \"\0";
    pxCode(pvParameters);
    /* FreeRTOS tasks should not return. Log the task name and abort. */
    /* We cannot use s(n)printf because it is in flash */
    strcat(msg, pcTaskGetName(NULL));
    strcat(msg, "\" should not return, Aborting now!");
    panic_abort(msg);
}
#endif // CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER

/**
 * @brief Initialize the task's starting interrupt stack frame
 *
 * This function initializes the task's starting interrupt stack frame. The dispatcher will use this stack frame in a
 * context restore routine. Therefore, the starting stack frame must be initialized as if the task was interrupted right
 * before its first instruction is called.
 *
 * - The stack frame is allocated to a 16-byte aligned address
 *
 * @param[in] uxStackPointer Current stack pointer address
 * @param[in] pxCode Task function
 * @param[in] pvParameters Task function's parameter
 * @param[in] threadptr_reg_init THREADPTR register initialization value
 * @return Stack pointer that points to the stack frame
 */
FORCE_INLINE_ATTR UBaseType_t uxInitialiseStackFrame(UBaseType_t uxStackPointer, TaskFunction_t pxCode, void *pvParameters, uint32_t threadptr_reg_init)
{
    /*
    Allocate space for the task's starting interrupt stack frame.
    - The stack frame must be allocated to a 16-byte aligned address.
    - We use RV_STK_FRMSZ as it rounds up the total size to a multiple of 16.
    */
    uxStackPointer = STACKPTR_ALIGN_DOWN(16, uxStackPointer - RV_STK_FRMSZ);

    // Clear the entire interrupt stack frame
    RvExcFrame *frame = (RvExcFrame *)uxStackPointer;
    memset(frame, 0, sizeof(RvExcFrame));

    /* Initialize the stack frame. */
    extern uint32_t __global_pointer$;
    #if CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER
        frame->mepc = (UBaseType_t)vPortTaskWrapper;
        frame->a0 = (UBaseType_t)pxCode;
        frame->a1 = (UBaseType_t)pvParameters;
    #else
        frame->mepc = (UBaseType_t)pxCode;
        frame->a0 = (UBaseType_t)pvParameters;
    #endif // CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER
    frame->gp = (UBaseType_t)&__global_pointer$;
    frame->tp = (UBaseType_t)threadptr_reg_init;

    return uxStackPointer;
}

StackType_t *pxPortInitialiseStack(StackType_t *pxTopOfStack, TaskFunction_t pxCode, void *pvParameters)
{
#ifdef __clang_analyzer__
    // Teach clang-tidy that pxTopOfStack cannot be a pointer to const
    volatile StackType_t * pxTemp = pxTopOfStack;
    pxTopOfStack = pxTemp;
#endif /*__clang_analyzer__ */
    /*
    HIGH ADDRESS
    |---------------------------| <- pxTopOfStack on entry
    | TLS Variables             |
    | ------------------------- | <- Start of useable stack
    | Starting stack frame      |
    | ------------------------- | <- pxTopOfStack on return (which is the tasks current SP)
    |             |             |
    |             |             |
    |             V             |
    ----------------------------- <- Bottom of stack
    LOW ADDRESS

    - All stack areas are aligned to 16 byte boundary
    - We use UBaseType_t for all of stack area initialization functions for more convenient pointer arithmetic
    */

    UBaseType_t uxStackPointer = (UBaseType_t)pxTopOfStack;
    configASSERT((uxStackPointer & portBYTE_ALIGNMENT_MASK) == 0);

    // IDF-7770: Support FPU context save area for P4

    // Initialize GCC TLS area
    uint32_t threadptr_reg_init;
    uxStackPointer = uxInitialiseStackTLS(uxStackPointer, &threadptr_reg_init);
    configASSERT((uxStackPointer & portBYTE_ALIGNMENT_MASK) == 0);

    // Initialize the starting interrupt stack frame
    uxStackPointer = uxInitialiseStackFrame(uxStackPointer, pxCode, pvParameters, threadptr_reg_init);
    configASSERT((uxStackPointer & portBYTE_ALIGNMENT_MASK) == 0);

    // Return the task's current stack pointer address which should point to the starting interrupt stack frame
    return (StackType_t *)uxStackPointer;
}



/* ---------------------------------------------- Port Implementations -------------------------------------------------
 *
 * ------------------------------------------------------------------------------------------------------------------ */

// --------------------- Interrupts ------------------------

BaseType_t xPortInIsrContext(void)
{
#if (configNUM_CORES > 1)
    unsigned int irqStatus;
    BaseType_t ret;

    /* Disable interrupts to fetch the coreID atomically */
    irqStatus = portSET_INTERRUPT_MASK_FROM_ISR();

    /* Return the interrupt nexting counter for this core */
    ret = port_uxInterruptNesting[xPortGetCoreID()];

    /* Restore interrupts */
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irqStatus);

    return ret;
#else
    /* Optimize the call for single-core targets */
    return port_uxInterruptNesting[0];
#endif /* (configNUM_CORES > 1) */
}

BaseType_t IRAM_ATTR xPortInterruptedFromISRContext(void)
{
    /* Return the interrupt nexting counter for this core */
    return port_uxInterruptNesting[xPortGetCoreID()];
}

UBaseType_t xPortSetInterruptMaskFromISR(void)
{
    UBaseType_t prev_int_level = 0;

#if !SOC_INT_CLIC_SUPPORTED
    unsigned old_mstatus = RV_CLEAR_CSR(mstatus, MSTATUS_MIE);
    prev_int_level = REG_READ(INTERRUPT_CORE0_CPU_INT_THRESH_REG);
    REG_WRITE(INTERRUPT_CORE0_CPU_INT_THRESH_REG, RVHAL_EXCM_LEVEL);
    RV_SET_CSR(mstatus, old_mstatus & MSTATUS_MIE);
#else
    /* When CLIC is supported, all interrupt priority levels less than or equal to the threshold level are masked. */
    prev_int_level = rv_utils_set_intlevel(RVHAL_EXCM_LEVEL - 1);
#endif /* !SOC_INIT_CLIC_SUPPORTED */
    /**
     * In theory, this function should not return immediately as there is a
     * delay between the moment we mask the interrupt threshold register and
     * the moment a potential lower-priority interrupt is triggered (as said
     * above), it should have a delay of 2 machine cycles/instructions.
     *
     * However, in practice, this function has an epilogue of one instruction,
     * thus the instruction masking the interrupt threshold register is
     * followed by two instructions: `ret` and `csrrs` (RV_SET_CSR).
     * That's why we don't need any additional nop instructions here.
     */
    return prev_int_level;
}

void vPortClearInterruptMaskFromISR(UBaseType_t prev_int_level)
{
#if !SOC_INT_CLIC_SUPPORTED
    REG_WRITE(INTERRUPT_CORE0_CPU_INT_THRESH_REG, prev_int_level);
#else
    rv_utils_restore_intlevel(prev_int_level);
#endif /* SOC_INIT_CLIC_SUPPORTED */
    /**
     * The delay between the moment we unmask the interrupt threshold register
     * and the moment the potential requested interrupt is triggered is not
     * null: up to three machine cycles/instructions can be executed.
     *
     * When compilation size optimization is enabled, this function and its
     * callers returning void will have NO epilogue, thus the instruction
     * following these calls will be executed.
     *
     * If the requested interrupt is a context switch to a higher priority
     * task then the one currently running, we MUST NOT execute any instruction
     * before the interrupt effectively happens.
     * In order to prevent this, force this routine to have a 3-instruction
     * delay before exiting.
     */
    asm volatile ( "nop" );
    asm volatile ( "nop" );
    asm volatile ( "nop" );
}

// ------------------ Critical Sections --------------------

#if (configNUM_CORES > 1)
BaseType_t __attribute__((optimize("-O3"))) xPortEnterCriticalTimeout(portMUX_TYPE *mux, BaseType_t timeout)
{
    /* Interrupts may already be disabled (if this function is called in nested
     * manner). However, there's no atomic operation that will allow us to check,
     * thus we have to disable interrupts again anyways.
     *
     * However, if this is call is NOT nested (i.e., the first call to enter a
     * critical section), we will save the previous interrupt level so that the
     * saved level can be restored on the last call to exit the critical.
     */
    BaseType_t xOldInterruptLevel = portSET_INTERRUPT_MASK_FROM_ISR();
    if (!spinlock_acquire(mux, timeout)) {
        //Timed out attempting to get spinlock. Restore previous interrupt level and return
        portCLEAR_INTERRUPT_MASK_FROM_ISR(xOldInterruptLevel);
        return pdFAIL;
    }
    //Spinlock acquired. Increment the critical nesting count.
    BaseType_t coreID = xPortGetCoreID();
    BaseType_t newNesting = port_uxCriticalNesting[coreID] + 1;
    port_uxCriticalNesting[coreID] = newNesting;
    //If this is the first entry to a critical section. Save the old interrupt level.
    if ( newNesting == 1 ) {
        port_uxOldInterruptState[coreID] = xOldInterruptLevel;
    }
    return pdPASS;
}

void __attribute__((optimize("-O3"))) vPortExitCriticalMultiCore(portMUX_TYPE *mux)
{
    /* This function may be called in a nested manner. Therefore, we only need
     * to reenable interrupts if this is the last call to exit the critical. We
     * can use the nesting count to determine whether this is the last exit call.
     */
    spinlock_release(mux);
    BaseType_t coreID = xPortGetCoreID();
    BaseType_t nesting = port_uxCriticalNesting[coreID];

    if (nesting > 0) {
        nesting--;
        port_uxCriticalNesting[coreID] = nesting;
        //This is the last exit call, restore the saved interrupt level
        if ( nesting == 0 ) {
            portCLEAR_INTERRUPT_MASK_FROM_ISR(port_uxOldInterruptState[coreID]);
        }
    }
}


BaseType_t xPortEnterCriticalTimeoutCompliance(portMUX_TYPE *mux, BaseType_t timeout)
{
    BaseType_t ret;
    if (!xPortInIsrContext()) {
        ret = xPortEnterCriticalTimeout(mux, timeout);
    } else {
        esp_rom_printf("port*_CRITICAL called from ISR context. Aborting!\n");
        abort();
        ret = pdFAIL;
    }
    return ret;
}

void vPortExitCriticalCompliance(portMUX_TYPE *mux)
{
    if (!xPortInIsrContext()) {
        vPortExitCriticalMultiCore(mux);
    } else {
        esp_rom_printf("port*_CRITICAL called from ISR context. Aborting!\n");
        abort();
    }
}
#endif /* (configNUM_CORES > 1) */

void vPortEnterCritical(void)
{
#if (configNUM_CORES > 1)
        esp_rom_printf("vPortEnterCritical(void) is not supported on single-core targets. Please use vPortEnterCriticalMultiCore(portMUX_TYPE *mux) instead.\n");
        abort();
#endif /* (configNUM_CORES > 1) */
    BaseType_t state = portSET_INTERRUPT_MASK_FROM_ISR();
    port_uxCriticalNesting[0]++;

    if (port_uxCriticalNesting[0] == 1) {
        port_uxOldInterruptState[0] = state;
    }
}

void vPortExitCritical(void)
{
#if (configNUM_CORES > 1)
        esp_rom_printf("vPortExitCritical(void) is not supported on single-core targets. Please use vPortExitCriticalMultiCore(portMUX_TYPE *mux) instead.\n");
        abort();
#endif /* (configNUM_CORES > 1) */
    if (port_uxCriticalNesting[0] > 0) {
        port_uxCriticalNesting[0]--;
        if (port_uxCriticalNesting[0] == 0) {
            portCLEAR_INTERRUPT_MASK_FROM_ISR(port_uxOldInterruptState[0]);
        }
    }
}

// ---------------------- Yielding -------------------------

void vPortYield(void)
{
    BaseType_t coreID = xPortGetCoreID();
    int system_cpu_int_reg;

#if !CONFIG_IDF_TARGET_ESP32P4
    system_cpu_int_reg = SYSTEM_CPU_INTR_FROM_CPU_0_REG;
#else
    system_cpu_int_reg = HP_SYSTEM_CPU_INT_FROM_CPU_0_REG;
#endif /* !CONFIG_IDF_TARGET_ESP32P4 */

    if (port_uxInterruptNesting[coreID]) {
        vPortYieldFromISR();
    } else {
        esp_crosscore_int_send_yield(coreID);
        /* There are 3-4 instructions of latency between triggering the software
           interrupt and the CPU interrupt happening. Make sure it happened before
           we return, otherwise vTaskDelay() may return and execute 1-2
           instructions before the delay actually happens.

           (We could use the WFI instruction here, but there is a chance that
           the interrupt will happen while evaluating the other two conditions
           for an instant yield, and if that happens then the WFI would be
           waiting for the next interrupt to occur...)
        */
        while (port_xSchedulerRunning[coreID] && port_uxCriticalNesting[coreID] == 0 && REG_READ(system_cpu_int_reg + 4 * coreID) != 0) {}
    }
}

void vPortYieldFromISR( void )
{
    traceISR_EXIT_TO_SCHEDULER();
    BaseType_t coreID = xPortGetCoreID();
    port_xSchedulerRunning[coreID] = 1;
    xPortSwitchFlag[coreID] = 1;
}

void vPortYieldOtherCore(BaseType_t coreid)
{
    esp_crosscore_int_send_yield(coreid);
}

// ------------------- Hook Functions ----------------------

void __attribute__((weak)) vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
#define ERR_STR1 "***ERROR*** A stack overflow in task "
#define ERR_STR2 " has been detected."
    const char *str[] = {ERR_STR1, pcTaskName, ERR_STR2};

    char buf[sizeof(ERR_STR1) + CONFIG_FREERTOS_MAX_TASK_NAME_LEN + sizeof(ERR_STR2) + 1 /* null char */] = {0};

    char *dest = buf;
    for (int i = 0; i < sizeof(str) / sizeof(str[0]); i++) {
        dest = strcat(dest, str[i]);
    }
    esp_system_abort(buf);
}

// ----------------------- System --------------------------

uint32_t xPortGetTickRateHz(void)
{
    return (uint32_t)configTICK_RATE_HZ;
}

#define STACK_WATCH_AREA_SIZE 32
#define STACK_WATCH_POINT_NUMBER (SOC_CPU_WATCHPOINTS_NUM - 1)

void vPortSetStackWatchpoint(void *pxStackStart)
{
    uint32_t addr = (uint32_t)pxStackStart;
    addr = (addr + (STACK_WATCH_AREA_SIZE - 1)) & (~(STACK_WATCH_AREA_SIZE - 1));
    esp_cpu_set_watchpoint(STACK_WATCH_POINT_NUMBER, (char *)addr, STACK_WATCH_AREA_SIZE, ESP_CPU_WATCHPOINT_STORE);
}



/* ---------------------------------------------- Misc Implementations -------------------------------------------------
 *
 * ------------------------------------------------------------------------------------------------------------------ */
