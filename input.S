section .text
    global _start

_start:

read_loop:
    in al, 0xf1
    out 0xf1, al
    jmp read_loop

end:
    hlt
