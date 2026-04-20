/* lsopcodes.h -- bytecode opcode definitions for the Litesrpent VM.
 *
 * Each instruction is a 32-bit word:
 *   bits  0..7  : opcode  (256 slots)
 *   bits  8..31 : operand (24-bit unsigned or signed immediate)
 *
 * For three-operand instructions the 24-bit field is split:
 *   bits  8..15 : A  (8 bits)
 *   bits 16..23 : B  (8 bits)
 *   bits 24..31 : C  (8 bits)
 */
#ifndef LS_OPCODES_H
#define LS_OPCODES_H

#include <stdint.h>

typedef enum {
    /* -- Loads / stores ------------------------------------------------- */
    OP_NOP = 0,         /* do nothing                                      */
    OP_CONST,           /* push consts[arg]                                */
    OP_NIL,             /* push nil                                        */
    OP_T,               /* push t                                          */
    OP_FIXNUM,          /* push small signed integer (24-bit sarg)         */
    OP_LOAD_LOCAL,      /* push locals[arg]                                */
    OP_STORE_LOCAL,     /* pop -> locals[arg]                              */
    OP_LOAD_UPVAL,      /* push upvals[arg]                                */
    OP_STORE_UPVAL,     /* pop -> upvals[arg]                              */
    OP_LOAD_GLOBAL,     /* push symbol-value of consts[arg] (a symbol)     */
    OP_STORE_GLOBAL,    /* pop -> symbol-value of consts[arg]              */

    /* -- Stack manipulation --------------------------------------------- */
    OP_POP,             /* discard TOS                                     */
    OP_DUP,             /* duplicate TOS                                   */

    /* -- Control flow --------------------------------------------------- */
    OP_JUMP,            /* pc += sarg                                      */
    OP_JUMP_IF_NIL,     /* pop; if nil, pc += sarg                         */
    OP_JUMP_IF_NOT_NIL, /* pop; if non-nil, pc += sarg                     */
    OP_CALL,            /* call TOS-nargs with nargs=arg arguments         */
    OP_TAILCALL,        /* tail call TOS-nargs with nargs=arg arguments    */
    OP_RETURN,          /* return TOS                                      */
    OP_HALT,            /* stop VM execution, return TOS                   */

    /* -- Arithmetic ----------------------------------------------------- */
    OP_ADD,             /* pop b, pop a, push a+b                          */
    OP_SUB,             /* pop b, pop a, push a-b                          */
    OP_MUL,             /* pop b, pop a, push a*b                          */
    OP_DIV,             /* pop b, pop a, push a/b                          */
    OP_MOD,             /* pop b, pop a, push a mod b                      */
    OP_NEG,             /* pop a, push -a                                  */

    /* -- Comparison ----------------------------------------------------- */
    OP_EQ,              /* pop b, pop a, push (eq a b)                     */
    OP_LT,              /* pop b, pop a, push (< a b)                      */
    OP_LE,              /* pop b, pop a, push (<= a b)                     */
    OP_GT,              /* pop b, pop a, push (> a b)                      */
    OP_GE,              /* pop b, pop a, push (>= a b)                     */
    OP_NUMEQ,           /* pop b, pop a, push (= a b)                      */

    /* -- Logic ---------------------------------------------------------- */
    OP_NOT,             /* pop a, push (not a)                             */

    /* -- Cons / list ---------------------------------------------------- */
    OP_CONS,            /* pop cdr, pop car, push (cons car cdr)           */
    OP_CAR,             /* pop a, push (car a)                             */
    OP_CDR,             /* pop a, push (cdr a)                             */
    OP_LIST,            /* pop arg values, push list of them               */

    /* -- Closures ------------------------------------------------------- */
    OP_CLOSURE,         /* create closure from inner proto consts[arg]     */

    /* -- Exception handling --------------------------------------------- */
    OP_PUSH_HANDLER,    /* push an exception handler (target = pc + sarg)  */
    OP_POP_HANDLER,     /* pop current handler                             */

    OP_MAX              /* sentinel -- not a real opcode                    */
} ls_opcode_t;

/* ---------- Instruction encoding / decoding macros ------------------- */

/* Build a 32-bit instruction from an opcode and a 24-bit unsigned arg. */
#define OP_MAKE(op, arg)    ((uint32_t)(((uint32_t)(arg) << 8) | ((uint32_t)(op) & 0xFF)))

/* Extract the 8-bit opcode. */
#define OP_GET_OP(instr)    ((uint32_t)((instr) & 0xFF))

/* Extract the 24-bit unsigned operand. */
#define OP_GET_ARG(instr)   ((uint32_t)((instr) >> 8))

/* Extract the 24-bit operand as a signed value (-8388608 .. 8388607). */
#define OP_GET_SARG(instr)  ((int32_t)(((int32_t)((instr) & 0xFFFFFF00)) >> 8))

/* Three-field encoding: A(8) B(8) C(8) packed into the upper 24 bits. */
#define OP_MAKE_ABC(op, a, b, c) \
    ((uint32_t)( ((uint32_t)(op) & 0xFF)       | \
                 (((uint32_t)(a) & 0xFF) << 8)  | \
                 (((uint32_t)(b) & 0xFF) << 16) | \
                 (((uint32_t)(c) & 0xFF) << 24) ))

#define OP_GET_A(instr)  (((instr) >>  8) & 0xFF)
#define OP_GET_B(instr)  (((instr) >> 16) & 0xFF)
#define OP_GET_C(instr)  (((instr) >> 24) & 0xFF)

/* Human-readable opcode names (for disassembler). */
static const char *const ls_opcode_names[] = {
    "NOP", "CONST", "NIL", "T", "FIXNUM",
    "LOAD_LOCAL", "STORE_LOCAL", "LOAD_UPVAL", "STORE_UPVAL",
    "LOAD_GLOBAL", "STORE_GLOBAL",
    "POP", "DUP",
    "JUMP", "JUMP_IF_NIL", "JUMP_IF_NOT_NIL",
    "CALL", "TAILCALL", "RETURN", "HALT",
    "ADD", "SUB", "MUL", "DIV", "MOD", "NEG",
    "EQ", "LT", "LE", "GT", "GE", "NUMEQ",
    "NOT",
    "CONS", "CAR", "CDR", "LIST",
    "CLOSURE",
    "PUSH_HANDLER", "POP_HANDLER",
};

#endif /* LS_OPCODES_H */
