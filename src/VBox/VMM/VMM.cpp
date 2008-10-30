/* $Id$ */
/** @file
 * VMM - The Virtual Machine Monitor Core.
 */

/*
 * Copyright (C) 2006-2007 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */

//#define NO_SUPCALLR0VMM

/** @page pg_vmm        VMM - The Virtual Machine Monitor
 *
 * The VMM component is two things at the moment, it's a component doing a few
 * management and routing tasks, and it's the whole virtual machine monitor
 * thing.  For hysterical reasons, it is not doing all the management that one
 * would expect, this is instead done by @ref pg_vm.  We'll address this
 * misdesign eventually.
 *
 * @see grp_vmm, grp_vm
 *
 *
 * @section sec_vmmstate        VMM State
 *
 * @image html VM_Statechart_Diagram.gif
 *
 * To be written.
 *
 *
 * @subsection  subsec_vmm_init     VMM Initialization
 *
 * To be written.
 *
 *
 * @subsection  subsec_vmm_term     VMM Termination
 *
 * To be written.
 *
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_VMM
#include <VBox/vmm.h>
#include <VBox/vmapi.h>
#include <VBox/pgm.h>
#include <VBox/cfgm.h>
#include <VBox/pdmqueue.h>
#include <VBox/pdmapi.h>
#include <VBox/cpum.h>
#include <VBox/mm.h>
#include <VBox/iom.h>
#include <VBox/trpm.h>
#include <VBox/selm.h>
#include <VBox/em.h>
#include <VBox/sup.h>
#include <VBox/dbgf.h>
#include <VBox/csam.h>
#include <VBox/patm.h>
#include <VBox/rem.h>
#include <VBox/ssm.h>
#include <VBox/tm.h>
#include "VMMInternal.h"
#include "VMMSwitcher/VMMSwitcher.h"
#include <VBox/vm.h>

#include <VBox/err.h>
#include <VBox/param.h>
#include <VBox/version.h>
#include <VBox/x86.h>
#include <VBox/hwaccm.h>
#include <iprt/assert.h>
#include <iprt/alloc.h>
#include <iprt/asm.h>
#include <iprt/time.h>
#include <iprt/stream.h>
#include <iprt/string.h>
#include <iprt/stdarg.h>
#include <iprt/ctype.h>



/** The saved state version. */
#define VMM_SAVED_STATE_VERSION     3


/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/** Array of switcher defininitions.
 * The type and index shall match!
 */
static PVMMSWITCHERDEF s_apSwitchers[VMMSWITCHER_MAX] =
{
    NULL, /* invalid entry */
#ifndef RT_ARCH_AMD64
    &vmmR3Switcher32BitTo32Bit_Def,
    &vmmR3Switcher32BitToPAE_Def,
    NULL,   //&vmmR3Switcher32BitToAMD64_Def,
    &vmmR3SwitcherPAETo32Bit_Def,
    &vmmR3SwitcherPAEToPAE_Def,
    NULL,   //&vmmR3SwitcherPAEToAMD64_Def,
# ifdef VBOX_WITH_HYBIRD_32BIT_KERNEL
    &vmmR3SwitcherAMD64ToPAE_Def,
# else
    NULL,   //&vmmR3SwitcherAMD64ToPAE_Def,
# endif
    NULL    //&vmmR3SwitcherAMD64ToAMD64_Def,
#else  /* RT_ARCH_AMD64 */
    NULL,   //&vmmR3Switcher32BitTo32Bit_Def,
    NULL,   //&vmmR3Switcher32BitToPAE_Def,
    NULL,   //&vmmR3Switcher32BitToAMD64_Def,
    NULL,   //&vmmR3SwitcherPAETo32Bit_Def,
    NULL,   //&vmmR3SwitcherPAEToPAE_Def,
    NULL,   //&vmmR3SwitcherPAEToAMD64_Def,
    &vmmR3SwitcherAMD64ToPAE_Def,
    NULL    //&vmmR3SwitcherAMD64ToAMD64_Def,
#endif /* RT_ARCH_AMD64 */
};


/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
static int                  vmmR3InitCoreCode(PVM pVM);
static DECLCALLBACK(int)    vmmR3Save(PVM pVM, PSSMHANDLE pSSM);
static DECLCALLBACK(int)    vmmR3Load(PVM pVM, PSSMHANDLE pSSM, uint32_t u32Version);
static DECLCALLBACK(void)   vmmR3YieldEMT(PVM pVM, PTMTIMER pTimer, void *pvUser);
static int                  vmmR3ServiceCallHostRequest(PVM pVM);
static DECLCALLBACK(void)   vmmR3InfoFF(PVM pVM, PCDBGFINFOHLP pHlp, const char *pszArgs);


/**
 * Initializes the VMM.
 *
 * @returns VBox status code.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) VMMR3Init(PVM pVM)
{
    LogFlow(("VMMR3Init\n"));

    /*
     * Assert alignment, sizes and order.
     */
    AssertMsg(pVM->vmm.s.offVM == 0, ("Already initialized!\n"));
    AssertMsg(sizeof(pVM->vmm.padding) >= sizeof(pVM->vmm.s),
              ("pVM->vmm.padding is too small! vmm.padding %d while vmm.s is %d\n",
               sizeof(pVM->vmm.padding), sizeof(pVM->vmm.s)));

    /*
     * Init basic VM VMM members.
     */
    pVM->vmm.s.offVM = RT_OFFSETOF(VM, vmm);
    int rc = CFGMR3QueryU32(CFGMR3GetRoot(pVM), "YieldEMTInterval", &pVM->vmm.s.cYieldEveryMillies);
    if (rc == VERR_CFGM_VALUE_NOT_FOUND)
        pVM->vmm.s.cYieldEveryMillies = 23; /* Value arrived at after experimenting with the grub boot prompt. */
        //pVM->vmm.s.cYieldEveryMillies = 8; //debugging
    else
        AssertMsgRCReturn(rc, ("Configuration error. Failed to query \"YieldEMTInterval\", rc=%Vrc\n", rc), rc);

    /* GC switchers are enabled by default. Turned off by HWACCM. */
    pVM->vmm.s.fSwitcherDisabled = false;

    /* Get the CPU count.*/
    rc = CFGMR3QueryU32Def(CFGMR3GetRoot(pVM), "NumCPUs", &pVM->cCPUs, 1);
    AssertLogRelMsgRCReturn(rc, ("Configuration error: Querying \"NumCPUs\" as integer failed, rc=%Vrc\n", rc), rc);
#ifdef VBOX_WITH_SMP_GUESTS
    AssertLogRelMsgReturn(pVM->cCPUs > 0 && pVM->cCPUs <= 256,
                          ("Configuration error: \"NumCPUs\"=%RU32 is out of range [1..256]\n", pVM->cCPUs), VERR_INVALID_PARAMETER);
#else
    AssertLogRelMsgReturn(pVM->cCPUs != 0,
                          ("Configuration error: \"NumCPUs\"=%RU32, expected 1\n", pVM->cCPUs), VERR_INVALID_PARAMETER);
#endif

#ifdef VBOX_WITH_SMP_GUESTS
    LogRel(("[SMP] VMM with %RU32 CPUs\n", pVM->cCPUs));
#endif

    /*
     * Register the saved state data unit.
     */
    rc = SSMR3RegisterInternal(pVM, "vmm", 1, VMM_SAVED_STATE_VERSION, VMM_STACK_SIZE + sizeof(RTGCPTR),
                               NULL, vmmR3Save, NULL,
                               NULL, vmmR3Load, NULL);
    if (VBOX_FAILURE(rc))
        return rc;

    /*
     * Register the Ring-0 VM handle with the session for fast ioctl calls.
     */
    rc = SUPSetVMForFastIOCtl(pVM->pVMR0);
    if (VBOX_FAILURE(rc))
        return rc;

    /*
     * Init core code.
     */
    rc = vmmR3InitCoreCode(pVM);
    if (VBOX_SUCCESS(rc))
    {
        /*
         * Allocate & init VMM GC stack.
         * The stack pages are also used by the VMM R0 when VMMR0CallHost is invoked.
         * (The page protection is modifed during R3 init completion.)
         */
#ifdef VBOX_STRICT_VMM_STACK
        rc = MMHyperAlloc(pVM, VMM_STACK_SIZE + PAGE_SIZE + PAGE_SIZE, PAGE_SIZE, MM_TAG_VMM, (void **)&pVM->vmm.s.pbHCStack);
#else
        rc = MMHyperAlloc(pVM, VMM_STACK_SIZE, PAGE_SIZE, MM_TAG_VMM, (void **)&pVM->vmm.s.pbHCStack);
#endif
        if (VBOX_SUCCESS(rc))
        {
            /* Set HC and GC stack pointers to top of stack. */
            pVM->vmm.s.CallHostR0JmpBuf.pvSavedStack = (RTR0PTR)pVM->vmm.s.pbHCStack;
            pVM->vmm.s.pbGCStack = MMHyperHC2GC(pVM, pVM->vmm.s.pbHCStack);
            pVM->vmm.s.pbGCStackBottom = pVM->vmm.s.pbGCStack + VMM_STACK_SIZE;
            AssertRelease(pVM->vmm.s.pbGCStack);

            /* Set hypervisor eip. */
            CPUMSetHyperESP(pVM, pVM->vmm.s.pbGCStack);

            /*
             * Allocate GC & R0 Logger instances (they are finalized in the relocator).
             */
#ifdef LOG_ENABLED
            PRTLOGGER pLogger = RTLogDefaultInstance();
            if (pLogger)
            {
                pVM->vmm.s.cbLoggerGC = RT_OFFSETOF(RTLOGGERRC, afGroups[pLogger->cGroups]);
                rc = MMHyperAlloc(pVM, pVM->vmm.s.cbLoggerGC, 0, MM_TAG_VMM, (void **)&pVM->vmm.s.pLoggerHC);
                if (VBOX_SUCCESS(rc))
                {
                    pVM->vmm.s.pLoggerGC = MMHyperHC2GC(pVM, pVM->vmm.s.pLoggerHC);

# ifdef VBOX_WITH_R0_LOGGING
                    rc = MMHyperAlloc(pVM, RT_OFFSETOF(VMMR0LOGGER, Logger.afGroups[pLogger->cGroups]),
                                      0, MM_TAG_VMM, (void **)&pVM->vmm.s.pR0Logger);
                    if (VBOX_SUCCESS(rc))
                    {
                        pVM->vmm.s.pR0Logger->pVM = pVM->pVMR0;
                        //pVM->vmm.s.pR0Logger->fCreated = false;
                        pVM->vmm.s.pR0Logger->cbLogger = RT_OFFSETOF(RTLOGGER, afGroups[pLogger->cGroups]);
                    }
# endif
                }
            }
#endif /* LOG_ENABLED */

#ifdef VBOX_WITH_RC_RELEASE_LOGGING
            /*
             * Allocate RC release logger instances (finalized in the relocator).
             */
            if (VBOX_SUCCESS(rc))
            {
                PRTLOGGER pRelLogger = RTLogRelDefaultInstance();
                if (pRelLogger)
                {
                    pVM->vmm.s.cbRelLoggerGC = RT_OFFSETOF(RTLOGGERRC, afGroups[pRelLogger->cGroups]);
                    rc = MMHyperAlloc(pVM, pVM->vmm.s.cbRelLoggerGC, 0, MM_TAG_VMM, (void **)&pVM->vmm.s.pRelLoggerHC);
                    if (VBOX_SUCCESS(rc))
                        pVM->vmm.s.pRelLoggerGC = MMHyperHC2GC(pVM, pVM->vmm.s.pRelLoggerHC);
                }
            }
#endif /* VBOX_WITH_RC_RELEASE_LOGGING */

#ifdef VBOX_WITH_NMI
            /*
             * Allocate mapping for the host APIC.
             */
            if (VBOX_SUCCESS(rc))
            {
                rc = MMR3HyperReserve(pVM, PAGE_SIZE, "Host APIC", &pVM->vmm.s.GCPtrApicBase);
                AssertRC(rc);
            }
#endif
            if (VBOX_SUCCESS(rc))
            {
                rc = RTCritSectInit(&pVM->vmm.s.CritSectVMLock);
                if (VBOX_SUCCESS(rc))
                {
                    /*
                     * Debug info.
                     */
                    DBGFR3InfoRegisterInternal(pVM, "ff", "Displays the current Forced actions Flags.", vmmR3InfoFF);

                    /*
                     * Statistics.
                     */
                    STAM_REG(pVM, &pVM->vmm.s.StatRunGC,                    STAMTYPE_COUNTER, "/VMM/RunGC",                     STAMUNIT_OCCURENCES, "Number of context switches.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetNormal,              STAMTYPE_COUNTER, "/VMM/GCRet/Normal",              STAMUNIT_OCCURENCES, "Number of VINF_SUCCESS returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetInterrupt,           STAMTYPE_COUNTER, "/VMM/GCRet/Interrupt",           STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_INTERRUPT returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetInterruptHyper,      STAMTYPE_COUNTER, "/VMM/GCRet/InterruptHyper",      STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_INTERRUPT_HYPER returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetGuestTrap,           STAMTYPE_COUNTER, "/VMM/GCRet/GuestTrap",           STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_GUEST_TRAP returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetRingSwitch,          STAMTYPE_COUNTER, "/VMM/GCRet/RingSwitch",          STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_RING_SWITCH returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetRingSwitchInt,       STAMTYPE_COUNTER, "/VMM/GCRet/RingSwitchInt",       STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_RING_SWITCH_INT returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetExceptionPrivilege,  STAMTYPE_COUNTER, "/VMM/GCRet/ExceptionPrivilege",  STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_EXCEPTION_PRIVILEGED returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetStaleSelector,       STAMTYPE_COUNTER, "/VMM/GCRet/StaleSelector",       STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_STALE_SELECTOR returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetIRETTrap,            STAMTYPE_COUNTER, "/VMM/GCRet/IRETTrap",            STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_IRET_TRAP returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetEmulate,             STAMTYPE_COUNTER, "/VMM/GCRet/Emulate",             STAMUNIT_OCCURENCES, "Number of VINF_EM_EXECUTE_INSTRUCTION returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPatchEmulate,        STAMTYPE_COUNTER, "/VMM/GCRet/PatchEmulate",        STAMUNIT_OCCURENCES, "Number of VINF_PATCH_EMULATE_INSTR returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetIORead,              STAMTYPE_COUNTER, "/VMM/GCRet/IORead",              STAMUNIT_OCCURENCES, "Number of VINF_IOM_HC_IOPORT_READ returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetIOWrite,             STAMTYPE_COUNTER, "/VMM/GCRet/IOWrite",             STAMUNIT_OCCURENCES, "Number of VINF_IOM_HC_IOPORT_WRITE returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetMMIORead,            STAMTYPE_COUNTER, "/VMM/GCRet/MMIORead",            STAMUNIT_OCCURENCES, "Number of VINF_IOM_HC_MMIO_READ returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetMMIOWrite,           STAMTYPE_COUNTER, "/VMM/GCRet/MMIOWrite",           STAMUNIT_OCCURENCES, "Number of VINF_IOM_HC_MMIO_WRITE returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetMMIOReadWrite,       STAMTYPE_COUNTER, "/VMM/GCRet/MMIOReadWrite",       STAMUNIT_OCCURENCES, "Number of VINF_IOM_HC_MMIO_READ_WRITE returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetMMIOPatchRead,       STAMTYPE_COUNTER, "/VMM/GCRet/MMIOPatchRead",       STAMUNIT_OCCURENCES, "Number of VINF_IOM_HC_MMIO_PATCH_READ returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetMMIOPatchWrite,      STAMTYPE_COUNTER, "/VMM/GCRet/MMIOPatchWrite",      STAMUNIT_OCCURENCES, "Number of VINF_IOM_HC_MMIO_PATCH_WRITE returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetLDTFault,            STAMTYPE_COUNTER, "/VMM/GCRet/LDTFault",            STAMUNIT_OCCURENCES, "Number of VINF_EM_EXECUTE_INSTRUCTION_GDT_FAULT returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetGDTFault,            STAMTYPE_COUNTER, "/VMM/GCRet/GDTFault",            STAMUNIT_OCCURENCES, "Number of VINF_EM_EXECUTE_INSTRUCTION_LDT_FAULT returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetIDTFault,            STAMTYPE_COUNTER, "/VMM/GCRet/IDTFault",            STAMUNIT_OCCURENCES, "Number of VINF_EM_EXECUTE_INSTRUCTION_IDT_FAULT returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetTSSFault,            STAMTYPE_COUNTER, "/VMM/GCRet/TSSFault",            STAMUNIT_OCCURENCES, "Number of VINF_EM_EXECUTE_INSTRUCTION_TSS_FAULT returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPDFault,             STAMTYPE_COUNTER, "/VMM/GCRet/PDFault",             STAMUNIT_OCCURENCES, "Number of VINF_EM_EXECUTE_INSTRUCTION_PD_FAULT returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetCSAMTask,            STAMTYPE_COUNTER, "/VMM/GCRet/CSAMTask",            STAMUNIT_OCCURENCES, "Number of VINF_CSAM_PENDING_ACTION returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetSyncCR3,             STAMTYPE_COUNTER, "/VMM/GCRet/SyncCR",              STAMUNIT_OCCURENCES, "Number of VINF_PGM_SYNC_CR3 returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetMisc,                STAMTYPE_COUNTER, "/VMM/GCRet/Misc",                STAMUNIT_OCCURENCES, "Number of misc returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPatchInt3,           STAMTYPE_COUNTER, "/VMM/GCRet/PatchInt3",           STAMUNIT_OCCURENCES, "Number of VINF_PATM_PATCH_INT3 returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPatchPF,             STAMTYPE_COUNTER, "/VMM/GCRet/PatchPF",             STAMUNIT_OCCURENCES, "Number of VINF_PATM_PATCH_TRAP_PF returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPatchGP,             STAMTYPE_COUNTER, "/VMM/GCRet/PatchGP",             STAMUNIT_OCCURENCES, "Number of VINF_PATM_PATCH_TRAP_GP returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPatchIretIRQ,        STAMTYPE_COUNTER, "/VMM/GCRet/PatchIret",           STAMUNIT_OCCURENCES, "Number of VINF_PATM_PENDING_IRQ_AFTER_IRET returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPageOverflow,        STAMTYPE_COUNTER, "/VMM/GCRet/InvlpgOverflow",      STAMUNIT_OCCURENCES, "Number of VERR_REM_FLUSHED_PAGES_OVERFLOW returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetRescheduleREM,       STAMTYPE_COUNTER, "/VMM/GCRet/ScheduleREM",         STAMUNIT_OCCURENCES, "Number of VINF_EM_RESCHEDULE_REM returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetToR3,                STAMTYPE_COUNTER, "/VMM/GCRet/ToR3",                STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_TO_R3 returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetTimerPending,        STAMTYPE_COUNTER, "/VMM/GCRet/TimerPending",        STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_TIMER_PENDING returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetInterruptPending,    STAMTYPE_COUNTER, "/VMM/GCRet/InterruptPending",    STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_INTERRUPT_PENDING returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetCallHost,            STAMTYPE_COUNTER, "/VMM/GCRet/CallHost/Misc",       STAMUNIT_OCCURENCES, "Number of VINF_VMM_CALL_HOST returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPGMGrowRAM,          STAMTYPE_COUNTER, "/VMM/GCRet/CallHost/GrowRAM",    STAMUNIT_OCCURENCES, "Number of VINF_VMM_CALL_HOST returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPDMLock,             STAMTYPE_COUNTER, "/VMM/GCRet/CallHost/PDMLock",    STAMUNIT_OCCURENCES, "Number of VINF_VMM_CALL_HOST returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetLogFlush,            STAMTYPE_COUNTER, "/VMM/GCRet/CallHost/LogFlush",   STAMUNIT_OCCURENCES, "Number of VINF_VMM_CALL_HOST returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPDMQueueFlush,       STAMTYPE_COUNTER, "/VMM/GCRet/CallHost/QueueFlush", STAMUNIT_OCCURENCES, "Number of VINF_VMM_CALL_HOST returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPGMPoolGrow,         STAMTYPE_COUNTER, "/VMM/GCRet/CallHost/PGMPoolGrow",STAMUNIT_OCCURENCES, "Number of VINF_VMM_CALL_HOST returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetRemReplay,           STAMTYPE_COUNTER, "/VMM/GCRet/CallHost/REMReplay",  STAMUNIT_OCCURENCES, "Number of VINF_VMM_CALL_HOST returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetVMSetError,          STAMTYPE_COUNTER, "/VMM/GCRet/CallHost/VMSetError", STAMUNIT_OCCURENCES, "Number of VINF_VMM_CALL_HOST returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPGMLock,             STAMTYPE_COUNTER, "/VMM/GCRet/CallHost/PGMLock",    STAMUNIT_OCCURENCES, "Number of VINF_VMM_CALL_HOST returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetHyperAssertion,      STAMTYPE_COUNTER, "/VMM/GCRet/CallHost/HyperAssert", STAMUNIT_OCCURENCES, "Number of VINF_VMM_CALL_HOST returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPATMDuplicateFn,     STAMTYPE_COUNTER, "/VMM/GCRet/PATMDuplicateFn",     STAMUNIT_OCCURENCES, "Number of VINF_PATM_DUPLICATE_FUNCTION returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPGMChangeMode,       STAMTYPE_COUNTER, "/VMM/GCRet/PGMChangeMode",       STAMUNIT_OCCURENCES, "Number of VINF_PGM_CHANGE_MODE returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetEmulHlt,             STAMTYPE_COUNTER, "/VMM/GCRet/EmulHlt",             STAMUNIT_OCCURENCES, "Number of VINF_EM_RAW_EMULATE_INSTR_HLT returns.");
                    STAM_REG(pVM, &pVM->vmm.s.StatGCRetPendingRequest,      STAMTYPE_COUNTER, "/VMM/GCRet/PendingRequest",      STAMUNIT_OCCURENCES, "Number of VINF_EM_PENDING_REQUEST returns.");

                    return VINF_SUCCESS;
                }
                AssertRC(rc);
            }
        }
        /** @todo: Need failure cleanup. */

        //more todo in here?
        //if (VBOX_SUCCESS(rc))
        //{
        //}
        //int rc2 = vmmR3TermCoreCode(pVM);
        //AssertRC(rc2));
    }

    return rc;
}


