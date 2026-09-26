// SPDX-License-Identifier: Apache-2.0
// Compiles, then fails code generation: no assembler knows this instruction. The
// frontend's warning must be reported along with the error.
#warning "frontend warning before a code-generation error"

void f(void)
{
    __asm__ volatile("ct_not_an_instruction");
}
