LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_C_INCLUDES += $(multirom_local_path) $(multirom_local_path)/lib
LOCAL_SRC_FILES:= \
    trampoline.c \
    devices.c \
    adb.c \

LOCAL_MODULE:= trampoline
LOCAL_MODULE_TAGS := optional

LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT)
LOCAL_UNSTRIPPED_PATH := $(TARGET_ROOT_OUT_UNSTRIPPED)
LOCAL_STATIC_LIBRARIES := libcutils libc libmultirom_static libbootimg
LOCAL_FORCE_STATIC_EXECUTABLE := true

ifeq ($(MR_INIT_DEVICES),)
    $(info MR_INIT_DEVICES was not defined in device files!)
endif
LOCAL_SRC_FILES += ../../../../$(MR_INIT_DEVICES)

# for adb
LOCAL_CFLAGS += -DPRODUCT_MODEL="\"$(PRODUCT_MODEL)\"" -DPRODUCT_MANUFACTURER="\"$(PRODUCT_MANUFACTURER)\""

# to find fstab
LOCAL_CFLAGS += -DTARGET_DEVICE="\"$(TARGET_DEVICE)\""

# create by-name symlinks for kernels that don't have it (eg older HTC One M7)
# specify MR_POPULATE_BY_NAME_PATH := "/dev/block/platform/msm_sdcc.1/by-name"
# or similar in BoardConfig
ifneq ($(MR_POPULATE_BY_NAME_PATH),)
    LOCAL_CFLAGS += -DMR_POPULATE_BY_NAME_PATH=\"$(MR_POPULATE_BY_NAME_PATH)\"
endif

# also add /dev/block/bootdevice symlinks
ifeq ($(MR_DEV_BLOCK_BOOTDEVICE),true)
    LOCAL_CFLAGS += -DMR_DEV_BLOCK_BOOTDEVICE
endif
ifneq ($(MR_DEVICE_BOOTDEVICE),)
    LOCAL_CFLAGS += -DMR_DEVICE_BOOTDEVICE="\"$(MR_DEVICE_BOOTDEVICE)\""
endif

ifneq ($(MR_DEVICE_HOOKS),)
ifeq ($(MR_DEVICE_HOOKS_VER),)
    $(info MR_DEVICE_HOOKS is set but MR_DEVICE_HOOKS_VER is not specified!)
else
    LOCAL_CFLAGS += -DMR_DEVICE_HOOKS=$(MR_DEVICE_HOOKS_VER)
    LOCAL_SRC_FILES += ../../../../$(MR_DEVICE_HOOKS)
endif
endif

ifeq ($(MR_ENCRYPTION),true)
    LOCAL_CFLAGS += -DMR_ENCRYPTION
    LOCAL_SRC_FILES += encryption.c

    ifeq ($(MR_QSEECOMD_HAX),true)
    MR_NO_KEXEC_MK_OPTIONS := true 1 allowed 2 enabled 3 ui_confirm 4 ui_choice 5 forced
    ifneq (,$(filter $(MR_NO_KEXEC), $(MR_NO_KEXEC_MK_OPTIONS)))
        ifneq (,$(filter $(MR_NO_KEXEC), true 1 allowed))
            # NO_KEXEC_DISABLED    =  0x00,   // no-kexec workaround disabled
            LOCAL_CFLAGS += -DMR_NO_KEXEC=0x00
        else ifneq (,$(filter $(MR_NO_KEXEC), 2 enabled))
            # NO_KEXEC_ALLOWED     =  0x01,   // "Use no-kexec only when needed"
            LOCAL_CFLAGS += -DMR_NO_KEXEC=0x01
        else ifneq (,$(filter $(MR_NO_KEXEC), 3 ui_confirm))
            # NO_KEXEC_CONFIRM     =  0x02,   // "..... but also ask for confirmation"
            LOCAL_CFLAGS += -DMR_NO_KEXEC=0x02
        else ifneq (,$(filter $(MR_NO_KEXEC), 4 ui_choice))
            # NO_KEXEC_CHOICE      =  0x04,   // "Ask whether to kexec or use no-kexec"
            LOCAL_CFLAGS += -DMR_NO_KEXEC=0x04
        else ifneq (,$(filter $(MR_NO_KEXEC), 5 forced))
            # NO_KEXEC_FORCED      =  0x08,   // "Always force using no-kexec workaround"
            LOCAL_CFLAGS += -DMR_NO_KEXEC=0x08
        endif

        LOCAL_CFLAGS += -DMR_QSEECOMD_HAX

        # clone libbootimg to /system/extras/ from
        # https://github.com/Tasssadar/libbootimg.git
        LOCAL_STATIC_LIBRARIES += libbootimg
        LOCAL_C_INCLUDES += system/extras/libbootimg/include

        LOCAL_SRC_FILES += ../no_kexec.c
    else
        $(error MR_NO_KEXEC is not set; this is needed for MR_QSEECOMD_HAX)
    endif # MR_NO_KEXEC
    endif # MR_QSEECOMD_HAX
endif

ifeq ($(MR_ENCRYPTION_FAKE_PROPERTIES),true)
    LOCAL_CFLAGS += -DMR_ENCRYPTION_FAKE_PROPERTIES
endif

ifeq ($(MR_USE_DEBUGFS_MOUNT),true)
    LOCAL_CFLAGS += -DMR_USE_DEBUGFS_MOUNT
endif

include $(BUILD_EXECUTABLE)