/**
 * VMMR3Init worker that initiates the core code.
 *
 * This is core per VM code which might need fixups and/or for ease of use are
 * put on linear contiguous backing.
 *
 * @returns VBox status code.
 * @param   pVM     Pointer to the shared VM structure.
 */
static int vmmR3InitCoreCode(PVM pVM)
{
    /*
     * Calc the size.
     */
    unsigned cbCoreCode = 0;
    for (unsigned iSwitcher = 0; iSwitcher < RT_ELEMENTS(s_apSwitchers); iSwitcher++)
    {
        pVM->vmm.s.aoffSwitchers[iSwitcher] = cbCoreCode;
        PVMMSWITCHERDEF pSwitcher = s_apSwitchers[iSwitcher];
        if (pSwitcher)
        {
            AssertRelease((unsigned)pSwitcher->enmType == iSwitcher);
            cbCoreCode += RT_ALIGN_32(pSwitcher->cbCode + 1, 32);
        }
    }

    /*
     * Allocate continguous pages for switchers and deal with
     * conflicts in the intermediate mapping of the code.
     */
    pVM->vmm.s.cbCoreCode = RT_ALIGN_32(cbCoreCode, PAGE_SIZE);
    pVM->vmm.s.pvHCCoreCodeR3 = SUPContAlloc2(pVM->vmm.s.cbCoreCode >> PAGE_SHIFT, &pVM->vmm.s.pvHCCoreCodeR0, &pVM->vmm.s.HCPhysCoreCode);
    int rc = VERR_NO_MEMORY;
    if (pVM->vmm.s.pvHCCoreCodeR3)
    {
        rc = PGMR3MapIntermediate(pVM, pVM->vmm.s.pvHCCoreCodeR0, pVM->vmm.s.HCPhysCoreCode, cbCoreCode);
        if (rc == VERR_PGM_INTERMEDIATE_PAGING_CONFLICT)
        {
            /* try more allocations - Solaris, Linux.  */
            const unsigned cTries = 8234;
            struct VMMInitBadTry
            {
                RTR0PTR  pvR0;
                void    *pvR3;
                RTHCPHYS HCPhys;
                RTUINT   cb;
            } *paBadTries = (struct VMMInitBadTry *)RTMemTmpAlloc(sizeof(*paBadTries) * cTries);
            AssertReturn(paBadTries, VERR_NO_TMP_MEMORY);
            unsigned i = 0;
            do
            {
                paBadTries[i].pvR3 = pVM->vmm.s.pvHCCoreCodeR3;
                paBadTries[i].pvR0 = pVM->vmm.s.pvHCCoreCodeR0;
                paBadTries[i].HCPhys = pVM->vmm.s.HCPhysCoreCode;
                i++;
                pVM->vmm.s.pvHCCoreCodeR0 = NIL_RTR0PTR;
                pVM->vmm.s.HCPhysCoreCode = NIL_RTHCPHYS;
                pVM->vmm.s.pvHCCoreCodeR3 = SUPContAlloc2(pVM->vmm.s.cbCoreCode >> PAGE_SHIFT, &pVM->vmm.s.pvHCCoreCodeR0, &pVM->vmm.s.HCPhysCoreCode);
                if (!pVM->vmm.s.pvHCCoreCodeR3)
                    break;
                rc = PGMR3MapIntermediate(pVM, pVM->vmm.s.pvHCCoreCodeR0, pVM->vmm.s.HCPhysCoreCode, cbCoreCode);
            } while (   rc == VERR_PGM_INTERMEDIATE_PAGING_CONFLICT
                     && i < cTries - 1);

            /* cleanup */
            if (VBOX_FAILURE(rc))
            {
                paBadTries[i].pvR3   = pVM->vmm.s.pvHCCoreCodeR3;
                paBadTries[i].pvR0   = pVM->vmm.s.pvHCCoreCodeR0;
                paBadTries[i].HCPhys = pVM->vmm.s.HCPhysCoreCode;
                paBadTries[i].cb     = pVM->vmm.s.cbCoreCode;
                i++;
                LogRel(("Failed to allocated and map core code: rc=%Vrc\n", rc));
            }
            while (i-- > 0)
            {
                LogRel(("Core code alloc attempt #%d: pvR3=%p pvR0=%p HCPhys=%VHp\n",
                        i, paBadTries[i].pvR3, paBadTries[i].pvR0, paBadTries[i].HCPhys));
                SUPContFree(paBadTries[i].pvR3, paBadTries[i].cb >> PAGE_SHIFT);
            }
            RTMemTmpFree(paBadTries);
        }
    }
    if (VBOX_SUCCESS(rc))
    {
        /*
         * copy the code.
         */
        for (unsigned iSwitcher = 0; iSwitcher < RT_ELEMENTS(s_apSwitchers); iSwitcher++)
        {
            PVMMSWITCHERDEF pSwitcher = s_apSwitchers[iSwitcher];
            if (pSwitcher)
                memcpy((uint8_t *)pVM->vmm.s.pvHCCoreCodeR3 + pVM->vmm.s.aoffSwitchers[iSwitcher],
                       pSwitcher->pvCode, pSwitcher->cbCode);
        }

        /*
         * Map the code into the GC address space.
         */
        RTGCPTR GCPtr;
        rc = MMR3HyperMapHCPhys(pVM, pVM->vmm.s.pvHCCoreCodeR3, pVM->vmm.s.HCPhysCoreCode, cbCoreCode, "Core Code", &GCPtr);
        if (VBOX_SUCCESS(rc))
        {
            pVM->vmm.s.pvGCCoreCode = GCPtr;
            MMR3HyperReserve(pVM, PAGE_SIZE, "fence", NULL);
            LogRel(("CoreCode: R3=%VHv R0=%VHv GC=%VRv Phys=%VHp cb=%#x\n",
                    pVM->vmm.s.pvHCCoreCodeR3, pVM->vmm.s.pvHCCoreCodeR0, pVM->vmm.s.pvGCCoreCode, pVM->vmm.s.HCPhysCoreCode, pVM->vmm.s.cbCoreCode));

            /*
             * Finally, PGM probably have selected a switcher already but we need
             * to get the routine addresses, so we'll reselect it.
             * This may legally fail so, we're ignoring the rc.
             */
            VMMR3SelectSwitcher(pVM, pVM->vmm.s.enmSwitcher);
            return rc;
        }

        /* shit */
        AssertMsgFailed(("PGMR3Map(,%VRv, %VGp, %#x, 0) failed with rc=%Vrc\n", pVM->vmm.s.pvGCCoreCode, pVM->vmm.s.HCPhysCoreCode, cbCoreCode, rc));
        SUPContFree(pVM->vmm.s.pvHCCoreCodeR3, pVM->vmm.s.cbCoreCode >> PAGE_SHIFT);
    }
    else
        VMSetError(pVM, rc, RT_SRC_POS,
                   N_("Failed to allocate %d bytes of contiguous memory for the world switcher code"),
                   cbCoreCode);

    pVM->vmm.s.pvHCCoreCodeR3 = NULL;
    pVM->vmm.s.pvHCCoreCodeR0 = NIL_RTR0PTR;
    pVM->vmm.s.pvGCCoreCode = 0;
    return rc;
}


/**
 * Ring-3 init finalizing.
 *
 * @returns VBox status code.
 * @param   pVM         The VM handle.
 */
