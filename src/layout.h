#ifndef LIBDMVIEW_LAYOUT_H
#define LIBDMVIEW_LAYOUT_H

#include "dmview_format.h"

/*
 * Byte offset of operand `i` of an opcode's instructions, as constants: the
 * interpreter reads its operands with OPOFF(op, i) for constant op and i, which
 * folds into an immediate offset - no table lookup while executing.
 * tests/ check every entry against dmv_get_layout().
 */
static inline uint8_t layout_offset(uint8_t op, unsigned i)
{
    switch (op)
    {
        case DMV_OP_BOX:      { static const uint8_t o[] = { 4, 6, 8, 10, 12 };          return o[i]; }
        case DMV_OP_JMP:
        case DMV_OP_CALL:     { static const uint8_t o[] = { 4 };                        return o[i]; }
        case DMV_OP_JEQ:
        case DMV_OP_JNE:
        case DMV_OP_JLT:
        case DMV_OP_JLE:
        case DMV_OP_JGT:
        case DMV_OP_JGE:      { static const uint8_t o[] = { 4, 8, 12 };                 return o[i]; }
        case DMV_OP_SCROLL:   { static const uint8_t o[] = { 4, 6 };                     return o[i]; }
        case DMV_OP_FOCUS:
        case DMV_OP_OPACITY:  { static const uint8_t o[] = { 4 };                        return o[i]; }
        case DMV_OP_FILL:     { static const uint8_t o[] = { 4 };                        return o[i]; }
        case DMV_OP_RECT:     { static const uint8_t o[] = { 4, 6, 8, 10, 12 };          return o[i]; }
        case DMV_OP_RRECT:
        case DMV_OP_FRAME:    { static const uint8_t o[] = { 4, 6, 8, 10, 12, 16 };      return o[i]; }
        case DMV_OP_RFRAME:   { static const uint8_t o[] = { 4, 6, 8, 10, 12, 14, 16 };  return o[i]; }
        case DMV_OP_LINE:     { static const uint8_t o[] = { 4, 6, 8, 10, 12, 16 };      return o[i]; }
        case DMV_OP_CIRCLE:   { static const uint8_t o[] = { 4, 6, 8, 12 };              return o[i]; }
        case DMV_OP_RING:     { static const uint8_t o[] = { 4, 6, 8, 10, 12 };          return o[i]; }
        case DMV_OP_TEXT:     { static const uint8_t o[] = { 4, 6, 8, 10, 12, 14, 16 };  return o[i]; }
        case DMV_OP_IMAGE:    { static const uint8_t o[] = { 4, 6, 8, 10, 12 };          return o[i]; }
        case DMV_OP_ICON:     { static const uint8_t o[] = { 4, 6, 8, 10, 12, 16 };      return o[i]; }
        case DMV_OP_SET:
        case DMV_OP_ADD:
        case DMV_OP_SUB:
        case DMV_OP_MUL:
        case DMV_OP_DIV:
        case DMV_OP_MOD:
        case DMV_OP_MIN:
        case DMV_OP_MAX:      { static const uint8_t o[] = { 4, 8 };                     return o[i]; }
        case DMV_OP_CLAMP:    { static const uint8_t o[] = { 4, 8, 12 };                 return o[i]; }
        case DMV_OP_TOGGLE:   { static const uint8_t o[] = { 4 };                        return o[i]; }
        case DMV_OP_FORMAT:   { static const uint8_t o[] = { 4, 6, 8 };                  return o[i]; }
        case DMV_OP_APPEND:   { static const uint8_t o[] = { 4, 6 };                     return o[i]; }
        case DMV_OP_ON:       { static const uint8_t o[] = { 4, 6 };                     return o[i]; }
        case DMV_OP_REDRAW:
        case DMV_OP_EXEC:
        case DMV_OP_SIGNAL:
        case DMV_OP_GOTO:
        case DMV_OP_RELOAD:
        case DMV_OP_SETFOCUS: { static const uint8_t o[] = { 4 };                        return o[i]; }
        case DMV_OP_SCROLLTO: { static const uint8_t o[] = { 4, 6, 8 };                  return o[i]; }
        default:              return 0;
    }
}

#define OPOFF(op, i)    layout_offset(DMV_OP_##op, i)

#endif /* LIBDMVIEW_LAYOUT_H */
