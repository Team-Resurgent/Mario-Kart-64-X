# macros.inc — x86 build of the decomp's data-only assembly files.
#
# The .s files under data/ are pure data segments: .incbin, .balign, and a
# handful of integer directives. Only the label macros are needed here; the
# function macros (leaf, ent) never appear in a data segment.
#
# The labels in those files already carry a leading underscore -- the classic
# MIPS assembly convention for C symbols -- which happens to be exactly the
# prefix x86 Windows uses. So glabel must NOT add one: the names line up as
# written.

.macro glabel label
    .global \label
    .balign 4
\label:
.endm

.macro export x
    .global \x
\x:
.endm