VMMR3DECL(int) VMMR3InitFinalize(PVM pVM)
{
#ifdef VBOX_STRICT_VMM_STACK
    /*
     * Two inaccessible pages at each sides of the stack to catch over/under-flows.
     */
    memset(pVM->vmm.s.pbHCStack - PAGE_SIZE, 0xcc, PAGE_SIZE);
    PGMMapSetPage(pVM, MMHyperHC2GC(pVM, pVM->vmm.s.pbHCStack - PAGE_SIZE), PAGE_SIZE, 0);
    RTMemProtect(pVM->vmm.s.pbHCStack - PAGE_SIZE, PAGE_SIZE, RTMEM_PROT_NONE);

    memset(pVM->vmm.s.pbHCStack + VMM_STACK_SIZE, 0xcc, PAGE_SIZE);
    PGMMapSetPage(pVM, MMHyperHC2GC(pVM, pVM->vmm.s.pbHCStack + VMM_STACK_SIZE), PAGE_SIZE, 0);
    RTMemProtect(pVM->vmm.s.pbHCStack + VMM_STACK_SIZE, PAGE_SIZE, RTMEM_PROT_NONE);
#endif

    /*
     * Set page attributes to r/w for stack pages.
     */
    int rc = PGMMapSetPage(pVM, pVM->vmm.s.pbGCStack, VMM_STACK_SIZE, X86_PTE_P | X86_PTE_A | X86_PTE_D | X86_PTE_RW);
    AssertRC(rc);
    if (VBOX_SUCCESS(rc))
    {
        /*
         * Create the EMT yield timer.
         */
        rc = TMR3TimerCreateInternal(pVM, TMCLOCK_REAL, vmmR3YieldEMT, NULL, "EMT Yielder", &pVM->vmm.s.pYieldTimer);
        if (VBOX_SUCCESS(rc))
           rc = TMTimerSetMillies(pVM->vmm.s.pYieldTimer, pVM->vmm.s.cYieldEveryMillies);
    }

#ifdef VBOX_WITH_NMI
    /*
     * Map the host APIC into GC - This may be host os specific!
     */
    if (VBOX_SUCCESS(rc))
        rc = PGMMap(pVM, pVM->vmm.s.GCPtrApicBase, 0xfee00000, PAGE_SIZE,
                    X86_PTE_P | X86_PTE_RW | X86_PTE_PWT | X86_PTE_PCD | X86_PTE_A | X86_PTE_D);
#endif
    return rc;
}


/**
 * Initializes the R0 VMM.
 *
 * @returns VBox status code.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) VMMR3InitR0(PVM pVM)
{
    int rc;

    /*
     * Initialize the ring-0 logger if we haven't done so yet.
     */
    if (    pVM->vmm.s.pR0Logger
        &&  !pVM->vmm.s.pR0Logger->fCreated)
    {
        rc = VMMR3UpdateLoggers(pVM);
        if (VBOX_FAILURE(rc))
            return rc;
    }

    /*
     * Call Ring-0 entry with init code.
     */
    for (;;)
    {
#ifdef NO_SUPCALLR0VMM
        //rc = VERR_GENERAL_FAILURE;
        rc = VINF_SUCCESS;
#else
        rc = SUPCallVMMR0Ex(pVM->pVMR0, VMMR0_DO_VMMR0_INIT, VMMGetSvnRev(), NULL);
#endif
        if (    pVM->vmm.s.pR0Logger
            &&  pVM->vmm.s.pR0Logger->Logger.offScratch > 0)
            RTLogFlushToLogger(&pVM->vmm.s.pR0Logger->Logger, NULL);
        if (rc != VINF_VMM_CALL_HOST)
            break;
        rc = vmmR3ServiceCallHostRequest(pVM);
        if (VBOX_FAILURE(rc) || (rc >= VINF_EM_FIRST && rc <= VINF_EM_LAST))
            break;
        /* Resume R0 */
    }

    if (VBOX_FAILURE(rc) || (rc >= VINF_EM_FIRST && rc <= VINF_EM_LAST))
    {
        LogRel(("R0 init failed, rc=%Vra\n", rc));
        if (VBOX_SUCCESS(rc))
            rc = VERR_INTERNAL_ERROR;
    }
    return rc;
}


/**
 * Initializes the RC VMM.
 *
 * @returns VBox status code.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) VMMR3InitRC(PVM pVM)
{
    /* In VMX mode, there's no need to init GC. */
    if (pVM->vmm.s.fSwitcherDisabled)
        return VINF_SUCCESS;

    /*
     * Call VMMGCInit():
     *      -# resolve the address.
     *      -# setup stackframe and EIP to use the trampoline.
     *      -# do a generic hypervisor call.
     */
    RTGCPTR32 GCPtrEP;
    int rc = PDMR3LdrGetSymbolRC(pVM, VMMGC_MAIN_MODULE_NAME, "VMMGCEntry", &GCPtrEP);
    if (VBOX_SUCCESS(rc))
    {
        CPUMHyperSetCtxCore(pVM, NULL);
        CPUMSetHyperESP(pVM, pVM->vmm.s.pbGCStackBottom); /* Clear the stack. */
        uint64_t u64TS = RTTimeProgramStartNanoTS();
        CPUMPushHyper(pVM, (uint32_t)(u64TS >> 32));    /* Param 3: The program startup TS - Hi. */
        CPUMPushHyper(pVM, (uint32_t)u64TS);            /* Param 3: The program startup TS - Lo. */
        CPUMPushHyper(pVM, VMMGetSvnRev());             /* Param 2: Version argument. */
        CPUMPushHyper(pVM, VMMGC_DO_VMMGC_INIT);        /* Param 1: Operation. */
        CPUMPushHyper(pVM, pVM->pVMGC);                 /* Param 0: pVM */
        CPUMPushHyper(pVM, 3 * sizeof(RTGCPTR32));      /* trampoline param: stacksize.  */
        CPUMPushHyper(pVM, GCPtrEP);                    /* Call EIP. */
        CPUMSetHyperEIP(pVM, pVM->vmm.s.pfnGCCallTrampoline);

        for (;;)
        {
#ifdef NO_SUPCALLR0VMM
            //rc = VERR_GENERAL_FAILURE;
            rc = VINF_SUCCESS;
#else
            rc = SUPCallVMMR0(pVM->pVMR0, VMMR0_DO_CALL_HYPERVISOR, NULL);
#endif
#ifdef LOG_ENABLED
            PRTLOGGERRC pLogger = pVM->vmm.s.pLoggerHC;
            if (    pLogger
                &&  pLogger->offScratch > 0)
                RTLogFlushGC(NULL, pLogger);
#endif
#ifdef VBOX_WITH_RC_RELEASE_LOGGING
            PRTLOGGERRC pRelLogger = pVM->vmm.s.pRelLoggerHC;
            if (RT_UNLIKELY(pRelLogger && pRelLogger->offScratch > 0))
                RTLogFlushGC(RTLogRelDefaultInstance(), pRelLogger);
#endif
            if (rc != VINF_VMM_CALL_HOST)
                break;
            rc = vmmR3ServiceCallHostRequest(pVM);
            if (VBOX_FAILURE(rc) || (rc >= VINF_EM_FIRST && rc <= VINF_EM_LAST))
                break;
        }

        if (VBOX_FAILURE(rc) || (rc >= VINF_EM_FIRST && rc <= VINF_EM_LAST))
        {
            VMMR3FatalDump(pVM, rc);
            if (rc >= VINF_EM_FIRST && rc <= VINF_EM_LAST)
                rc = VERR_INTERNAL_ERROR;
        }
        AssertRC(rc);
    }
    return rc;
}


/**
 * Terminate the VMM bits.
 *
 * @returns VINF_SUCCESS.
 * @param   pVM         The VM handle.
 */
VMMR3DECL(int) VMMR3Term(PVM pVM)
{
    /*
     * Call Ring-0 entry with termination code.
     */
    int rc;
    for (;;)
    {
#ifdef NO_SUPCALLR0VMM
        //rc = VERR_GENERAL_FAILURE;
        rc = VINF_SUCCESS;
#else
        rc = SUPCallVMMR0Ex(pVM->pVMR0, VMMR0_DO_VMMR0_TERM, 0, NULL);
#endif
        if (    pVM->vmm.s.pR0Logger
            &&  pVM->vmm.s.pR0Logger->Logger.offScratch > 0)
            RTLogFlushToLogger(&pVM->vmm.s.pR0Logger->Logger, NULL);
        if (rc != VINF_VMM_CALL_HOST)
            break;
        rc = vmmR3ServiceCallHostRequest(pVM);
        if (VBOX_FAILURE(rc) || (rc >= VINF_EM_FIRST && rc <= VINF_EM_LAST))
            break;
        /* Resume R0 */
    }
    if (VBOX_FAILURE(rc) || (rc >= VINF_EM_FIRST && rc <= VINF_EM_LAST))
    {
        LogRel(("VMMR3Term: R0 term failed, rc=%Vra. (warning)\n", rc));
        if (VBOX_SUCCESS(rc))
            rc = VERR_INTERNAL_ERROR;
    }

#ifdef VBOX_STRICT_VMM_STACK
    /*
     * Make the two stack guard pages present again.
     */
    RTMemProtect(pVM->vmm.s.pbHCStack - PAGE_SIZE,      PAGE_SIZE, RTMEM_PROT_READ | RTMEM_PROT_WRITE);
    RTMemProtect(pVM->vmm.s.pbHCStack + VMM_STACK_SIZE, PAGE_SIZE, RTMEM_PROT_READ | RTMEM_PROT_WRITE);
#endif
    return rc;
}


/**
 * Applies relocations to data and code managed by this
 * component. This function will be called at init and
 * whenever the VMM need to relocate it self inside the GC.
 *
 * The VMM will need to apply relocations to the core code.
 *
 * @param   pVM         The VM handle.
 * @param   offDelta    The relocation delta.
 */
VMMR3DECL(void) VMMR3Relocate(PVM pVM, RTGCINTPTR offDelta)
{
    LogFlow(("VMMR3Relocate: offDelta=%VGv\n", offDelta));

    /*
     * Recalc the GC address.
     */
    pVM->vmm.s.pvGCCoreCode = MMHyperHC2GC(pVM, pVM->vmm.s.pvHCCoreCodeR3);

    /*
     * The stack.
     */
    CPUMSetHyperESP(pVM, CPUMGetHyperESP(pVM) + offDelta);
    pVM->vmm.s.pbGCStack = MMHyperHC2GC(pVM, pVM->vmm.s.pbHCStack);
    pVM->vmm.s.pbGCStackBottom = pVM->vmm.s.pbGCStack + VMM_STACK_SIZE;

    /*
     * All the switchers.
     */
    for (unsigned iSwitcher = 0; iSwitcher < RT_ELEMENTS(s_apSwitchers); iSwitcher++)
    {
        PVMMSWITCHERDEF pSwitcher = s_apSwitchers[iSwitcher];
        if (pSwitcher && pSwitcher->pfnRelocate)
        {
            unsigned off = pVM->vmm.s.aoffSwitchers[iSwitcher];
            pSwitcher->pfnRelocate(pVM,
                                   pSwitcher,
                                   (uint8_t *)pVM->vmm.s.pvHCCoreCodeR0 + off,
                                   (uint8_t *)pVM->vmm.s.pvHCCoreCodeR3 + off,
                                   pVM->vmm.s.pvGCCoreCode + off,
                                   pVM->vmm.s.HCPhysCoreCode + off);
        }
    }

    /*
     * Recalc the GC address for the current switcher.
     */
    PVMMSWITCHERDEF pSwitcher   = s_apSwitchers[pVM->vmm.s.enmSwitcher];
    RTGCPTR         GCPtr       = pVM->vmm.s.pvGCCoreCode + pVM->vmm.s.aoffSwitchers[pVM->vmm.s.enmSwitcher];
    pVM->vmm.s.pfnGCGuestToHost         = GCPtr + pSwitcher->offGCGuestToHost;
    pVM->vmm.s.pfnGCCallTrampoline      = GCPtr + pSwitcher->offGCCallTrampoline;
    pVM->pfnVMMGCGuestToHostAsm         = GCPtr + pSwitcher->offGCGuestToHostAsm;
    pVM->pfnVMMGCGuestToHostAsmHyperCtx = GCPtr + pSwitcher->offGCGuestToHostAsmHyperCtx;
    pVM->pfnVMMGCGuestToHostAsmGuestCtx = GCPtr + pSwitcher->offGCGuestToHostAsmGuestCtx;

    /*
     * Get other GC entry points.
     */
    int rc = PDMR3LdrGetSymbolRC(pVM, VMMGC_MAIN_MODULE_NAME, "CPUMGCResumeGuest", &pVM->vmm.s.pfnCPUMGCResumeGuest);
    AssertReleaseMsgRC(rc, ("CPUMGCResumeGuest not found! rc=%Vra\n", rc));

    rc = PDMR3LdrGetSymbolRC(pVM, VMMGC_MAIN_MODULE_NAME, "CPUMGCResumeGuestV86", &pVM->vmm.s.pfnCPUMGCResumeGuestV86);
    AssertReleaseMsgRC(rc, ("CPUMGCResumeGuestV86 not found! rc=%Vra\n", rc));

    /*
     * Update the logger.
     */
    VMMR3UpdateLoggers(pVM);
}


/**
 * Updates the settings for the GC and R0 loggers.
 *
 * @returns VBox status code.
 * @param   pVM     The VM handle.
 */
VMMR3DECL(int)  VMMR3UpdateLoggers(PVM pVM)
{
    /*
     * Simply clone the logger instance (for GC).
     */
    int rc = VINF_SUCCESS;
    RTGCPTR32 GCPtrLoggerFlush = 0;

    if (pVM->vmm.s.pLoggerHC
#ifdef VBOX_WITH_RC_RELEASE_LOGGING
        || pVM->vmm.s.pRelLoggerHC
#endif
       )
    {
        rc = PDMR3LdrGetSymbolRC(pVM, VMMGC_MAIN_MODULE_NAME, "vmmGCLoggerFlush", &GCPtrLoggerFlush);
        AssertReleaseMsgRC(rc, ("vmmGCLoggerFlush not found! rc=%Vra\n", rc));
    }

    if (pVM->vmm.s.pLoggerHC)
    {
        RTGCPTR32 GCPtrLoggerWrapper = 0;
        rc = PDMR3LdrGetSymbolRC(pVM, VMMGC_MAIN_MODULE_NAME, "vmmGCLoggerWrapper", &GCPtrLoggerWrapper);
        AssertReleaseMsgRC(rc, ("vmmGCLoggerWrapper not found! rc=%Vra\n", rc));
        pVM->vmm.s.pLoggerGC = MMHyperHC2GC(pVM, pVM->vmm.s.pLoggerHC);
        rc = RTLogCloneRC(NULL /* default */, pVM->vmm.s.pLoggerHC, pVM->vmm.s.cbLoggerGC,
                          GCPtrLoggerWrapper,  GCPtrLoggerFlush, RTLOGFLAGS_BUFFERED);
        AssertReleaseMsgRC(rc, ("RTLogCloneGC failed! rc=%Vra\n", rc));
    }

#ifdef VBOX_WITH_RC_RELEASE_LOGGING
    if (pVM->vmm.s.pRelLoggerHC)
    {
        RTGCPTR32 GCPtrLoggerWrapper = 0;
        rc = PDMR3LdrGetSymbolRC(pVM, VMMGC_MAIN_MODULE_NAME, "vmmGCRelLoggerWrapper", &GCPtrLoggerWrapper);
        AssertReleaseMsgRC(rc, ("vmmGCRelLoggerWrapper not found! rc=%Vra\n", rc));
        pVM->vmm.s.pRelLoggerGC = MMHyperHC2GC(pVM, pVM->vmm.s.pRelLoggerHC);
        rc = RTLogCloneRC(RTLogRelDefaultInstance(), pVM->vmm.s.pRelLoggerHC, pVM->vmm.s.cbRelLoggerGC,
                          GCPtrLoggerWrapper,  GCPtrLoggerFlush, RTLOGFLAGS_BUFFERED);
        AssertReleaseMsgRC(rc, ("RTLogCloneGC failed! rc=%Vra\n", rc));
    }
#endif /* VBOX_WITH_RC_RELEASE_LOGGING */

    /*
     * For the ring-0 EMT logger, we use a per-thread logger
     * instance in ring-0. Only initialize it once.
     */
    PVMMR0LOGGER pR0Logger = pVM->vmm.s.pR0Logger;
    if (pR0Logger)
    {
        if (!pR0Logger->fCreated)
        {
            RTR0PTR pfnLoggerWrapper = NIL_RTR0PTR;
            rc = PDMR3LdrGetSymbolR0(pVM, VMMR0_MAIN_MODULE_NAME, "vmmR0LoggerWrapper", &pfnLoggerWrapper);
            AssertReleaseMsgRCReturn(rc, ("VMMLoggerWrapper not found! rc=%Vra\n", rc), rc);

            RTR0PTR pfnLoggerFlush = NIL_RTR0PTR;
            rc = PDMR3LdrGetSymbolR0(pVM, VMMR0_MAIN_MODULE_NAME, "vmmR0LoggerFlush", &pfnLoggerFlush);
            AssertReleaseMsgRCReturn(rc, ("VMMLoggerFlush not found! rc=%Vra\n", rc), rc);

            rc = RTLogCreateForR0(&pR0Logger->Logger, pR0Logger->cbLogger,
                                  *(PFNRTLOGGER *)&pfnLoggerWrapper, *(PFNRTLOGFLUSH *)&pfnLoggerFlush,
                                  RTLOGFLAGS_BUFFERED, RTLOGDEST_DUMMY);
            AssertReleaseMsgRCReturn(rc, ("RTLogCloneGC failed! rc=%Vra\n", rc), rc);
            pR0Logger->fCreated = true;
        }

        rc = RTLogCopyGroupsAndFlags(&pR0Logger->Logger, NULL /* default */, pVM->vmm.s.pLoggerHC->fFlags, RTLOGFLAGS_BUFFERED);
        AssertRC(rc);
    }

    return rc;
}


