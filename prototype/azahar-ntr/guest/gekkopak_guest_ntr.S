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
.equ REG_BLOCK_TX, 0x100
.equ REG_BLOCK_RX, 0x300

.equ CARD_START,  0x80000000
.equ CARD_RESET,  0x20000000
.equ CARD_READY,  0x00800000
.equ CARD_BLK4,   0x07000000

.equ W_WRITE_REG,   0xF0
.equ W_EXEC,        0xF1
.equ W_READ_REG,    0xF2
.equ W_PAYLOAD,     0xF3
.equ W_WRITE_BLOCK, 0xF4
.equ W_READ_BLOCK,  0xF5
.equ GK_G,          0x47
.equ GK_K,          0x4B
.equ EVENT_COMPLETION_DEPTH, 0xFE

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

.equ CAP_BLOCK_TRANSPORT, 0x10
.equ DESC_MAGIC,   0x31444B47
.equ DESC_VER_OP,  0x00010001
.equ DESC_INLINE,  0x00000001
.equ COMP_MAGIC,   0x31434B47
.equ PASS_MAGIC,   0x53534150
.equ FAIL_MAGIC,   0x4C494146

_start:
    ldr r10, =NTR
    ldr r11, =SUMMARY
    mov r8, #0
    mov r0, #0
    str r0, [r11, #0]

    /* Bootstrap over F0-F2 so block transport remains capability-negotiated. */
    mov r0, #CMD_HELLO
    bl exec_hi
    mov r0, #R_OUT0
    bl rdreg
    str r0, [r11, #4]

    mov r0, #CMD_GET_CAPS
    bl exec_hi
    mov r0, #R_OUT0
    bl rdreg
    str r0, [r11, #8]
    tst r0, #CAP_BLOCK_TRANSPORT
    beq failed
    mov r0, #R_OUT1
    bl rdreg
    str r0, [r11, #12]

    mov r0, #R_ARG0
    mov r1, #4096
    bl wrreg
    mov r0, #CMD_ALLOC
    bl exec_hi
    mov r0, #R_OUT0
    bl rdreg
    mov r7, r0
    str r7, [r11, #16]

    /* Build one 64-byte GKD1 descriptor + 16 inline bytes in the Azahar-only
     * block staging window. On hardware this buffer becomes the F4 NTR data phase. */
    add r9, r10, #REG_BLOCK_TX
    ldr r0, =DESC_MAGIC
    str r0, [r9, #0]
    ldr r0, =DESC_VER_OP
    str r0, [r9, #4]
    mov r0, #1
    str r0, [r9, #8]
    ldr r0, =DESC_INLINE
    str r0, [r9, #12]
    str r7, [r9, #16]
    mov r0, #0
    str r0, [r9, #20]
    mov r0, #16
    str r0, [r9, #24]
    mov r0, #0
    str r0, [r9, #28]
    str r0, [r9, #32]
    mov r0, #16
    str r0, [r9, #36]
    mov r0, #1
    str r0, [r9, #40]
    ldr r0, =250000
    str r0, [r9, #44]
    ldr r0, =3500
    str r0, [r9, #48]
    mov r0, #0
    str r0, [r9, #52]
    str r0, [r9, #56]
    str r0, [r9, #60]

    ldr r0, =0x11223344
    str r0, [r9, #64]
    ldr r0, =0x55667788
    str r0, [r9, #68]
    ldr r0, =0xAABBCCDD
    str r0, [r9, #72]
    ldr r0, =0x0BADF00D
    str r0, [r9, #76]

    /* Steady-state transaction 1/3: F4 descriptor + inline input. */
    mov r0, #W_WRITE_BLOCK
    mov r1, #0
    mov r2, #80
    mov r3, #0
    bl wire

    /* Steady-state transaction 2/3: one four-byte completion-depth event. */
    mov r0, #EVENT_COMPLETION_DEPTH
    bl rdreg
    cmp r0, #1
    blt failed

    /* Steady-state transaction 3/3: F5 completion block. len=64, offset=0. */
    mov r0, #W_READ_BLOCK
    mov r1, #1
    ldr r2, =0x00400000
    mov r3, #0
    bl wire

    add r9, r10, #REG_BLOCK_RX
    ldr r0, [r9, #0]
    ldr r1, =COMP_MAGIC
    cmp r0, r1
    bne failed
    ldrh r0, [r9, #6]
    cmp r0, #0
    bne failed
    ldr r0, [r9, #12]
    str r0, [r11, #20]
    ldr r0, [r9, #16]
    str r0, [r11, #24]
    ldr r0, [r9, #20]
    str r0, [r11, #28]
    ldr r0, [r9, #24]
    str r0, [r11, #32]
    ldr r0, [r9, #36]
    str r0, [r11, #36]
    ldr r0, [r9, #40]
    str r0, [r11, #40]
    mov r0, #3
    str r0, [r11, #44]

    /* Cleanup remains on the bootstrap/debug path. */
    mov r0, #R_ARG0
    mov r1, r7
    bl wrreg
    mov r0, #CMD_FREE
    bl exec_hi

    mov r0, #CMD_COMPLETE
    bl exec_hi
    ldr r0, =PASS_MAGIC
    str r0, [r11, #0]

finished:
    b finished

wrreg:
    push {lr}
    mov r2, r1
    mov r1, r0
    mov r0, #W_WRITE_REG
    mov r3, #0
    bl wire
    pop {pc}

rdreg:
    push {lr}
    mov r1, r0
    mov r0, #W_READ_REG
    mov r2, #0
    mov r3, #1
    bl wire
    pop {pc}

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

/* r0=wire opcode, r1=index/selector, r2=value, r3=read four-byte FIFO response. */
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
