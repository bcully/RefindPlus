/** @file
 * AmendSysTable.c
 * Amends the SystemTable to provide CreateEventEx and a UEFI 2 Revision Number
 *
 * Copyright (c) 2020 Dayo Akanji (dakanji@users.sourceforge.net)
 * Portions Copyright (c) 2020 Joe van Tunen (joevt@shaw.ca)
 * Portions Copyright (c) 2004-2008 The Intel Corporation
 *
 * THIS PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
 * WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include "../MainRP/global.h"
#include "../include/refit_call_wrapper.h"
#include "../../MdeModulePkg/Core/Dxe/DxeMain.h"
#include "../../MdeModulePkg/Core/Dxe/Event/Event.h"

#define EFI_FIELD_OFFSET(TYPE, Field) ((UINTN) (&(((TYPE *) 0)->Field)))

EFI_CPU_ARCH_PROTOCOL   *gCpu       = NULL;
EFI_SMM_BASE2_PROTOCOL  *gSmmBase2  = NULL;

EFI_RUNTIME_ARCH_PROTOCOL gRuntimeTemplate = {
    INITIALIZE_LIST_HEAD_VARIABLE (gRuntimeTemplate.ImageHead),
    INITIALIZE_LIST_HEAD_VARIABLE (gRuntimeTemplate.EventHead),
    sizeof (EFI_MEMORY_DESCRIPTOR) +
        sizeof (UINT64) -
        (sizeof (EFI_MEMORY_DESCRIPTOR) % sizeof (UINT64)),
    EFI_MEMORY_DESCRIPTOR_VERSION, 0,
    NULL, NULL,
    FALSE, FALSE
};

UINTN                      gEventPending      = 0;
EFI_TPL                    gEfiCurrentTpl     = TPL_APPLICATION;
EFI_LOCK                   gEventQueueLock    = EFI_INITIALIZE_LOCK_VARIABLE (TPL_HIGH_LEVEL);
EFI_RUNTIME_ARCH_PROTOCOL  *gRuntime          = &gRuntimeTemplate;
LIST_ENTRY                 gEventSignalQueue  = INITIALIZE_LIST_HEAD_VARIABLE (gEventSignalQueue);
LIST_ENTRY                 gEventQueue[TPL_HIGH_LEVEL + 1];

UINT32 mEventTable[] = {
    EVT_TIMER | EVT_NOTIFY_SIGNAL,
    EVT_TIMER, EVT_NOTIFY_WAIT, EVT_NOTIFY_SIGNAL,
    EVT_SIGNAL_EXIT_BOOT_SERVICES,
    EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE, 0x00000000,
    EVT_TIMER | EVT_NOTIFY_WAIT
};

EFI_TPL EFIAPI FakeRaiseTpl (IN EFI_TPL  NewTpl);
VOID EFIAPI FakeRestoreTpl (IN EFI_TPL  NewTpl);
VOID FakeSetInterruptState (IN BOOLEAN  Enable);
VOID FakeDispatchEventNotifies (IN EFI_TPL  Priority);
VOID FakeAcquireLock (IN EFI_LOCK  *Lock);
VOID FakeReleaseLock (IN EFI_LOCK  *Lock);
EFI_STATUS AmendSysTable (VOID);
EFI_STATUS FakeCreateEventEx (
    UINT32            Type,
    EFI_TPL           NotifyTpl,
    EFI_EVENT_NOTIFY  NotifyFunction,
    const void        *NotifyContext,
    const EFI_GUID    *EventGroup,
    EFI_EVENT         *Event
);

/**
  Set Interrupt State.
  @param Enable: The state of enable or disable interrupt
**/
VOID
FakeSetInterruptState (
    IN BOOLEAN  Enable
) {
    EFI_STATUS  Status;
    BOOLEAN     InSmm;

    if (gCpu == NULL) {
        return;
    }
    else if (!Enable) {
        gCpu->DisableInterrupt (gCpu);
        return;
    }
    else if (gSmmBase2 == NULL) {
        gCpu->EnableInterrupt (gCpu);
        return;
    }

    Status = gSmmBase2->InSmm (gSmmBase2, &InSmm);
    if (!EFI_ERROR (Status) && !InSmm) {
        gCpu->EnableInterrupt (gCpu);
    }
}