/**
 * Generic switch code relocator.
 *
 * @param   pVM         The VM handle.
 * @param   pSwitcher   The switcher definition.
 * @param   pu8CodeR3   Pointer to the core code block for the switcher, ring-3 mapping.
 * @param   pu8CodeR0   Pointer to the core code block for the switcher, ring-0 mapping.
 * @param   GCPtrCode   The guest context address corresponding to pu8Code.
 * @param   u32IDCode   The identity mapped (ID) address corresponding to pu8Code.
 * @param   SelCS       The hypervisor CS selector.
 * @param   SelDS       The hypervisor DS selector.
 * @param   SelTSS      The hypervisor TSS selector.
 * @param   GCPtrGDT    The GC address of the hypervisor GDT.
 * @param   SelCS64     The 64-bit mode hypervisor CS selector.
 */
static void vmmR3SwitcherGenericRelocate(PVM pVM, PVMMSWITCHERDEF pSwitcher, uint8_t *pu8CodeR0, uint8_t *pu8CodeR3, RTGCPTR GCPtrCode, uint32_t u32IDCode,
                                         RTSEL SelCS, RTSEL SelDS, RTSEL SelTSS, RTGCPTR GCPtrGDT, RTSEL SelCS64)
{
    union
    {
        const uint8_t *pu8;
        const uint16_t *pu16;
        const uint32_t *pu32;
        const uint64_t *pu64;
        const void     *pv;
        uintptr_t       u;
    } u;
    u.pv = pSwitcher->pvFixups;

    /*
     * Process fixups.
     */
    uint8_t u8;
    while ((u8 = *u.pu8++) != FIX_THE_END)
    {
        /*
         * Get the source (where to write the fixup).
         */
        uint32_t offSrc = *u.pu32++;
        Assert(offSrc < pSwitcher->cbCode);
        union
        {
            uint8_t    *pu8;
            uint16_t   *pu16;
            uint32_t   *pu32;
            uint64_t   *pu64;
            uintptr_t   u;
        } uSrc;
        uSrc.pu8 = pu8CodeR3 + offSrc;

        /* The fixup target and method depends on the type. */
        switch (u8)
        {
            /*
             * 32-bit relative, source in HC and target in GC.
             */
            case FIX_HC_2_GC_NEAR_REL:
            {
                Assert(offSrc - pSwitcher->offHCCode0 < pSwitcher->cbHCCode0 || offSrc - pSwitcher->offHCCode1 < pSwitcher->cbHCCode1);
                uint32_t offTrg = *u.pu32++;
                Assert(offTrg - pSwitcher->offGCCode < pSwitcher->cbGCCode);
                *uSrc.pu32 = (uint32_t)((GCPtrCode + offTrg) - (uSrc.u + 4));
                break;
            }

            /*
             * 32-bit relative, source in HC and target in ID.
             */
            case FIX_HC_2_ID_NEAR_REL:
            {
                Assert(offSrc - pSwitcher->offHCCode0 < pSwitcher->cbHCCode0 || offSrc - pSwitcher->offHCCode1 < pSwitcher->cbHCCode1);
                uint32_t offTrg = *u.pu32++;
                Assert(offTrg - pSwitcher->offIDCode0 < pSwitcher->cbIDCode0 || offTrg - pSwitcher->offIDCode1 < pSwitcher->cbIDCode1);
                *uSrc.pu32 = (uint32_t)((u32IDCode + offTrg) - ((uintptr_t)pu8CodeR0 + offSrc + 4));
                break;
            }

            /*
             * 32-bit relative, source in GC and target in HC.
             */
            case FIX_GC_2_HC_NEAR_REL:
            {
                Assert(offSrc - pSwitcher->offGCCode < pSwitcher->cbGCCode);
                uint32_t offTrg = *u.pu32++;
                Assert(offTrg - pSwitcher->offHCCode0 < pSwitcher->cbHCCode0 || offTrg - pSwitcher->offHCCode1 < pSwitcher->cbHCCode1);
                *uSrc.pu32 = (uint32_t)(((uintptr_t)pu8CodeR0 + offTrg) - (GCPtrCode + offSrc + 4));
                break;
            }

            /*
             * 32-bit relative, source in GC and target in ID.
             */
            case FIX_GC_2_ID_NEAR_REL:
            {
                Assert(offSrc - pSwitcher->offGCCode < pSwitcher->cbGCCode);
                uint32_t offTrg = *u.pu32++;
                Assert(offTrg - pSwitcher->offIDCode0 < pSwitcher->cbIDCode0 || offTrg - pSwitcher->offIDCode1 < pSwitcher->cbIDCode1);
                *uSrc.pu32 = (uint32_t)((u32IDCode + offTrg) - (GCPtrCode + offSrc + 4));
                break;
            }

            /*
             * 32-bit relative, source in ID and target in HC.
             */
            case FIX_ID_2_HC_NEAR_REL:
            {
                Assert(offSrc - pSwitcher->offIDCode0 < pSwitcher->cbIDCode0 || offSrc - pSwitcher->offIDCode1 < pSwitcher->cbIDCode1);
                uint32_t offTrg = *u.pu32++;
                Assert(offTrg - pSwitcher->offHCCode0 < pSwitcher->cbHCCode0 || offTrg - pSwitcher->offHCCode1 < pSwitcher->cbHCCode1);
                *uSrc.pu32 = (uint32_t)(((uintptr_t)pu8CodeR0 + offTrg) - (u32IDCode + offSrc + 4));
                break;
            }

            /*
             * 32-bit relative, source in ID and target in HC.
             */
            case FIX_ID_2_GC_NEAR_REL:
            {
                Assert(offSrc - pSwitcher->offIDCode0 < pSwitcher->cbIDCode0 || offSrc - pSwitcher->offIDCode1 < pSwitcher->cbIDCode1);
                uint32_t offTrg = *u.pu32++;
                Assert(offTrg - pSwitcher->offGCCode < pSwitcher->cbGCCode);
                *uSrc.pu32 = (uint32_t)((GCPtrCode + offTrg) - (u32IDCode + offSrc + 4));
                break;
            }

            /*
             * 16:32 far jump, target in GC.
             */
            case FIX_GC_FAR32:
            {
                uint32_t offTrg = *u.pu32++;
                Assert(offTrg - pSwitcher->offGCCode < pSwitcher->cbGCCode);
                *uSrc.pu32++ = (uint32_t)(GCPtrCode + offTrg);
                *uSrc.pu16++ = SelCS;
                break;
            }

            /*
             * Make 32-bit GC pointer given CPUM offset.
             */
            case FIX_GC_CPUM_OFF:
            {
                uint32_t offCPUM = *u.pu32++;
                Assert(offCPUM < sizeof(pVM->cpum));
                *uSrc.pu32 = (uint32_t)(VM_GUEST_ADDR(pVM, &pVM->cpum) + offCPUM);
                break;
            }

            /*
             * Make 32-bit GC pointer given VM offset.
             */
            case FIX_GC_VM_OFF:
            {
                uint32_t offVM = *u.pu32++;
                Assert(offVM < sizeof(VM));
                *uSrc.pu32 = (uint32_t)(VM_GUEST_ADDR(pVM, pVM) + offVM);
                break;
            }

            /*
             * Make 32-bit HC pointer given CPUM offset.
             */
            case FIX_HC_CPUM_OFF:
            {
                uint32_t offCPUM = *u.pu32++;
                Assert(offCPUM < sizeof(pVM->cpum));
                *uSrc.pu32 = (uint32_t)pVM->pVMR0 + RT_OFFSETOF(VM, cpum) + offCPUM;
                break;
            }

            /*
             * Make 32-bit R0 pointer given VM offset.
             */
            case FIX_HC_VM_OFF:
            {
                uint32_t offVM = *u.pu32++;
                Assert(offVM < sizeof(VM));
                *uSrc.pu32 = (uint32_t)pVM->pVMR0 + offVM;
                break;
            }

            /*
             * Store the 32-Bit CR3 (32-bit) for the intermediate memory context.
             */
            case FIX_INTER_32BIT_CR3:
            {

                *uSrc.pu32 = PGMGetInter32BitCR3(pVM);
                break;
            }

            /*
             * Store the PAE CR3 (32-bit) for the intermediate memory context.
             */
            case FIX_INTER_PAE_CR3:
            {

                *uSrc.pu32 = PGMGetInterPaeCR3(pVM);
                break;
            }

            /*
             * Store the AMD64 CR3 (32-bit) for the intermediate memory context.
             */
            case FIX_INTER_AMD64_CR3:
            {

                *uSrc.pu32 = PGMGetInterAmd64CR3(pVM);
                break;
            }

            /*
             * Store the 32-Bit CR3 (32-bit) for the hypervisor (shadow) memory context.
             */
            case FIX_HYPER_32BIT_CR3:
            {

                *uSrc.pu32 = PGMGetHyper32BitCR3(pVM);
                break;
            }

            /*
             * Store the PAE CR3 (32-bit) for the hypervisor (shadow) memory context.
             */
            case FIX_HYPER_PAE_CR3:
            {

                *uSrc.pu32 = PGMGetHyperPaeCR3(pVM);
                break;
            }

            /*
             * Store the AMD64 CR3 (32-bit) for the hypervisor (shadow) memory context.
             */
            case FIX_HYPER_AMD64_CR3:
            {

                *uSrc.pu32 = PGMGetHyperAmd64CR3(pVM);
                break;
            }

            /*
             * Store Hypervisor CS (16-bit).
             */
            case FIX_HYPER_CS:
            {
                *uSrc.pu16 = SelCS;
                break;
            }

            /*
             * Store Hypervisor DS (16-bit).
             */
            case FIX_HYPER_DS:
            {
                *uSrc.pu16 = SelDS;
                break;
            }

            /*
             * Store Hypervisor TSS (16-bit).
             */
            case FIX_HYPER_TSS:
            {
                *uSrc.pu16 = SelTSS;
                break;
            }

            /*
             * Store the 32-bit GC address of the 2nd dword of the TSS descriptor (in the GDT).
             */
            case FIX_GC_TSS_GDTE_DW2:
            {
                RTGCPTR GCPtr = GCPtrGDT + (SelTSS & ~7) + 4;
                *uSrc.pu32 = (uint32_t)GCPtr;
                break;
            }


            ///@todo case FIX_CR4_MASK:
            ///@todo case FIX_CR4_OSFSXR:

            /*
             * Insert relative jump to specified target it FXSAVE/FXRSTOR isn't supported by the cpu.
             */
            case FIX_NO_FXSAVE_JMP:
            {
                uint32_t offTrg = *u.pu32++;
                Assert(offTrg < pSwitcher->cbCode);
                if (!CPUMSupportsFXSR(pVM))
                {
                    *uSrc.pu8++ = 0xe9; /* jmp rel32 */
                    *uSrc.pu32++ = offTrg - (offSrc + 5);
                }
                else
                {
                    *uSrc.pu8++ = *((uint8_t *)pSwitcher->pvCode + offSrc);
                    *uSrc.pu32++ = *(uint32_t *)((uint8_t *)pSwitcher->pvCode + offSrc + 1);
                }
                break;
            }

            /*
             * Insert relative jump to specified target it SYSENTER isn't used by the host.
             */
            case FIX_NO_SYSENTER_JMP:
            {
                uint32_t offTrg = *u.pu32++;
                Assert(offTrg < pSwitcher->cbCode);
                if (!CPUMIsHostUsingSysEnter(pVM))
                {
                    *uSrc.pu8++ = 0xe9; /* jmp rel32 */
                    *uSrc.pu32++ = offTrg - (offSrc + 5);
                }
                else
                {
                    *uSrc.pu8++ = *((uint8_t *)pSwitcher->pvCode + offSrc);
                    *uSrc.pu32++ = *(uint32_t *)((uint8_t *)pSwitcher->pvCode + offSrc + 1);
                }
                break;
            }

            /*
             * Insert relative jump to specified target it SYSENTER isn't used by the host.
             */
            case FIX_NO_SYSCALL_JMP:
            {
                uint32_t offTrg = *u.pu32++;
                Assert(offTrg < pSwitcher->cbCode);
                if (!CPUMIsHostUsingSysEnter(pVM))
                {
                    *uSrc.pu8++ = 0xe9; /* jmp rel32 */
                    *uSrc.pu32++ = offTrg - (offSrc + 5);
                }
                else
                {
                    *uSrc.pu8++ = *((uint8_t *)pSwitcher->pvCode + offSrc);
                    *uSrc.pu32++ = *(uint32_t *)((uint8_t *)pSwitcher->pvCode + offSrc + 1);
                }
                break;
            }

            /*
             * 32-bit HC pointer fixup to (HC) target within the code (32-bit offset).
             */
            case FIX_HC_32BIT:
            {
                uint32_t offTrg = *u.pu32++;
                Assert(offSrc < pSwitcher->cbCode);
                Assert(offTrg - pSwitcher->offHCCode0 < pSwitcher->cbHCCode0 || offTrg - pSwitcher->offHCCode1 < pSwitcher->cbHCCode1);
                *uSrc.pu32 = (uintptr_t)pu8CodeR0 + offTrg;
                break;
            }

#if defined(RT_ARCH_AMD64) || defined(VBOX_WITH_HYBIRD_32BIT_KERNEL)
            /*
             * 64-bit HC pointer fixup to (HC) target within the code (32-bit offset).
             */
            case FIX_HC_64BIT:
            {
                uint32_t offTrg = *u.pu32++;
                Assert(offSrc < pSwitcher->cbCode);
                Assert(offTrg - pSwitcher->offHCCode0 < pSwitcher->cbHCCode0 || offTrg - pSwitcher->offHCCode1 < pSwitcher->cbHCCode1);
                *uSrc.pu64 = (uintptr_t)pu8CodeR0 + offTrg;
                break;
            }

            /*
             * 64-bit HC Code Selector (no argument).
             */
            case FIX_HC_64BIT_CS:
            {
                Assert(offSrc < pSwitcher->cbCode);
#if defined(RT_OS_DARWIN) && defined(VBOX_WITH_HYBIRD_32BIT_KERNEL)
                *uSrc.pu16 = 0x80; /* KERNEL64_CS from i386/seg.h */
#else
                AssertFatalMsgFailed(("FIX_HC_64BIT_CS not implemented for this host\n"));
#endif
                break;
            }

            /*
             * 64-bit HC pointer to the CPUM instance data (no argument).
             */
            case FIX_HC_64BIT_CPUM:
            {
                Assert(offSrc < pSwitcher->cbCode);
                *uSrc.pu64 = pVM->pVMR0 + RT_OFFSETOF(VM, cpum);
                break;
            }
#endif

            /*
             * 32-bit ID pointer to (ID) target within the code (32-bit offset).
             */
            case FIX_ID_32BIT:
            {
                uint32_t offTrg = *u.pu32++;
                Assert(offSrc < pSwitcher->cbCode);
                Assert(offTrg - pSwitcher->offIDCode0 < pSwitcher->cbIDCode0 || offTrg - pSwitcher->offIDCode1 < pSwitcher->cbIDCode1);
                *uSrc.pu32 = u32IDCode + offTrg;
                break;
            }

            /*
             * 64-bit ID pointer to (ID) target within the code (32-bit offset).
             */
            case FIX_ID_64BIT:
            {
                uint32_t offTrg = *u.pu32++;
                Assert(offSrc < pSwitcher->cbCode);
                Assert(offTrg - pSwitcher->offIDCode0 < pSwitcher->cbIDCode0 || offTrg - pSwitcher->offIDCode1 < pSwitcher->cbIDCode1);
                *uSrc.pu64 = u32IDCode + offTrg;
                break;
            }

            /*
             * Far 16:32 ID pointer to 64-bit mode (ID) target within the code (32-bit offset).
             */
            case FIX_ID_FAR32_TO_64BIT_MODE:
            {
                uint32_t offTrg = *u.pu32++;
                Assert(offSrc < pSwitcher->cbCode);
                Assert(offTrg - pSwitcher->offIDCode0 < pSwitcher->cbIDCode0 || offTrg - pSwitcher->offIDCode1 < pSwitcher->cbIDCode1);
                *uSrc.pu32++ = u32IDCode + offTrg;
                *uSrc.pu16 = SelCS64;
                AssertRelease(SelCS64);
                break;
            }

#ifdef VBOX_WITH_NMI
            /*
             * 32-bit address to the APIC base.
             */
            case FIX_GC_APIC_BASE_32BIT:
            {
                *uSrc.pu32 = pVM->vmm.s.GCPtrApicBase;
                break;
            }
#endif

            default:
                AssertReleaseMsgFailed(("Unknown fixup %d in switcher %s\n", u8, pSwitcher->pszDesc));
                break;
        }
    }

#ifdef LOG_ENABLED
    /*
     * If Log2 is enabled disassemble the switcher code.
     *
     * The switcher code have 1-2 HC parts, 1 GC part and 0-2 ID parts.
     */
    if (LogIs2Enabled())
    {
        RTLogPrintf("*** Disassembly of switcher %d '%s' %#x bytes ***\n"
                    "   pu8CodeR0   = %p\n"
                    "   pu8CodeR3   = %p\n"
                    "   GCPtrCode   = %VGv\n"
                    "   u32IDCode   = %08x\n"
                    "   pVMGC       = %VGv\n"
                    "   pCPUMGC     = %VGv\n"
                    "   pVMHC       = %p\n"
                    "   pCPUMHC     = %p\n"
                    "   GCPtrGDT    = %VGv\n"
                    "   InterCR3s   = %08x, %08x, %08x (32-Bit, PAE, AMD64)\n"
                    "   HyperCR3s   = %08x, %08x, %08x (32-Bit, PAE, AMD64)\n"
                    "   SelCS       = %04x\n"
                    "   SelDS       = %04x\n"
                    "   SelCS64     = %04x\n"
                    "   SelTSS      = %04x\n",
                    pSwitcher->enmType, pSwitcher->pszDesc, pSwitcher->cbCode,
                    pu8CodeR0, pu8CodeR3, GCPtrCode, u32IDCode, VM_GUEST_ADDR(pVM, pVM),
                    VM_GUEST_ADDR(pVM, &pVM->cpum), pVM, &pVM->cpum,
                    GCPtrGDT,
                    PGMGetHyper32BitCR3(pVM), PGMGetHyperPaeCR3(pVM), PGMGetHyperAmd64CR3(pVM),
                    PGMGetInter32BitCR3(pVM), PGMGetInterPaeCR3(pVM), PGMGetInterAmd64CR3(pVM),
                    SelCS, SelDS, SelCS64, SelTSS);

        uint32_t offCode = 0;
        while (offCode < pSwitcher->cbCode)
        {
            /*
             * Figure out where this is.
             */
            const char *pszDesc = NULL;
            RTUINTPTR   uBase;
            uint32_t    cbCode;
            if (offCode - pSwitcher->offHCCode0 < pSwitcher->cbHCCode0)
            {
                pszDesc = "HCCode0";
                uBase   = (RTUINTPTR)pu8CodeR0;
                offCode = pSwitcher->offHCCode0;
                cbCode  = pSwitcher->cbHCCode0;
            }
            else if (offCode - pSwitcher->offHCCode1 < pSwitcher->cbHCCode1)
            {
                pszDesc = "HCCode1";
                uBase   = (RTUINTPTR)pu8CodeR0;
                offCode = pSwitcher->offHCCode1;
                cbCode  = pSwitcher->cbHCCode1;
            }
            else if (offCode - pSwitcher->offGCCode < pSwitcher->cbGCCode)
            {
                pszDesc = "GCCode";
                uBase   = GCPtrCode;
                offCode = pSwitcher->offGCCode;
                cbCode  = pSwitcher->cbGCCode;
            }
            else if (offCode - pSwitcher->offIDCode0 < pSwitcher->cbIDCode0)
            {
                pszDesc = "IDCode0";
                uBase   = u32IDCode;
                offCode = pSwitcher->offIDCode0;
                cbCode  = pSwitcher->cbIDCode0;
            }
            else if (offCode - pSwitcher->offIDCode1 < pSwitcher->cbIDCode1)
            {
                pszDesc = "IDCode1";
                uBase   = u32IDCode;
                offCode = pSwitcher->offIDCode1;
                cbCode  = pSwitcher->cbIDCode1;
            }
            else
            {
                RTLogPrintf("  %04x: %02x '%c' (nowhere)\n",
                            offCode, pu8CodeR3[offCode], isprint(pu8CodeR3[offCode]) ? pu8CodeR3[offCode] : ' ');
                offCode++;
                continue;
            }

            /*
             * Disassemble it.
             */
            RTLogPrintf("  %s: offCode=%#x cbCode=%#x\n", pszDesc, offCode, cbCode);
            DISCPUSTATE Cpu;

            memset(&Cpu, 0, sizeof(Cpu));
            Cpu.mode = CPUMODE_32BIT;
            while (cbCode > 0)
            {
                /* try label it */
                if (pSwitcher->offR0HostToGuest == offCode)
                    RTLogPrintf(" *R0HostToGuest:\n");
                if (pSwitcher->offGCGuestToHost == offCode)
                    RTLogPrintf(" *GCGuestToHost:\n");
                if (pSwitcher->offGCCallTrampoline == offCode)
                    RTLogPrintf(" *GCCallTrampoline:\n");
                if (pSwitcher->offGCGuestToHostAsm == offCode)
                    RTLogPrintf(" *GCGuestToHostAsm:\n");
                if (pSwitcher->offGCGuestToHostAsmHyperCtx == offCode)
                    RTLogPrintf(" *GCGuestToHostAsmHyperCtx:\n");
                if (pSwitcher->offGCGuestToHostAsmGuestCtx == offCode)
                    RTLogPrintf(" *GCGuestToHostAsmGuestCtx:\n");

                /* disas */
                uint32_t cbInstr = 0;
                char szDisas[256];
                if (RT_SUCCESS(DISInstr(&Cpu, (RTUINTPTR)pu8CodeR3 + offCode, uBase - (RTUINTPTR)pu8CodeR3, &cbInstr, szDisas)))
                    RTLogPrintf("  %04x: %s", offCode, szDisas); //for whatever reason szDisas includes '\n'.
                else
                {
                    RTLogPrintf("  %04x: %02x '%c'\n",
                                offCode, pu8CodeR3[offCode], isprint(pu8CodeR3[offCode]) ? pu8CodeR3[offCode] : ' ');
                    cbInstr = 1;
                }
                offCode += cbInstr;
                cbCode -= RT_MIN(cbInstr, cbCode);
            }
        }
    }
#endif
}


