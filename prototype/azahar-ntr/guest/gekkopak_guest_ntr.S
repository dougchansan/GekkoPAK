.syntax unified
.arm
.global _start
.type _start,%function

.equ NTR,         0x1EC64000
.equ SUMMARY,     0x0FFFC000

.equ REG_ROMCNT,  0x04
.equ REG_CMD0,    0x08
.equ REG_CMD1,    0x0C
.equ REG_FIFO,    0x1C

.equ CARD_START,  0x80000000
.equ CARD_RESET,  0x20000000
.equ CARD_READY,  0x00800000
.equ CARD_BLK4,   0x07000000

.equ W_WRITE_REG, 0xF0
.equ W_EXEC,      0xF1
.equ W_READ_REG,  0xF2
.equ W_PAYLOAD,   0xF3
.equ GK_G,        0x47
.equ GK_K,        0x4B

.equ R_ARG0,      0
.equ R_ARG1,      1
.equ R_ARG2,      2
.equ R_ARG3,      3
.equ R_PLEN,      4
.equ R_RESULT,    5
.equ R_OUT0,      6
.equ R_OUT1,      7
.equ R_OUT2,      8
.equ R_OUT3,      9

.equ CMD_HELLO,    1
.equ CMD_GET_CAPS, 2
.equ CMD_ALLOC,    3
.equ CMD_UPLOAD,   4
.equ CMD_SUBMIT,   5
.equ CMD_POLL,     6
.equ CMD_COLLECT,  7
.equ CMD_FREE,     8
.equ CMD_COMPLETE, 9

.equ PASS_MAGIC,  0x53534150
.equ FAIL_MAGIC,  0x4C494146

_start:
    ldr r10, =NTR
    ldr r11, =SUMMARY
    mov r8, #0
    mov r0, #0
    str r0, [r11, #0]

    /* HELLO */
    mov r0, #CMD_HELLO
    bl exec_hi
    mov r0, #R_OUT0
    bl rdreg
    str r0, [r11, #4]

    /* GET_CAPS */
    mov r0, #CMD_GET_CAPS
    bl exec_hi
    mov r0, #R_OUT0
    bl rdreg
    str r0, [r11, #8]
    mov r0, #R_OUT1
    bl rdreg
    str r0, [r11, #12]

    /* ALLOC 4096 */
    mov r0, #R_ARG0
    mov r1, #4096
    bl wrreg
    mov r0, #CMD_ALLOC
    bl exec_hi
    mov r0, #R_OUT0
    bl rdreg
    mov r7, r0
    str r7, [r11, #16]

    /* 16 bytes guest -> cart, carried in F3 command words. */
    mov r0, #0
    ldr r1, =0x11223344
    bl wrpayload
    mov r0, #1
    ldr r1, =0x55667788
    bl wrpayload
    mov r0, #2
    ldr r1, =0xAABBCCDD
    bl wrpayload
    mov r0, #3
    ldr r1, =0x0BADF00D
    bl wrpayload

    mov r0, #R_ARG0
    mov r1, r7
    bl wrreg
    mov r0, #R_ARG1
    mov r1, #0
    bl wrreg
    mov r0, #R_PLEN
    mov r1, #16
    bl wrreg
    mov r0, #CMD_UPLOAD
    bl exec_hi

    /* SUBMIT DSP job. */
    mov r0, #R_ARG0
    mov r1, r7
    bl wrreg
    mov r0, #R_ARG1
    mov r1, #1
    bl wrreg
    mov r0, #R_ARG2
    ldr r1, =250000
    bl wrreg
    mov r0, #R_ARG3
    ldr r1, =3500
    bl wrreg
    mov r0, #CMD_SUBMIT
    bl exec_hi
    mov r0, #R_OUT0
    bl rdreg
    mov r6, r0
    str r6, [r11, #20]

poll_again:
    mov r0, #R_ARG0
    mov r1, r6
    bl wrreg
    mov r0, #CMD_POLL
    bl exec_hi
    mov r0, #R_OUT0
    bl rdreg
    cmp r0, #0
    beq poll_again

    /* COLLECT */
    mov r0, #R_ARG0
    mov r1, r6
    bl wrreg
    mov r0, #CMD_COLLECT
    bl exec_hi
    mov r0, #R_OUT0
    bl rdreg
    str r0, [r11, #24]
    mov r0, #R_OUT1
    bl rdreg
    str r0, [r11, #28]
    mov r0, #R_OUT2
    bl rdreg
    str r0, [r11, #32]

    /* FREE */
    mov r0, #R_ARG0
    mov r1, r7
    bl wrreg
    mov r0, #CMD_FREE
    bl exec_hi

    /* COMPLETE must be acknowledged by the cartridge before PASS is visible. */
    mov r0, #CMD_COMPLETE
    bl exec_hi
    ldr r0, =PASS_MAGIC
    str r0, [r11, #0]

finished:
    b finished

/* r0 = staging register, r1 = value */
wrreg:
    push {lr}
    mov r2, r1
    mov r1, r0
    mov r0, #W_WRITE_REG
    mov r3, #0
    bl wire
    pop {pc}

/* r0 = payload word index, r1 = value */
wrpayload:
    push {lr}
    mov r2, r1
    mov r1, r0
    mov r0, #W_PAYLOAD
    mov r3, #0
    bl wire
    pop {pc}

/* r0 = staging register; returns value in r0 */
rdreg:
    push {lr}
    mov r1, r0
    mov r0, #W_READ_REG
    mov r2, #0
    mov r3, #1
    bl wire
    pop {pc}

/* r0 = high command. */
exec_hi:
    push {r4, lr}
    mov r4, r0
    add r8, r8, #1
    mov r0, #W_EXEC
    mov r1, r4
    mov r2, r8
    mov r3, #0
    bl wire
    mov r0, #R_RESULT
    bl rdreg
    cmp r0, #0
    bne failed
    pop {r4, pc}

/*
 * Low-level NTR transaction.
 * r0=wire opcode, r1=index/high command, r2=value/sequence, r3=read response.
 */
wire:
    push {r4-r5, lr}
    and r0, r0, #0xFF
    and r1, r1, #0xFF
    orr r4, r0, #(GK_G << 8)
    orr r4, r4, #(GK_K << 16)
    orr r4, r4, r1, lsl #24
    str r4, [r10, #REG_CMD0]
    str r2, [r10, #REG_CMD1]

    ldr r4, =(CARD_START | CARD_RESET)
    cmp r3, #0
    orrne r4, r4, #CARD_BLK4
    str r4, [r10, #REG_ROMCNT]

1:  ldr r5, [r10, #REG_ROMCNT]
    tst r5, #CARD_START
    bne 1b

    cmp r3, #0
    beq 3f
2:  ldr r5, [r10, #REG_ROMCNT]
    tst r5, #CARD_READY
    beq 2b
    ldr r0, [r10, #REG_FIFO]
3:
    pop {r4-r5, pc}

failed:
    ldr r0, =FAIL_MAGIC
    str r0, [r11, #0]
fail_spin:
    b fail_spin

.size _start, .-_start