/**
  Dispatches pending events.
  @param Priority: Task priority level of event notifications to dispatch
**/
VOID
FakeDispatchEventNotifies (
    IN EFI_TPL  Priority
) {
    IEVENT        *Event;
    LIST_ENTRY    *Head;

    FakeAcquireLock (&gEventQueueLock);
    ASSERT (gEventQueueLock.OwnerTpl == Priority);
    Head = &gEventQueue[Priority];

    // Dispatch pending notifications
    while (!IsListEmpty (Head)) {
        Event = refit_call4_wrapper(
            CR,
            Head->ForwardLink,
            IEVENT,
            NotifyLink,
            EVENT_SIGNATURE
        );
        refit_call1_wrapper(RemoveEntryList, &Event->NotifyLink);
        Event->NotifyLink.ForwardLink = NULL;

        // Only clear the SIGNAL status if it is a SIGNAL type event.
        // WAIT type events are only cleared in CheckEvent()
        if ((Event->Type & EVT_NOTIFY_SIGNAL) != 0) {
            Event->SignalCount = 0;
        }

        FakeReleaseLock (&gEventQueueLock);

        // Notify this event
        ASSERT (Event->NotifyFunction != NULL);
        refit_call2_wrapper(
            Event->NotifyFunction,
            Event,
            Event->NotifyContext
        );

        // Check for next pending event
        FakeAcquireLock (&gEventQueueLock);
    } // while

    gEventPending &= ~(UINTN)(1 << Priority);
    FakeReleaseLock (&gEventQueueLock);
}

/**
  Raise the task priority level to the new level.
  High level is implemented by disabling processor interrupts.
  @param  NewTpl:  New task priority level
  @return The previous task priority level
**/
EFI_TPL
EFIAPI
FakeRaiseTpl (
    IN EFI_TPL  NewTpl
) {
    EFI_TPL     OldTpl;

    OldTpl = gEfiCurrentTpl;
    if (OldTpl > NewTpl) {
        #if REFIT_DEBUG > 0
        MsgLog (
            "FATAL ERROR: RaiseTpl with OldTpl (0x%x) > NewTpl (0x%x)\n\n",
            OldTpl,
            NewTpl
        );
        #endif

        ASSERT (FALSE);
    }
    ASSERT (VALID_TPL (NewTpl));

    // If raising to high level, disable interrupts
    if (NewTpl >= TPL_HIGH_LEVEL  &&  OldTpl < TPL_HIGH_LEVEL) {
        FakeSetInterruptState (FALSE);
    }

    // Set the new value
    gEfiCurrentTpl = NewTpl;

    return OldTpl;
}

/**
  Lowers the task priority to the previous value.   If the new
  priority unmasks events at a higher priority, they are dispatched.
  @param  NewTpl:  New, lower, task priority
**/
VOID
EFIAPI
FakeRestoreTpl (
    IN EFI_TPL NewTpl
) {
    EFI_TPL    OldTpl;
    EFI_TPL    PendingTpl;

    OldTpl = gEfiCurrentTpl;
    if (NewTpl > OldTpl) {
        #if REFIT_DEBUG > 0
        MsgLog (
            "FATAL ERROR: RestoreTpl with NewTpl (0x%x) > OldTpl (0x%x)\n",
            NewTpl,
            OldTpl
        );
        #endif

        ASSERT (FALSE);
    }
    ASSERT (VALID_TPL (NewTpl));

    // Ensure interrupts are enabled if lowering below HIGH_LEVEL
    if (OldTpl >= TPL_HIGH_LEVEL  &&  NewTpl < TPL_HIGH_LEVEL) {
        gEfiCurrentTpl = TPL_HIGH_LEVEL;
    }

    // Dispatch pending events
    while (gEventPending != 0) {
        PendingTpl = (UINTN) HighBitSet64 (gEventPending);
        if (PendingTpl <= NewTpl) {
            break;
        }

        gEfiCurrentTpl = PendingTpl;
        if (gEfiCurrentTpl < TPL_HIGH_LEVEL) {
            FakeSetInterruptState (TRUE);
        }
        FakeDispatchEventNotifies (gEfiCurrentTpl);
    } // while

    // Set new value
    gEfiCurrentTpl = NewTpl;

    // Ensure interrupts are enabled if lowering below HIGH_LEVEL
    if (gEfiCurrentTpl < TPL_HIGH_LEVEL) {
        FakeSetInterruptState (TRUE);
    }
}