/**
 * Relocator for the 32-Bit to 32-Bit world switcher.
 */
DECLCALLBACK(void) vmmR3Switcher32BitTo32Bit_Relocate(PVM pVM, PVMMSWITCHERDEF pSwitcher, uint8_t *pu8CodeR0, uint8_t *pu8CodeR3, RTGCPTR GCPtrCode, uint32_t u32IDCode)
{
    vmmR3SwitcherGenericRelocate(pVM, pSwitcher, pu8CodeR0, pu8CodeR3, GCPtrCode, u32IDCode,
                                 SELMGetHyperCS(pVM), SELMGetHyperDS(pVM), SELMGetHyperTSS(pVM), SELMGetHyperGDT(pVM), 0);
}


/**
 * Relocator for the 32-Bit to PAE world switcher.
 */
DECLCALLBACK(void) vmmR3Switcher32BitToPAE_Relocate(PVM pVM, PVMMSWITCHERDEF pSwitcher, uint8_t *pu8CodeR0, uint8_t *pu8CodeR3, RTGCPTR GCPtrCode, uint32_t u32IDCode)
{
    vmmR3SwitcherGenericRelocate(pVM, pSwitcher, pu8CodeR0, pu8CodeR3, GCPtrCode, u32IDCode,
                                 SELMGetHyperCS(pVM), SELMGetHyperDS(pVM), SELMGetHyperTSS(pVM), SELMGetHyperGDT(pVM), 0);
}


/**
 * Relocator for the PAE to 32-Bit world switcher.
 */
DECLCALLBACK(void) vmmR3SwitcherPAETo32Bit_Relocate(PVM pVM, PVMMSWITCHERDEF pSwitcher, uint8_t *pu8CodeR0, uint8_t *pu8CodeR3, RTGCPTR GCPtrCode, uint32_t u32IDCode)
{
    vmmR3SwitcherGenericRelocate(pVM, pSwitcher, pu8CodeR0, pu8CodeR3, GCPtrCode, u32IDCode,
                                 SELMGetHyperCS(pVM), SELMGetHyperDS(pVM), SELMGetHyperTSS(pVM), SELMGetHyperGDT(pVM), 0);
}


/**
 * Relocator for the PAE to PAE world switcher.
 */
DECLCALLBACK(void) vmmR3SwitcherPAEToPAE_Relocate(PVM pVM, PVMMSWITCHERDEF pSwitcher, uint8_t *pu8CodeR0, uint8_t *pu8CodeR3, RTGCPTR GCPtrCode, uint32_t u32IDCode)
{
    vmmR3SwitcherGenericRelocate(pVM, pSwitcher, pu8CodeR0, pu8CodeR3, GCPtrCode, u32IDCode,
                                 SELMGetHyperCS(pVM), SELMGetHyperDS(pVM), SELMGetHyperTSS(pVM), SELMGetHyperGDT(pVM), 0);
}


/**
 * Relocator for the AMD64 to PAE world switcher.
 */
DECLCALLBACK(void) vmmR3SwitcherAMD64ToPAE_Relocate(PVM pVM, PVMMSWITCHERDEF pSwitcher, uint8_t *pu8CodeR0, uint8_t *pu8CodeR3, RTGCPTR GCPtrCode, uint32_t u32IDCode)
{
    vmmR3SwitcherGenericRelocate(pVM, pSwitcher, pu8CodeR0, pu8CodeR3, GCPtrCode, u32IDCode,
                                 SELMGetHyperCS(pVM), SELMGetHyperDS(pVM), SELMGetHyperTSS(pVM), SELMGetHyperGDT(pVM), SELMGetHyperCS64(pVM));
}


/**
 * Gets the pointer to g_szRTAssertMsg1 in GC.
 * @returns Pointer to VMMGC::g_szRTAssertMsg1.
 *          Returns NULL if not present.
 * @param   pVM         The VM handle.
 */
VMMR3DECL(const char *) VMMR3GetGCAssertMsg1(PVM pVM)
{
    RTGCPTR32 GCPtr;
    int rc = PDMR3LdrGetSymbolRC(pVM, NULL, "g_szRTAssertMsg1", &GCPtr);
    if (VBOX_SUCCESS(rc))
        return (const char *)MMHyperGC2HC(pVM, GCPtr);
    return NULL;
}


/**
 * Gets the pointer to g_szRTAssertMsg2 in GC.
 * @returns Pointer to VMMGC::g_szRTAssertMsg2.
 *          Returns NULL if not present.
 * @param   pVM         The VM handle.
 */
VMMR3DECL(const char *) VMMR3GetGCAssertMsg2(PVM pVM)
{
    RTGCPTR32 GCPtr;
    int rc = PDMR3LdrGetSymbolRC(pVM, NULL, "g_szRTAssertMsg2", &GCPtr);
    if (VBOX_SUCCESS(rc))
        return (const char *)MMHyperGC2HC(pVM, GCPtr);
    return NULL;
}


/**
 * Execute state save operation.
 *
 * @returns VBox status code.
 * @param   pVM             VM Handle.
 * @param   pSSM            SSM operation handle.
 */
static DECLCALLBACK(int) vmmR3Save(PVM pVM, PSSMHANDLE pSSM)
{
    LogFlow(("vmmR3Save:\n"));

    /*
     * The hypervisor stack.
     */
    SSMR3PutRCPtr(pSSM, pVM->vmm.s.pbGCStackBottom);
    RTRCPTR GCPtrESP = CPUMGetHyperESP(pVM);
    AssertMsg(pVM->vmm.s.pbGCStackBottom - GCPtrESP <= VMM_STACK_SIZE, ("Bottom %VGv ESP=%VGv\n", pVM->vmm.s.pbGCStackBottom, GCPtrESP));
    SSMR3PutRCPtr(pSSM, GCPtrESP);
    SSMR3PutMem(pSSM, pVM->vmm.s.pbHCStack, VMM_STACK_SIZE);
    return SSMR3PutU32(pSSM, ~0); /* terminator */
}


