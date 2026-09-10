.text
.align 2
// Darwin ARM64 variadic arguments are in the caller's stack area. Preserve
// that address before the Linux-ABI C++ helper creates its own frame.
.global foundation_string_format
.type foundation_string_format,%function
foundation_string_format:
    mov x3, sp
    b foundation_string_format_stack
.global foundation_array_objects
.type foundation_array_objects,%function
foundation_array_objects:
    mov x3, sp
    b foundation_array_objects_stack
.global foundation_dictionary_objects_keys
.type foundation_dictionary_objects_keys,%function
foundation_dictionary_objects_keys:
    mov x3, sp
    b foundation_dictionary_objects_keys_stack
.section .note.GNU-stack,"",%progbits
