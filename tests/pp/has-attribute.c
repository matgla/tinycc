/* __has_attribute answers what tcc's attribute parser acts on. */
#if defined(__has_attribute)
defined_yes
#endif
#ifdef __has_attribute
ifdef_yes
#endif
#if __has_attribute(naked)
naked_yes
#endif
#if __has_attribute(noinline) && __has_attribute(__noinline__)
noinline_yes
#endif
#if __has_attribute(aligned) && __has_attribute(section) && __has_attribute(visibility)
aligned_section_visibility_yes
#endif
#if __has_attribute(transparent_union)
transparent_union_yes
#endif
#if __has_attribute(musttail)
musttail_wrong
#else
musttail_no
#endif
#if __has_attribute(stdcall)
stdcall_wrong
#else
stdcall_no
#endif
#define ATTR(x) __has_attribute(x)
#if ATTR(weak) && !ATTR(not_tail_called)
through_macro_yes
#endif
/* the operand is an attribute name and is not expanded even when a macro of
   that name exists, as in clang (gcc expands it). Passed as a function-like
   macro's argument it is expanded before substitution like any argument --
   which is why headers probe the __noinline__ spelling. */
#define noinline __attribute__((noinline))
#if __has_attribute(noinline) && ATTR(__noinline__)
operand_not_expanded_yes
#endif