/**
 * Execute state load operation.
 *
 * @returns VBox status code.
 * @param   pVM             VM Handle.
 * @param   pSSM            SSM operation handle.
 * @param   u32Version      Data layout version.
 */
static DECLCALLBACK(int) vmmR3Load(PVM pVM, PSSMHANDLE pSSM, uint32_t u32Version)
{
    LogFlow(("vmmR3Load:\n"));

    /*
     * Validate version.
     */
    if (u32Version != VMM_SAVED_STATE_VERSION)
    {
        AssertMsgFailed(("vmmR3Load: Invalid version u32Version=%d!\n", u32Version));
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;
    }

    /*
     * Check that the stack is in the same place, or that it's fearly empty.
     */
    RTRCPTR GCPtrStackBottom;
    SSMR3GetRCPtr(pSSM, &GCPtrStackBottom);
    RTRCPTR GCPtrESP;
    int rc = SSMR3GetRCPtr(pSSM, &GCPtrESP);
    if (VBOX_FAILURE(rc))
        return rc;

    /* Previously we checked if the location of the stack was identical or that the stack was empty.
     * This is not required as we can never initiate a save when GC context code performs a ring 3 call.
     */
    /* restore the stack. (not necessary; just consistency checking) */
    SSMR3GetMem(pSSM, pVM->vmm.s.pbHCStack, VMM_STACK_SIZE);

    /* terminator */
    uint32_t u32;
    rc = SSMR3GetU32(pSSM, &u32);
    if (VBOX_FAILURE(rc))
        return rc;
    if (u32 != ~0U)
    {
        AssertMsgFailed(("u32=%#x\n", u32));
        return VERR_SSM_DATA_UNIT_FORMAT_CHANGED;
    }
    return VINF_SUCCESS;
}


/**
 * Selects the switcher to be used for switching to GC.
 *
 * @returns VBox status code.
 * @param   pVM             VM handle.
 * @param   enmSwitcher     The new switcher.
 * @remark  This function may be called before the VMM is initialized.
 */
VMMR3DECL(int) VMMR3SelectSwitcher(PVM pVM, VMMSWITCHER enmSwitcher)
{
    /*
     * Validate input.
     */
    if (    enmSwitcher < VMMSWITCHER_INVALID
        ||  enmSwitcher >= VMMSWITCHER_MAX)
    {
        AssertMsgFailed(("Invalid input enmSwitcher=%d\n", enmSwitcher));
        return VERR_INVALID_PARAMETER;
    }

    /* Do nothing if the switcher is disabled. */
    if (pVM->vmm.s.fSwitcherDisabled)
        return VINF_SUCCESS;

    /*
     * Select the new switcher.
     */
    PVMMSWITCHERDEF pSwitcher = s_apSwitchers[enmSwitcher];
    if (pSwitcher)
    {
        Log(("VMMR3SelectSwitcher: enmSwitcher %d -> %d %s\n", pVM->vmm.s.enmSwitcher, enmSwitcher, pSwitcher->pszDesc));
        pVM->vmm.s.enmSwitcher = enmSwitcher;

        RTR0PTR     pbCodeR0 = (RTR0PTR)pVM->vmm.s.pvHCCoreCodeR0 + pVM->vmm.s.aoffSwitchers[enmSwitcher]; /** @todo fix the pvHCCoreCodeR0 type */
        pVM->vmm.s.pfnR0HostToGuest = pbCodeR0 + pSwitcher->offR0HostToGuest;

        RTGCPTR     GCPtr = pVM->vmm.s.pvGCCoreCode + pVM->vmm.s.aoffSwitchers[enmSwitcher];
        pVM->vmm.s.pfnGCGuestToHost         = GCPtr + pSwitcher->offGCGuestToHost;
        pVM->vmm.s.pfnGCCallTrampoline      = GCPtr + pSwitcher->offGCCallTrampoline;
        pVM->pfnVMMGCGuestToHostAsm         = GCPtr + pSwitcher->offGCGuestToHostAsm;
        pVM->pfnVMMGCGuestToHostAsmHyperCtx = GCPtr + pSwitcher->offGCGuestToHostAsmHyperCtx;
        pVM->pfnVMMGCGuestToHostAsmGuestCtx = GCPtr + pSwitcher->offGCGuestToHostAsmGuestCtx;
        return VINF_SUCCESS;
    }
    return VERR_NOT_IMPLEMENTED;
}

/**
 * Disable the switcher logic permanently.
 *
 * @returns VBox status code.
 * @param   pVM             VM handle.
 */
VMMR3DECL(int) VMMR3DisableSwitcher(PVM pVM)
{
/** @todo r=bird: I would suggest that we create a dummy switcher which just does something like:
 * @code
 *       mov eax, VERR_INTERNAL_ERROR
 *       ret
 * @endcode
 * And then check for fSwitcherDisabled in VMMR3SelectSwitcher() in order to prevent it from being removed.
 */
    pVM->vmm.s.fSwitcherDisabled = true;
    return VINF_SUCCESS;
}


/**
 * Resolve a builtin GC symbol.
 * Called by PDM when loading or relocating GC modules.
 *
 * @returns VBox status
 * @param   pVM         VM Handle.
 * @param   pszSymbol   Symbol to resolv
 * @param   pGCPtrValue Where to store the symbol value.
 * @remark  This has to work before VMMR3Relocate() is called.
 */
VMMR3DECL(int) VMMR3GetImportGC(PVM pVM, const char *pszSymbol, PRTGCPTR pGCPtrValue)
{
    if (!strcmp(pszSymbol, "g_Logger"))
    {
        if (pVM->vmm.s.pLoggerHC)
            pVM->vmm.s.pLoggerGC = MMHyperHC2GC(pVM, pVM->vmm.s.pLoggerHC);
        *pGCPtrValue = pVM->vmm.s.pLoggerGC;
    }
    else if (!strcmp(pszSymbol, "g_RelLogger"))
    {
#ifdef VBOX_WITH_RC_RELEASE_LOGGING
        if (pVM->vmm.s.pRelLoggerHC)
            pVM->vmm.s.pRelLoggerGC = MMHyperHC2GC(pVM, pVM->vmm.s.pRelLoggerHC);
        *pGCPtrValue = pVM->vmm.s.pRelLoggerGC;
#else
        *pGCPtrValue = NIL_RTGCPTR;
#endif
    }
    else
        return VERR_SYMBOL_NOT_FOUND;
    return VINF_SUCCESS;
}


/**
 * Suspends the the CPU yielder.
 *
 * @param   pVM             The VM handle.
 */
VMMR3DECL(void) VMMR3YieldSuspend(PVM pVM)
{
    if (!pVM->vmm.s.cYieldResumeMillies)
    {
        uint64_t u64Now = TMTimerGet(pVM->vmm.s.pYieldTimer);
        uint64_t u64Expire = TMTimerGetExpire(pVM->vmm.s.pYieldTimer);
        if (u64Now >= u64Expire || u64Expire == ~(uint64_t)0)
            pVM->vmm.s.cYieldResumeMillies = pVM->vmm.s.cYieldEveryMillies;
        else
            pVM->vmm.s.cYieldResumeMillies = TMTimerToMilli(pVM->vmm.s.pYieldTimer, u64Expire - u64Now);
        TMTimerStop(pVM->vmm.s.pYieldTimer);
    }
    pVM->vmm.s.u64LastYield = RTTimeNanoTS();
}


/**
 * Stops the the CPU yielder.
 *
 * @param   pVM             The VM handle.
 */
VMMR3DECL(void) VMMR3YieldStop(PVM pVM)
{
    if (!pVM->vmm.s.cYieldResumeMillies)
        TMTimerStop(pVM->vmm.s.pYieldTimer);
    pVM->vmm.s.cYieldResumeMillies = pVM->vmm.s.cYieldEveryMillies;
    pVM->vmm.s.u64LastYield = RTTimeNanoTS();
}


/**
 * Resumes the CPU yielder when it has been a suspended or stopped.
 *
 * @param   pVM             The VM handle.
 */
VMMR3DECL(void) VMMR3YieldResume(PVM pVM)
{
    if (pVM->vmm.s.cYieldResumeMillies)
    {
        TMTimerSetMillies(pVM->vmm.s.pYieldTimer, pVM->vmm.s.cYieldResumeMillies);
        pVM->vmm.s.cYieldResumeMillies = 0;
    }
}


/**
 * Internal timer callback function.
 *
 * @param   pVM             The VM.
 * @param   pTimer          The timer handle.
 * @param   pvUser          User argument specified upon timer creation.
 */
static DECLCALLBACK(void) vmmR3YieldEMT(PVM pVM, PTMTIMER pTimer, void *pvUser)
{
    /*
     * This really needs some careful tuning. While we shouldn't be too gready since
     * that'll cause the rest of the system to stop up, we shouldn't be too nice either
     * because that'll cause us to stop up.
     *
     * The current logic is to use the default interval when there is no lag worth
     * mentioning, but when we start accumulating lag we don't bother yielding at all.
     *
     * (This depends on the TMCLOCK_VIRTUAL_SYNC to be scheduled before TMCLOCK_REAL
     * so the lag is up to date.)
     */
    const uint64_t u64Lag = TMVirtualSyncGetLag(pVM);
    if (    u64Lag     <   50000000 /* 50ms */
        ||  (   u64Lag < 1000000000 /*  1s */
             && RTTimeNanoTS() - pVM->vmm.s.u64LastYield < 500000000 /* 500 ms */)
       )
    {
        uint64_t u64Elapsed = RTTimeNanoTS();
        pVM->vmm.s.u64LastYield = u64Elapsed;

        RTThreadYield();

#ifdef LOG_ENABLED
        u64Elapsed = RTTimeNanoTS() - u64Elapsed;
        Log(("vmmR3YieldEMT: %RI64 ns\n", u64Elapsed));
#endif
    }
    TMTimerSetMillies(pTimer, pVM->vmm.s.cYieldEveryMillies);
}


/**
 * Acquire global VM lock.
 *
 * @returns VBox status code
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) VMMR3Lock(PVM pVM)
{
    return RTCritSectEnter(&pVM->vmm.s.CritSectVMLock);
}


/**
 * Release global VM lock.
 *
 * @returns VBox status code
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(int) VMMR3Unlock(PVM pVM)
{
    return RTCritSectLeave(&pVM->vmm.s.CritSectVMLock);
}


/**
 * Return global VM lock owner.
 *
 * @returns Thread id of owner.
 * @returns NIL_RTTHREAD if no owner.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(RTNATIVETHREAD) VMMR3LockGetOwner(PVM pVM)
{
    return RTCritSectGetOwner(&pVM->vmm.s.CritSectVMLock);
}


/**
 * Checks if the current thread is the owner of the global VM lock.
 *
 * @returns true if owner.
 * @returns false if not owner.
 * @param   pVM         The VM to operate on.
 */
VMMR3DECL(bool) VMMR3LockIsOwner(PVM pVM)
{
    return RTCritSectIsOwner(&pVM->vmm.s.CritSectVMLock);
}


/**
 * Executes guest code.
 *
 * @param   pVM         VM handle.
 */
VMMR3DECL(int) VMMR3RawRunGC(PVM pVM)
{
    Log2(("VMMR3RawRunGC: (cs:eip=%04x:%08x)\n", CPUMGetGuestCS(pVM), CPUMGetGuestEIP(pVM)));

    /*
     * Set the EIP and ESP.
     */
    CPUMSetHyperEIP(pVM, CPUMGetGuestEFlags(pVM) & X86_EFL_VM
                    ? pVM->vmm.s.pfnCPUMGCResumeGuestV86
                    : pVM->vmm.s.pfnCPUMGCResumeGuest);
    CPUMSetHyperESP(pVM, pVM->vmm.s.pbGCStackBottom);

    /*
     * We hide log flushes (outer) and hypervisor interrupts (inner).
     */
    for (;;)
    {
        int rc;
        do
        {
#ifdef NO_SUPCALLR0VMM
            rc = VERR_GENERAL_FAILURE;
#else
            rc = SUPCallVMMR0Fast(pVM->pVMR0, VMMR0_DO_RAW_RUN);
            if (RT_LIKELY(rc == VINF_SUCCESS))
                rc = pVM->vmm.s.iLastGCRc;
#endif
        } while (rc == VINF_EM_RAW_INTERRUPT_HYPER);

        /*
         * Flush the logs.
         */
#ifdef LOG_ENABLED
        PRTLOGGERRC pLogger = pVM->vmm.s.pLoggerHC;
        if (    pLogger
            &&  pLogger->offScratch > 0)
            RTLogFlushGC(NULL, pLogger);
#endif
#ifdef VBOX_WITH_RC_RELEASE_LOGGING
        PRTLOGGERRC pRelLogger = pVM->vmm.s.pRelLoggerHC;
        if (RT_UNLIKELY(pRelLogger && pRelLogger->offScratch > 0))
            RTLogFlushGC(RTLogRelDefaultInstance(), pRelLogger);
#endif
        if (rc != VINF_VMM_CALL_HOST)
        {
            Log2(("VMMR3RawRunGC: returns %Vrc (cs:eip=%04x:%08x)\n", rc, CPUMGetGuestCS(pVM), CPUMGetGuestEIP(pVM)));
            return rc;
        }
        rc = vmmR3ServiceCallHostRequest(pVM);
        if (VBOX_FAILURE(rc))
            return rc;
        /* Resume GC */
    }
}


/**
 * Executes guest code (Intel VT-x and AMD-V).
 *
 * @param   pVM         VM handle.
 */
VMMR3DECL(int) VMMR3HwAccRunGC(PVM pVM)
{
    Log2(("VMMR3HwAccRunGC: (cs:eip=%04x:%08x)\n", CPUMGetGuestCS(pVM), CPUMGetGuestEIP(pVM)));

    for (;;)
    {
        int rc;
        do
        {
#ifdef NO_SUPCALLR0VMM
            rc = VERR_GENERAL_FAILURE;
#else
            rc = SUPCallVMMR0Fast(pVM->pVMR0, VMMR0_DO_HWACC_RUN);
            if (RT_LIKELY(rc == VINF_SUCCESS))
                rc = pVM->vmm.s.iLastGCRc;
#endif
        } while (rc == VINF_EM_RAW_INTERRUPT_HYPER);

#ifdef LOG_ENABLED
        /*
         * Flush the log
         */
        PVMMR0LOGGER pR0Logger = pVM->vmm.s.pR0Logger;
        if (    pR0Logger
            &&  pR0Logger->Logger.offScratch > 0)
            RTLogFlushToLogger(&pR0Logger->Logger, NULL);
#endif /* !LOG_ENABLED */
        if (rc != VINF_VMM_CALL_HOST)
        {
            Log2(("VMMR3HwAccRunGC: returns %Vrc (cs:eip=%04x:%08x)\n", rc, CPUMGetGuestCS(pVM), CPUMGetGuestEIP(pVM)));
            return rc;
        }
        rc = vmmR3ServiceCallHostRequest(pVM);
        if (VBOX_FAILURE(rc) || rc == VINF_EM_DBG_HYPER_ASSERTION)
            return rc;
        /* Resume R0 */
    }
}