/**
  Raising to the task priority level of the mutual exclusion
  lock, and then acquires ownership of the lock.
  @param  Lock:  The lock to acquire
  @return Lock owned
**/
VOID
FakeAcquireLock (
    IN EFI_LOCK  *Lock
) {
    ASSERT (Lock != NULL);
    ASSERT (Lock->Lock == EfiLockReleased);

    Lock->OwnerTpl = FakeRaiseTpl (Lock->Tpl);
    Lock->Lock     = EfiLockAcquired;
}

/**
  Releases ownership of the mutual exclusion lock, and
  restores the previous task priority level.
  @param  Lock:  The lock to release
  @return Lock unowned
**/
VOID
FakeReleaseLock (
    IN EFI_LOCK  *Lock
) {
    EFI_TPL Tpl;

    ASSERT (Lock != NULL);
    ASSERT (Lock->Lock == EfiLockAcquired);

    Tpl        = Lock->OwnerTpl;
    Lock->Lock = EfiLockReleased;

    FakeRestoreTpl (Tpl);
}


EFI_STATUS
FakeCreateEventEx (
    UINT32            Type,
    EFI_TPL           NotifyTpl,
    EFI_EVENT_NOTIFY  NotifyFunction,
    const void        *NotifyContext,
    const EFI_GUID    *EventGroup,
    EFI_EVENT         *Event
) {
    EFI_STATUS      Status = EFI_SUCCESS;
    IEVENT          *IEvent;
    INTN            Index;

    // Check for invalid NotifyTpl if a notify event type
    if ((Type & (EVT_NOTIFY_WAIT | EVT_NOTIFY_SIGNAL)) != 0) {
        if (NotifyTpl != TPL_APPLICATION &&
            NotifyTpl != TPL_CALLBACK &&
            NotifyTpl != TPL_NOTIFY
        ) {
            return EFI_INVALID_PARAMETER;
        }
    }

    if (Event == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    // Ensure no reserved flags are set
    Status = EFI_INVALID_PARAMETER;
    for (Index = 0; Index < (sizeof (mEventTable) / sizeof (UINT32)); Index++) {
        if (Type == mEventTable[Index]) {
            Status = EFI_SUCCESS;
            break;
        }
    }
    if (EFI_ERROR (Status)) {
        return EFI_INVALID_PARAMETER;
    }

    // Convert Event type for pre-defined Event groups
    if (EventGroup != NULL) {
        // EVT_SIGNAL_EXIT_BOOT_SERVICES and EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE
        // are invalid For EventGroup
        if ((Type == EVT_SIGNAL_EXIT_BOOT_SERVICES) ||
            (Type == EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE)
        ) {
            return EFI_INVALID_PARAMETER;
        }

        if (CompareGuid (EventGroup, &gEfiEventExitBootServicesGuid)) {
            Type = EVT_SIGNAL_EXIT_BOOT_SERVICES;
        }
        else if (CompareGuid (EventGroup, &gEfiEventVirtualAddressChangeGuid)) {
            Type = EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE;
        }
    }
    else {
        // Convert EFI 1.10 Events to their UEFI 2.0 CreateEventEx mapping
        if (Type == EVT_SIGNAL_EXIT_BOOT_SERVICES) {
            EventGroup = &gEfiEventExitBootServicesGuid;
        }
        else if (Type == EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE) {
            EventGroup = &gEfiEventVirtualAddressChangeGuid;
        }
    }

    // Check parameters if notify event type
    if ((Type & (EVT_NOTIFY_WAIT | EVT_NOTIFY_SIGNAL)) != 0) {
        // Check for invalid NotifyFunction or NotifyTpl
        if ((NotifyFunction == NULL) ||
            (NotifyTpl <= TPL_APPLICATION) ||
            (NotifyTpl >= TPL_HIGH_LEVEL)
        ) {
            return EFI_INVALID_PARAMETER;
        }
    }
    else {
        // No notification needed. Zero out ignored values
        NotifyTpl      = 0;
        NotifyFunction = NULL;
        NotifyContext  = NULL;
    }

    // Allocate and initialize a new event structure.
    if ((Type & EVT_RUNTIME) != 0) {
        IEvent = AllocateRuntimeZeroPool (sizeof (IEVENT));
    }
    else {
        IEvent = AllocateZeroPool (sizeof (IEVENT));
    }

    if (IEvent == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    IEvent->Signature = EVENT_SIGNATURE;
    IEvent->Type = Type;

    IEvent->NotifyTpl      = NotifyTpl;
    IEvent->NotifyFunction = NotifyFunction;
    IEvent->NotifyContext  = (VOID *)NotifyContext;

    if (EventGroup != NULL) {
        CopyGuid (&IEvent->EventGroup, EventGroup);
        IEvent->ExFlag |= EVT_EXFLAG_EVENT_GROUP;
    }

    *Event = IEvent;

    if ((Type & EVT_RUNTIME) != 0) {
        // Keep a list of all RT events so we can tell the RT AP.
        IEvent->RuntimeData.Type           = Type;
        IEvent->RuntimeData.NotifyTpl      = NotifyTpl;
        IEvent->RuntimeData.NotifyFunction = NotifyFunction;
        IEvent->RuntimeData.NotifyContext  = (VOID *) NotifyContext;
        IEvent->RuntimeData.Event          = (EFI_EVENT *) IEvent;
        InsertTailList (&gRuntime->EventHead, &IEvent->RuntimeData.Link);
    }

    FakeAcquireLock (&gEventQueueLock);

    if ((Type & EVT_NOTIFY_SIGNAL) != 0x00000000) {
        // The Event's NotifyFunction must be queued whenever the event is signaled
        InsertHeadList (&gEventSignalQueue, &IEvent->SignalLink);
    }

    FakeReleaseLock (&gEventQueueLock);

    // Done
    return EFI_SUCCESS;
}

EFI_STATUS
AmendSysTable (
    VOID
) {
    EFI_STATUS        Status;
    EFI_BOOT_SERVICES *uBS;

    if (gST->Hdr.Revision <= 0x1FFFF ||
        gBS->Hdr.HeaderSize <= EFI_FIELD_OFFSET(EFI_BOOT_SERVICES, CreateEventEx)
    ) {
        uBS = (EFI_BOOT_SERVICES *) AllocateCopyPool (sizeof (*gBS) * 2, gBS);
        if (uBS) {
            uBS->CreateEventEx  = FakeCreateEventEx;
            uBS->Hdr.HeaderSize = sizeof (*gBS);

            gBS                 = uBS;
            gBS->Hdr.CRC32      = 0;
            gBS->CalculateCrc32 (
                gBS,
                gBS->Hdr.HeaderSize,
                &gBS->Hdr.CRC32
            );

            gST->BootServices   = gBS;
            gST->Hdr.Revision   = 0x0002001E;
            gST->Hdr.CRC32      = 0;
            gBS->CalculateCrc32 (
                (VOID *) gST,
                sizeof (EFI_SYSTEM_TABLE),
                &gST->Hdr.CRC32
            );


            TweakSysTable       = TRUE;
            Status              = EFI_SUCCESS;
        }
        else {
            Status = EFI_LOAD_ERROR;
        }
    }
    else {
        Status = EFI_ALREADY_STARTED;
    }

    return Status;
}