/**
 * Calls GC a function.
 *
 * @param   pVM         The VM handle.
 * @param   GCPtrEntry  The GC function address.
 * @param   cArgs       The number of arguments in the ....
 * @param   ...         Arguments to the function.
 */
VMMR3DECL(int) VMMR3CallGC(PVM pVM, RTRCPTR GCPtrEntry, unsigned cArgs, ...)
{
    va_list args;
    va_start(args, cArgs);
    int rc = VMMR3CallGCV(pVM, GCPtrEntry, cArgs, args);
    va_end(args);
    return rc;
}


/**
 * Calls GC a function.
 *
 * @param   pVM         The VM handle.
 * @param   GCPtrEntry  The GC function address.
 * @param   cArgs       The number of arguments in the ....
 * @param   args        Arguments to the function.
 */
VMMR3DECL(int) VMMR3CallGCV(PVM pVM, RTRCPTR GCPtrEntry, unsigned cArgs, va_list args)
{
    Log2(("VMMR3CallGCV: GCPtrEntry=%VRv cArgs=%d\n", GCPtrEntry, cArgs));

    /*
     * Setup the call frame using the trampoline.
     */
    CPUMHyperSetCtxCore(pVM, NULL);
    memset(pVM->vmm.s.pbHCStack, 0xaa, VMM_STACK_SIZE); /* Clear the stack. */
    CPUMSetHyperESP(pVM, pVM->vmm.s.pbGCStackBottom - cArgs * sizeof(RTGCUINTPTR32));
    PRTGCUINTPTR32 pFrame = (PRTGCUINTPTR32)(pVM->vmm.s.pbHCStack + VMM_STACK_SIZE) - cArgs;
    int i = cArgs;
    while (i-- > 0)
        *pFrame++ = va_arg(args, RTGCUINTPTR32);

    CPUMPushHyper(pVM, cArgs * sizeof(RTGCUINTPTR32));                          /* stack frame size */
    CPUMPushHyper(pVM, GCPtrEntry);                                             /* what to call */
    CPUMSetHyperEIP(pVM, pVM->vmm.s.pfnGCCallTrampoline);

    /*
     * We hide log flushes (outer) and hypervisor interrupts (inner).
     */
    for (;;)
    {
        int rc;
        do
        {
#ifdef NO_SUPCALLR0VMM
            rc = VERR_GENERAL_FAILURE;
#else
            rc = SUPCallVMMR0Fast(pVM->pVMR0, VMMR0_DO_RAW_RUN);
            if (RT_LIKELY(rc == VINF_SUCCESS))
                rc = pVM->vmm.s.iLastGCRc;
#endif
        } while (rc == VINF_EM_RAW_INTERRUPT_HYPER);

        /*
         * Flush the logs.
         */
#ifdef LOG_ENABLED
        PRTLOGGERRC pLogger = pVM->vmm.s.pLoggerHC;
        if (    pLogger
            &&  pLogger->offScratch > 0)
            RTLogFlushGC(NULL, pLogger);
#endif
#ifdef VBOX_WITH_RC_RELEASE_LOGGING
        PRTLOGGERRC pRelLogger = pVM->vmm.s.pRelLoggerHC;
        if (RT_UNLIKELY(pRelLogger && pRelLogger->offScratch > 0))
            RTLogFlushGC(RTLogRelDefaultInstance(), pRelLogger);
#endif
        if (rc == VERR_TRPM_PANIC || rc == VERR_TRPM_DONT_PANIC)
            VMMR3FatalDump(pVM, rc);
        if (rc != VINF_VMM_CALL_HOST)
        {
            Log2(("VMMR3CallGCV: returns %Vrc (cs:eip=%04x:%08x)\n", rc, CPUMGetGuestCS(pVM), CPUMGetGuestEIP(pVM)));
            return rc;
        }
        rc = vmmR3ServiceCallHostRequest(pVM);
        if (VBOX_FAILURE(rc))
            return rc;
    }
}


/**
 * Resumes executing hypervisor code when interrupted
 * by a queue flush or a debug event.
 *
 * @returns VBox status code.
 * @param   pVM         VM handle.
 */
VMMR3DECL(int) VMMR3ResumeHyper(PVM pVM)
{
    Log(("VMMR3ResumeHyper: eip=%VGv esp=%VGv\n", CPUMGetHyperEIP(pVM), CPUMGetHyperESP(pVM)));

    /*
     * We hide log flushes (outer) and hypervisor interrupts (inner).
     */
    for (;;)
    {
        int rc;
        do
        {
#ifdef NO_SUPCALLR0VMM
            rc = VERR_GENERAL_FAILURE;
#else
            rc = SUPCallVMMR0Fast(pVM->pVMR0, VMMR0_DO_RAW_RUN);
            if (RT_LIKELY(rc == VINF_SUCCESS))
                rc = pVM->vmm.s.iLastGCRc;
#endif
        } while (rc == VINF_EM_RAW_INTERRUPT_HYPER);

        /*
         * Flush the loggers,
         */
#ifdef LOG_ENABLED
        PRTLOGGERRC pLogger = pVM->vmm.s.pLoggerHC;
        if (    pLogger
            &&  pLogger->offScratch > 0)
            RTLogFlushGC(NULL, pLogger);
#endif
#ifdef VBOX_WITH_RC_RELEASE_LOGGING
        PRTLOGGERRC pRelLogger = pVM->vmm.s.pRelLoggerHC;
        if (RT_UNLIKELY(pRelLogger && pRelLogger->offScratch > 0))
            RTLogFlushGC(RTLogRelDefaultInstance(), pRelLogger);
#endif
        if (rc == VERR_TRPM_PANIC || rc == VERR_TRPM_DONT_PANIC)
            VMMR3FatalDump(pVM, rc);
        if (rc != VINF_VMM_CALL_HOST)
        {
            Log(("VMMR3ResumeHyper: returns %Vrc\n", rc));
            return rc;
        }
        rc = vmmR3ServiceCallHostRequest(pVM);
        if (VBOX_FAILURE(rc))
            return rc;
    }
}


/**
 * Service a call to the ring-3 host code.
 *
 * @returns VBox status code.
 * @param   pVM     VM handle.
 * @remark  Careful with critsects.
 */
static int vmmR3ServiceCallHostRequest(PVM pVM)
{
    switch (pVM->vmm.s.enmCallHostOperation)
    {
        /*
         * Acquire the PDM lock.
         */
        case VMMCALLHOST_PDM_LOCK:
        {
            pVM->vmm.s.rcCallHost = PDMR3LockCall(pVM);
            break;
        }

        /*
         * Flush a PDM queue.
         */
        case VMMCALLHOST_PDM_QUEUE_FLUSH:
        {
            PDMR3QueueFlushWorker(pVM, NULL);
            pVM->vmm.s.rcCallHost = VINF_SUCCESS;
            break;
        }

        /*
         * Grow the PGM pool.
         */
        case VMMCALLHOST_PGM_POOL_GROW:
        {
            pVM->vmm.s.rcCallHost = PGMR3PoolGrow(pVM);
            break;
        }

        /*
         * Maps an page allocation chunk into ring-3 so ring-0 can use it.
         */
        case VMMCALLHOST_PGM_MAP_CHUNK:
        {
            pVM->vmm.s.rcCallHost = PGMR3PhysChunkMap(pVM, pVM->vmm.s.u64CallHostArg);
            break;
        }

        /*
         * Allocates more handy pages.
         */
        case VMMCALLHOST_PGM_ALLOCATE_HANDY_PAGES:
        {
            pVM->vmm.s.rcCallHost = PGMR3PhysAllocateHandyPages(pVM);
            break;
        }
#ifndef VBOX_WITH_NEW_PHYS_CODE

        case VMMCALLHOST_PGM_RAM_GROW_RANGE:
        {
            const RTGCPHYS GCPhys = pVM->vmm.s.u64CallHostArg;
            pVM->vmm.s.rcCallHost = PGM3PhysGrowRange(pVM, &GCPhys);
            break;
        }
#endif

        /*
         * Acquire the PGM lock.
         */
        case VMMCALLHOST_PGM_LOCK:
        {
            pVM->vmm.s.rcCallHost = PGMR3LockCall(pVM);
            break;
        }

        /*
         * Flush REM handler notifications.
         */
        case VMMCALLHOST_REM_REPLAY_HANDLER_NOTIFICATIONS:
        {
            REMR3ReplayHandlerNotifications(pVM);
            break;
        }

        /*
         * This is a noop. We just take this route to avoid unnecessary
         * tests in the loops.
         */
        case VMMCALLHOST_VMM_LOGGER_FLUSH:
            break;

        /*
         * Set the VM error message.
         */
        case VMMCALLHOST_VM_SET_ERROR:
            VMR3SetErrorWorker(pVM);
            break;

        /*
         * Set the VM runtime error message.
         */
        case VMMCALLHOST_VM_SET_RUNTIME_ERROR:
            VMR3SetRuntimeErrorWorker(pVM);
            break;

        /*
         * Signal a ring 0 hypervisor assertion.
         * Cancel the longjmp operation that's in progress.
         */
        case VMMCALLHOST_VM_R0_HYPER_ASSERTION:
            pVM->vmm.s.enmCallHostOperation = VMMCALLHOST_INVALID;
            pVM->vmm.s.CallHostR0JmpBuf.fInRing3Call = false;
#ifdef RT_ARCH_X86
            pVM->vmm.s.CallHostR0JmpBuf.eip = 0;
#else
            pVM->vmm.s.CallHostR0JmpBuf.rip = 0;
#endif
            LogRel((pVM->vmm.s.szRing0AssertMsg1));
            LogRel((pVM->vmm.s.szRing0AssertMsg2));
            return VINF_EM_DBG_HYPER_ASSERTION;

        default:
            AssertMsgFailed(("enmCallHostOperation=%d\n", pVM->vmm.s.enmCallHostOperation));
            return VERR_INTERNAL_ERROR;
    }

    pVM->vmm.s.enmCallHostOperation = VMMCALLHOST_INVALID;
    return VINF_SUCCESS;
}



/**
 * Structure to pass to DBGFR3Info() and for doing all other
 * output during fatal dump.
 */
typedef struct VMMR3FATALDUMPINFOHLP
{
    /** The helper core. */
    DBGFINFOHLP Core;
    /** The release logger instance. */
    PRTLOGGER   pRelLogger;
    /** The saved release logger flags. */
    RTUINT      fRelLoggerFlags;
    /** The logger instance. */
    PRTLOGGER   pLogger;
    /** The saved logger flags. */
    RTUINT      fLoggerFlags;
    /** The saved logger destination flags. */
    RTUINT      fLoggerDestFlags;
    /** Whether to output to stderr or not. */
    bool        fStdErr;
} VMMR3FATALDUMPINFOHLP, *PVMMR3FATALDUMPINFOHLP;
typedef const VMMR3FATALDUMPINFOHLP *PCVMMR3FATALDUMPINFOHLP;


/**
 * Print formatted string.
 *
 * @param   pHlp        Pointer to this structure.
 * @param   pszFormat   The format string.
 * @param   ...         Arguments.
 */
static DECLCALLBACK(void) vmmR3FatalDumpInfoHlp_pfnPrintf(PCDBGFINFOHLP pHlp, const char *pszFormat, ...)
{
    va_list args;
    va_start(args, pszFormat);
    pHlp->pfnPrintfV(pHlp, pszFormat, args);
    va_end(args);
}


/**
 * Print formatted string.
 *
 * @param   pHlp        Pointer to this structure.
 * @param   pszFormat   The format string.
 * @param   args        Argument list.
 */
static DECLCALLBACK(void) vmmR3FatalDumpInfoHlp_pfnPrintfV(PCDBGFINFOHLP pHlp, const char *pszFormat, va_list args)
{
    PCVMMR3FATALDUMPINFOHLP pMyHlp = (PCVMMR3FATALDUMPINFOHLP)pHlp;

    if (pMyHlp->pRelLogger)
    {
        va_list args2;
        va_copy(args2, args);
        RTLogLoggerV(pMyHlp->pRelLogger, pszFormat, args2);
        va_end(args2);
    }
    if (pMyHlp->pLogger)
    {
        va_list args2;
        va_copy(args2, args);
        RTLogLoggerV(pMyHlp->pLogger, pszFormat, args);
        va_end(args2);
    }
    if (pMyHlp->fStdErr)
    {
        va_list args2;
        va_copy(args2, args);
        RTStrmPrintfV(g_pStdErr, pszFormat, args);
        va_end(args2);
    }
}


/**
 * Initializes the fatal dump output helper.
 *
 * @param   pHlp        The structure to initialize.
 */
static void vmmR3FatalDumpInfoHlpInit(PVMMR3FATALDUMPINFOHLP pHlp)
{
    memset(pHlp, 0, sizeof(*pHlp));

    pHlp->Core.pfnPrintf = vmmR3FatalDumpInfoHlp_pfnPrintf;
    pHlp->Core.pfnPrintfV = vmmR3FatalDumpInfoHlp_pfnPrintfV;

    /*
     * The loggers.
     */
    pHlp->pRelLogger = RTLogRelDefaultInstance();
#ifndef LOG_ENABLED
    if (!pHlp->pRelLogger)
#endif
        pHlp->pLogger = RTLogDefaultInstance();

    if (pHlp->pRelLogger)
    {
        pHlp->fRelLoggerFlags = pHlp->pRelLogger->fFlags;
        pHlp->pRelLogger->fFlags &= ~(RTLOGFLAGS_BUFFERED | RTLOGFLAGS_DISABLED);
    }

    if (pHlp->pLogger)
    {
        pHlp->fLoggerFlags     = pHlp->pLogger->fFlags;
        pHlp->fLoggerDestFlags = pHlp->pLogger->fDestFlags;
        pHlp->pLogger->fFlags     &= ~(RTLOGFLAGS_BUFFERED | RTLOGFLAGS_DISABLED);
#ifndef DEBUG_sandervl
        pHlp->pLogger->fDestFlags |= RTLOGDEST_DEBUGGER;
#endif
    }

    /*
     * Check if we need write to stderr.
     */
#ifdef DEBUG_sandervl
    pHlp->fStdErr = false; /* takes too long to display here */
#else
    pHlp->fStdErr = (!pHlp->pRelLogger || !(pHlp->pRelLogger->fDestFlags & (RTLOGDEST_STDOUT | RTLOGDEST_STDERR)))
                 && (!pHlp->pLogger || !(pHlp->pLogger->fDestFlags & (RTLOGDEST_STDOUT | RTLOGDEST_STDERR)));
#endif
}


/**
 * Deletes the fatal dump output helper.
 *
 * @param   pHlp        The structure to delete.
 */
static void vmmR3FatalDumpInfoHlpDelete(PVMMR3FATALDUMPINFOHLP pHlp)
{
    if (pHlp->pRelLogger)
    {
        RTLogFlush(pHlp->pRelLogger);
        pHlp->pRelLogger->fFlags = pHlp->fRelLoggerFlags;
    }

    if (pHlp->pLogger)
    {
        RTLogFlush(pHlp->pLogger);
        pHlp->pLogger->fFlags     = pHlp->fLoggerFlags;
        pHlp->pLogger->fDestFlags = pHlp->fLoggerDestFlags;
    }
}


/**
 * Dumps the VM state on a fatal error.
 *
 * @param   pVM         VM Handle.
 * @param   rcErr       VBox status code.
 */
VMMR3DECL(void) VMMR3FatalDump(PVM pVM, int rcErr)
{
    /*
     * Create our output helper and sync it with the log settings.
     * This helper will be used for all the output.
     */
    VMMR3FATALDUMPINFOHLP   Hlp;
    PCDBGFINFOHLP           pHlp = &Hlp.Core;
    vmmR3FatalDumpInfoHlpInit(&Hlp);

    /*
     * Header.
     */
    pHlp->pfnPrintf(pHlp,
                    "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
                    "!!\n"
                    "!!                 Guru Meditation %d (%Vrc)\n"
                    "!!\n",
                    rcErr, rcErr);

    /*
     * Continue according to context.
     */
    bool fDoneHyper = false;
    switch (rcErr)
    {
        /*
         * Hypervisor errors.
         */
        case VINF_EM_DBG_HYPER_ASSERTION:
        {
            const char *pszMsg1 = HWACCMR3IsActive(pVM) ? pVM->vmm.s.szRing0AssertMsg1 : VMMR3GetGCAssertMsg1(pVM);
            while (pszMsg1 && *pszMsg1 == '\n')
                pszMsg1++;
            const char *pszMsg2 = HWACCMR3IsActive(pVM) ? pVM->vmm.s.szRing0AssertMsg2 : VMMR3GetGCAssertMsg2(pVM);
            while (pszMsg2 && *pszMsg2 == '\n')
                pszMsg2++;
            pHlp->pfnPrintf(pHlp,
                            "%s"
                            "%s",
                            pszMsg1,
                            pszMsg2);
            if (    !pszMsg2
                ||  !*pszMsg2
                ||  strchr(pszMsg2, '\0')[-1] != '\n')
                pHlp->pfnPrintf(pHlp, "\n");
            pHlp->pfnPrintf(pHlp, "!!\n");
            /* fall thru */
        }
        case VERR_TRPM_DONT_PANIC:
        case VERR_TRPM_PANIC:
        case VINF_EM_RAW_STALE_SELECTOR:
        case VINF_EM_RAW_IRET_TRAP:
        case VINF_EM_DBG_HYPER_BREAKPOINT:
        case VINF_EM_DBG_HYPER_STEPPED:
        {
            /*
             * Active trap? This is only of partial interest when in hardware
             * assisted virtualization mode, thus the different messages.
             */
            uint32_t        uEIP       = CPUMGetHyperEIP(pVM);
            TRPMEVENT       enmType;
            uint8_t         u8TrapNo   =       0xce;
            RTGCUINT        uErrorCode = 0xdeadface;
            RTGCUINTPTR     uCR2       = 0xdeadface;
            int rc2 = TRPMQueryTrapAll(pVM, &u8TrapNo, &enmType, &uErrorCode, &uCR2);
            if (!HWACCMR3IsActive(pVM))
            {
                if (RT_SUCCESS(rc2))
                    pHlp->pfnPrintf(pHlp,
                                    "!! TRAP=%02x ERRCD=%RGv CR2=%RGv EIP=%RX32 Type=%d\n",
                                    u8TrapNo, uErrorCode, uCR2, uEIP, enmType);
                else
                    pHlp->pfnPrintf(pHlp,
                                    "!! EIP=%RX32 NOTRAP\n",
                                    uEIP);
            }
            else if (RT_SUCCESS(rc2))
                pHlp->pfnPrintf(pHlp,
                                "!! ACTIVE TRAP=%02x ERRCD=%RGv CR2=%RGv PC=%RGr Type=%d (Guest!)\n",
                                u8TrapNo, uErrorCode, uCR2, CPUMGetGuestRIP(pVM), enmType);

            /*
             * The hypervisor dump is not relevant when we're in VT-x/AMD-V mode.
             */
            if (HWACCMR3IsActive(pVM))
                pHlp->pfnPrintf(pHlp, "\n");
            else
            {
                /*
                 * Try figure out where eip is.
                 */
                /* core code? */
                if (uEIP - (RTGCUINTPTR)pVM->vmm.s.pvGCCoreCode < pVM->vmm.s.cbCoreCode)
                    pHlp->pfnPrintf(pHlp,
                                "!! EIP is in CoreCode, offset %#x\n",
                                uEIP - (RTGCUINTPTR)pVM->vmm.s.pvGCCoreCode);
                else
                {   /* ask PDM */  /** @todo ask DBGFR3Sym later? */
                    char        szModName[64];
                    RTRCPTR     RCPtrMod;
                    char        szNearSym1[260];
                    RTRCPTR     RCPtrNearSym1;
                    char        szNearSym2[260];
                    RTRCPTR     RCPtrNearSym2;
                    int rc = PDMR3LdrQueryRCModFromPC(pVM, uEIP,
                                                      &szModName[0],  sizeof(szModName),  &RCPtrMod,
                                                      &szNearSym1[0], sizeof(szNearSym1), &RCPtrNearSym1,
                                                      &szNearSym2[0], sizeof(szNearSym2), &RCPtrNearSym2);
                    if (VBOX_SUCCESS(rc))
                        pHlp->pfnPrintf(pHlp,
                                        "!! EIP in %s (%RRv) at rva %x near symbols:\n"
                                        "!!    %RRv rva %RRv off %08x  %s\n"
                                        "!!    %RRv rva %RRv off -%08x %s\n",
                                        szModName,  RCPtrMod, (unsigned)(uEIP - RCPtrMod),
                                        RCPtrNearSym1, RCPtrNearSym1 - RCPtrMod, (unsigned)(uEIP - RCPtrNearSym1), szNearSym1,
                                        RCPtrNearSym2, RCPtrNearSym2 - RCPtrMod, (unsigned)(RCPtrNearSym2 - uEIP), szNearSym2);
                    else
                        pHlp->pfnPrintf(pHlp,
                                        "!! EIP is not in any code known to VMM!\n");
                }

                /* Disassemble the instruction. */
                char szInstr[256];
                rc2 = DBGFR3DisasInstrEx(pVM, 0, 0, DBGF_DISAS_FLAGS_CURRENT_HYPER, &szInstr[0], sizeof(szInstr), NULL);
                if (VBOX_SUCCESS(rc2))
                    pHlp->pfnPrintf(pHlp,
                                    "!! %s\n", szInstr);

                /* Dump the hypervisor cpu state. */
                pHlp->pfnPrintf(pHlp,
                                "!!\n"
                                "!!\n"
                                "!!\n");
                rc2 = DBGFR3Info(pVM, "cpumhyper", "verbose", pHlp);
                fDoneHyper = true;

                /* Callstack. */
                DBGFSTACKFRAME Frame = {0};
                rc2 = DBGFR3StackWalkBeginHyper(pVM, &Frame);
                if (VBOX_SUCCESS(rc2))
                {
                    pHlp->pfnPrintf(pHlp,
                                    "!!\n"
                                    "!! Call Stack:\n"
                                    "!!\n"
                                    "EBP      Ret EBP  Ret CS:EIP    Arg0     Arg1     Arg2     Arg3     CS:EIP        Symbol [line]\n");
                    do
                    {
                        pHlp->pfnPrintf(pHlp,
                                        "%08RX32 %08RX32 %04RX32:%08RX32 %08RX32 %08RX32 %08RX32 %08RX32",
                                        (uint32_t)Frame.AddrFrame.off,
                                        (uint32_t)Frame.AddrReturnFrame.off,
                                        (uint32_t)Frame.AddrReturnPC.Sel,
                                        (uint32_t)Frame.AddrReturnPC.off,
                                        Frame.Args.au32[0],
                                        Frame.Args.au32[1],
                                        Frame.Args.au32[2],
                                        Frame.Args.au32[3]);
                        pHlp->pfnPrintf(pHlp, " %RTsel:%08RGv", Frame.AddrPC.Sel, Frame.AddrPC.off);
                        if (Frame.pSymPC)
                        {
                            RTGCINTPTR offDisp = Frame.AddrPC.FlatPtr - Frame.pSymPC->Value;
                            if (offDisp > 0)
                                pHlp->pfnPrintf(pHlp, " %s+%llx", Frame.pSymPC->szName, (int64_t)offDisp);
                            else if (offDisp < 0)
                                pHlp->pfnPrintf(pHlp, " %s-%llx", Frame.pSymPC->szName, -(int64_t)offDisp);
                            else
                                pHlp->pfnPrintf(pHlp, " %s", Frame.pSymPC->szName);
                        }
                        if (Frame.pLinePC)
                            pHlp->pfnPrintf(pHlp, " [%s @ 0i%d]", Frame.pLinePC->szFilename, Frame.pLinePC->uLineNo);
                        pHlp->pfnPrintf(pHlp, "\n");

                        /* next */
                        rc2 = DBGFR3StackWalkNext(pVM, &Frame);
                    } while (VBOX_SUCCESS(rc2));
                    DBGFR3StackWalkEnd(pVM, &Frame);
                }

                /* raw stack */
                pHlp->pfnPrintf(pHlp,
                                "!!\n"
                                "!! Raw stack (mind the direction).\n"
                                "!!\n"
                                "%.*Vhxd\n",
                                VMM_STACK_SIZE, (char *)pVM->vmm.s.pbHCStack);
            } /* !HWACCMR3IsActive */
            break;
        }

        default:
        {
            break;
        }

    } /* switch (rcErr) */


    /*
     * Generic info dumper loop.
     */
    static struct
    {
        const char *pszInfo;
        const char *pszArgs;
    } const     aInfo[] =
    {
        { "mappings",       NULL },
        { "hma",            NULL },
        { "cpumguest",      "verbose" },
        { "cpumguestinstr", "verbose" },
        { "cpumhyper",      "verbose" },
        { "cpumhost",       "verbose" },
        { "mode",           "all" },
        { "cpuid",          "verbose" },
        { "gdt",            NULL },
        { "ldt",            NULL },
        //{ "tss",            NULL },
        { "ioport",         NULL },
        { "mmio",           NULL },
        { "phys",           NULL },
        //{ "pgmpd",          NULL }, - doesn't always work at init time...
        { "timers",         NULL },
        { "activetimers",   NULL },
        { "handlers",       "phys virt hyper stats" },
        { "cfgm",           NULL },
    };
    for (unsigned i = 0; i < RT_ELEMENTS(aInfo); i++)
    {
        if (fDoneHyper && !strcmp(aInfo[i].pszInfo, "cpumhyper"))
            continue;
        pHlp->pfnPrintf(pHlp,
                        "!!\n"
                        "!! {%s, %s}\n"
                        "!!\n",
                        aInfo[i].pszInfo, aInfo[i].pszArgs);
        DBGFR3Info(pVM, aInfo[i].pszInfo, aInfo[i].pszArgs, pHlp);
    }

    /* done */
    pHlp->pfnPrintf(pHlp,
                    "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");


    /*
     * Delete the output instance (flushing and restoring of flags).
     */
    vmmR3FatalDumpInfoHlpDelete(&Hlp);
}



/**
 * Displays the Force action Flags.
 *
 * @param   pVM         The VM handle.
 * @param   pHlp        The output helpers.
 * @param   pszArgs     The additional arguments (ignored).
 */
static DECLCALLBACK(void) vmmR3InfoFF(PVM pVM, PCDBGFINFOHLP pHlp, const char *pszArgs)
{
    const uint32_t fForcedActions = pVM->fForcedActions;

    pHlp->pfnPrintf(pHlp, "Forced action Flags: %#RX32", fForcedActions);

    /* show the flag mnemonics  */
    int c = 0;
    uint32_t f = fForcedActions;
#define PRINT_FLAG(flag) do { \
        if (f & (flag)) \
        { \
            static const char *s_psz = #flag; \
            if (!(c % 6)) \
                pHlp->pfnPrintf(pHlp, "%s\n    %s", c ? "," : "", s_psz + 6); \
            else \
                pHlp->pfnPrintf(pHlp, ", %s", s_psz + 6); \
            c++; \
            f &= ~(flag); \
        } \
    } while (0)
    PRINT_FLAG(VM_FF_INTERRUPT_APIC);
    PRINT_FLAG(VM_FF_INTERRUPT_PIC);
    PRINT_FLAG(VM_FF_TIMER);
    PRINT_FLAG(VM_FF_PDM_QUEUES);
    PRINT_FLAG(VM_FF_PDM_DMA);
    PRINT_FLAG(VM_FF_PDM_CRITSECT);
    PRINT_FLAG(VM_FF_DBGF);
    PRINT_FLAG(VM_FF_REQUEST);
    PRINT_FLAG(VM_FF_TERMINATE);
    PRINT_FLAG(VM_FF_RESET);
    PRINT_FLAG(VM_FF_PGM_SYNC_CR3);
    PRINT_FLAG(VM_FF_PGM_SYNC_CR3_NON_GLOBAL);
    PRINT_FLAG(VM_FF_TRPM_SYNC_IDT);
    PRINT_FLAG(VM_FF_SELM_SYNC_TSS);
    PRINT_FLAG(VM_FF_SELM_SYNC_GDT);
    PRINT_FLAG(VM_FF_SELM_SYNC_LDT);
    PRINT_FLAG(VM_FF_INHIBIT_INTERRUPTS);
    PRINT_FLAG(VM_FF_CSAM_SCAN_PAGE);
    PRINT_FLAG(VM_FF_CSAM_PENDING_ACTION);
    PRINT_FLAG(VM_FF_TO_R3);
    PRINT_FLAG(VM_FF_DEBUG_SUSPEND);
    if (f)
        pHlp->pfnPrintf(pHlp, "%s\n    Unknown bits: %#RX32\n", c ? "," : "", f);
    else
        pHlp->pfnPrintf(pHlp, "\n");
#undef PRINT_FLAG

    /* the groups */
    c = 0;
#define PRINT_GROUP(grp) do { \
        if (fForcedActions & (grp)) \
        { \
            static const char *s_psz = #grp; \
            if (!(c % 5)) \
                pHlp->pfnPrintf(pHlp, "%s    %s", c ? ",\n" : "Groups:\n", s_psz + 6); \
            else \
                pHlp->pfnPrintf(pHlp, ", %s", s_psz + 6); \
            c++; \
        } \
    } while (0)
    PRINT_GROUP(VM_FF_EXTERNAL_SUSPENDED_MASK);
    PRINT_GROUP(VM_FF_EXTERNAL_HALTED_MASK);
    PRINT_GROUP(VM_FF_HIGH_PRIORITY_PRE_MASK);
    PRINT_GROUP(VM_FF_HIGH_PRIORITY_PRE_RAW_MASK);
    PRINT_GROUP(VM_FF_HIGH_PRIORITY_POST_MASK);
    PRINT_GROUP(VM_FF_NORMAL_PRIORITY_POST_MASK);
    PRINT_GROUP(VM_FF_NORMAL_PRIORITY_MASK);
    PRINT_GROUP(VM_FF_RESUME_GUEST_MASK);
    PRINT_GROUP(VM_FF_ALL_BUT_RAW_MASK);
    if (c)
        pHlp->pfnPrintf(pHlp, "\n");
#undef PRINT_GROUP
}

