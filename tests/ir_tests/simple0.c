❯ ../../armv8m-tcc -c 20_op_add.c -o 20_op_add
Generating IR for function simple0
DEBUG gfunc_return: entry, func_type->t=0x3
DEBUG gfunc_return: before RETURNVALUE, vtop->r = 0x30, VT_LVAL=0
=== IR BEFORE OPTIMIZATIONS ===
0000: RETURNVALUE #12312
=== END IR BEFORE OPTIMIZATIONS ===
0000: RETURNVALUE #12312
Generating IR for function simple01
DEBUG gfunc_return: entry, func_type->t=0x3
DEBUG gfunc_return: before RETURNVALUE, vtop->r = 0x30, VT_LVAL=0
=== IR BEFORE OPTIMIZATIONS ===
0000: RETURNVALUE #-559038737
=== END IR BEFORE OPTIMIZATIONS ===
0000: RETURNVALUE #-559038737
Generating IR for function simple02
DEBUG gfunc_return: entry, func_type->t=0x3
DEBUG gfunc_return: before RETURNVALUE, vtop->r = 0x0, VT_LVAL=0
=== IR BEFORE OPTIMIZATIONS ===
0000: VReg VAR:0 <-- #-559038737
0001: VReg TMP:0 <-- VReg PAR:0 ADD VReg VAR:0
0002: RETURNVALUE VReg TMP:0
=== END IR BEFORE OPTIMIZATIONS ===
0000: VReg VAR:0 <-- #-559038737
0001: VReg TMP:0 <-- VReg PAR:0 ADD VReg VAR:0
0002: RETURNVALUE VReg TMP:0
Adding live interval for VReg VAR:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:0, start=1 end=2 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg PAR:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Interval 0 (0,1), VReg PAR:0 [int] --> R0
Interval 1 (0,1), VReg VAR:0 [int] --> R1
Interval 2 (1,2), VReg TMP:0 [int] --> R2
Generating IR for function simple022
DEBUG gfunc_return: entry, func_type->t=0x3
DEBUG gfunc_return: before RETURNVALUE, vtop->r = 0x0, VT_LVAL=0
=== IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 ADD #-559038737
0001: RETURNVALUE VReg TMP:0
=== END IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 ADD #-559038737
0001: RETURNVALUE VReg TMP:0
Adding live interval for VReg TMP:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg PAR:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Interval 0 (0,1), VReg PAR:0 [int] --> R0
Interval 1 (0,1), VReg TMP:0 [int] --> R1
Generating IR for function simple1
DEBUG gfunc_return: entry, func_type->t=0x3
DEBUG gfunc_return: before RETURNVALUE, vtop->r = 0x0, VT_LVAL=0
=== IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 MUL VReg PAR:0
0001: VReg TMP:1 <-- VReg TMP:0 ADD #42
0002: RETURNVALUE VReg TMP:1
=== END IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 MUL VReg PAR:0
0001: VReg TMP:1 <-- VReg TMP:0 ADD #42
0002: RETURNVALUE VReg TMP:1
Adding live interval for VReg TMP:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:1, start=1 end=2 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg PAR:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Interval 0 (0,1), VReg PAR:0 [int] --> R0
Interval 1 (0,1), VReg TMP:0 [int] --> R1
Interval 2 (1,2), VReg TMP:1 [int] --> R2
Generating IR for function simple_stack
DEBUG gfunc_return: entry, func_type->t=0x3
DEBUG gfunc_return: before RETURNVALUE, vtop->r = 0x0, VT_LVAL=0
=== IR BEFORE OPTIMIZATIONS ===
0000: VReg VAR:0 <-- VReg PAR:0 ADD #123
0001: VReg TMP:1 <-- VReg VAR:0
0002: RETURNVALUE VReg TMP:1
=== END IR BEFORE OPTIMIZATIONS ===
0000: VReg VAR:0 <-- VReg PAR:0 ADD #123
0001: VReg TMP:1 <-- VReg VAR:0
0002: RETURNVALUE VReg TMP:1
Adding live interval for VReg VAR:0, start=1 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:1, start=1 end=2 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Interval 0 (0,1), VReg PAR:0 [int] --> R0
Interval 1 (0,1), VReg TMP:0 [int] --> R1
Interval 2 (1,1), VReg VAR:0 [int] --> R2
Interval 3 (1,2), VReg TMP:1 [int] --> R3
Generating IR for function simple2
DEBUG gfunc_return: entry, func_type->t=0x3
DEBUG gfunc_return: before RETURNVALUE, vtop->r = 0x0, VT_LVAL=0
=== IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 ADD VReg PAR:1
0001: RETURNVALUE VReg TMP:0
=== END IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 ADD VReg PAR:1
0001: RETURNVALUE VReg TMP:0
Adding live interval for VReg TMP:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg PAR:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:1, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Interval 0 (0,1), VReg PAR:0 [int] --> R0
Interval 1 (0,1), VReg PAR:1 [int] --> R1
Interval 2 (0,1), VReg TMP:0 [int] --> R2
Generating IR for function simple3
DEBUG gfunc_return: entry, func_type->t=0x3
DEBUG gfunc_return: before RETURNVALUE, vtop->r = 0x0, VT_LVAL=0
=== IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 MUL VReg PAR:1
0001: VReg TMP:1 <-- VReg TMP:0 ADD VReg PAR:2
0002: RETURNVALUE VReg TMP:1
=== END IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 MUL VReg PAR:1
0001: VReg TMP:1 <-- VReg TMP:0 ADD VReg PAR:2
0002: RETURNVALUE VReg TMP:1
Adding live interval for VReg TMP:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:1, start=1 end=2 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg PAR:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:1, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:2, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Interval 0 (0,1), VReg PAR:0 [int] --> R0
Interval 1 (0,1), VReg PAR:1 [int] --> R1
Interval 2 (0,1), VReg PAR:2 [int] --> R2
Interval 3 (0,1), VReg TMP:0 [int] --> R3
Interval 4 (1,2), VReg TMP:1 [int] --> R4
Generating IR for function simple4
DEBUG gfunc_return: entry, func_type->t=0x3
DEBUG gfunc_return: before RETURNVALUE, vtop->r = 0x0, VT_LVAL=0
=== IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 ADD VReg PAR:1
0001: VReg TMP:1 <-- VReg TMP:0 ADD VReg PAR:2
0002: VReg TMP:2 <-- VReg TMP:1 ADD VReg PAR:3
0003: RETURNVALUE VReg TMP:2
=== END IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 ADD VReg PAR:1
0001: VReg TMP:1 <-- VReg TMP:0 ADD VReg PAR:2
0002: VReg TMP:2 <-- VReg TMP:1 ADD VReg PAR:3
0003: RETURNVALUE VReg TMP:2
Adding live interval for VReg TMP:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:1, start=1 end=2 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:2, start=2 end=3 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg PAR:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:1, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:2, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:3, start=0 end=2 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Interval 0 (0,1), VReg PAR:0 [int] --> R0
Interval 1 (0,1), VReg PAR:1 [int] --> R1
Interval 2 (0,1), VReg PAR:2 [int] --> R2
Interval 3 (0,2), VReg PAR:3 [int] --> R3
Interval 4 (0,1), VReg TMP:0 [int] --> R4
Interval 5 (1,2), VReg TMP:1 [int] --> R5
Interval 6 (2,3), VReg TMP:2 [int] --> R0
Generating IR for function simple5
DEBUG gfunc_return: entry, func_type->t=0x3
DEBUG gfunc_return: before RETURNVALUE, vtop->r = 0x0, VT_LVAL=0
=== IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 MUL VReg PAR:1
0001: VReg TMP:1 <-- VReg PAR:2 MUL VReg PAR:3
0002: VReg TMP:2 <-- VReg TMP:0 ADD VReg TMP:1
0003: VReg TMP:3 <-- VReg TMP:2 ADD VReg PAR:4
0004: VReg TMP:4 <-- VReg TMP:3 ADD VReg PAR:5
0005: RETURNVALUE VReg TMP:4
=== END IR BEFORE OPTIMIZATIONS ===
0000: VReg TMP:0 <-- VReg PAR:0 MUL VReg PAR:1
0001: VReg TMP:1 <-- VReg PAR:2 MUL VReg PAR:3
0002: VReg TMP:2 <-- VReg TMP:0 ADD VReg TMP:1
0003: VReg TMP:3 <-- VReg TMP:2 ADD VReg PAR:4
0004: VReg TMP:4 <-- VReg TMP:3 ADD VReg PAR:5
0005: RETURNVALUE VReg TMP:4
Adding live interval for VReg TMP:0, start=0 end=2 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:1, start=1 end=2 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:2, start=2 end=3 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:3, start=3 end=4 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:4, start=4 end=5 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg PAR:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:1, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:2, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:3, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:4, start=0 end=3 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:5, start=0 end=4 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Interval 0 (0,1), VReg PAR:0 [int] --> R0
Interval 1 (0,1), VReg PAR:1 [int] --> R1
Interval 2 (0,1), VReg PAR:2 [int] --> R2
Interval 3 (0,1), VReg PAR:3 [int] --> R3
Interval 4 (0,3), VReg PAR:4 [int] --> R4
Interval 5 (0,4), VReg PAR:5 [int] --> R5
Interval 6 (0,2), VReg TMP:0 [int] --> R6
Interval 7 (1,2), VReg TMP:1 [int] --> R8
Interval 8 (2,3), VReg TMP:2 [int] --> R0
Interval 9 (3,4), VReg TMP:3 [int] --> R1
Interval 10 (4,5), VReg TMP:4 [int] --> R2
Generating IR for function main
DEBUG gfunc_return: entry, func_type->t=0x3
DEBUG gfunc_return: before RETURNVALUE, vtop->r = 0x30, VT_LVAL=0
=== IR BEFORE OPTIMIZATIONS ===
0000: VReg VAR:0 <-- #0
0001: VReg VAR:1 <-- #0
0002: FUNCPARAMVOID
0003: CALL GlobalSym(11451) --> VReg VAR:0
0004: PARAM1 GlobalSym(268435459)
0005: PARAM2 VReg VAR:0
0006: CALL GlobalSym(11297)
0007: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0008: FUNCPARAMVOID
0009: CALL GlobalSym(11452) --> VReg VAR:0
0010: PARAM1 GlobalSym(268435460)
0011: PARAM2 VReg VAR:0
0012: CALL GlobalSym(11297)
0013: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0014: PARAM1 #1
0015: CALL GlobalSym(11453) --> VReg VAR:0
0016: PARAM1 GlobalSym(268435461)
0017: PARAM2 VReg VAR:0
0018: CALL GlobalSym(11297)
0019: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0020: PARAM1 #10
0021: CALL GlobalSym(11454) --> VReg VAR:0
0022: PARAM1 GlobalSym(268435462)
0023: PARAM2 VReg VAR:0
0024: CALL GlobalSym(11297)
0025: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0026: PARAM1 #2
0027: CALL GlobalSym(11455) --> VReg VAR:0
0028: PARAM1 GlobalSym(268435463)
0029: PARAM2 VReg VAR:0
0030: CALL GlobalSym(11297)
0031: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0032: PARAM1 #3
0033: CALL GlobalSym(11456) --> VReg VAR:0
0034: PARAM1 GlobalSym(268435464)
0035: PARAM2 VReg VAR:0
0036: CALL GlobalSym(11297)
0037: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0038: PARAM1 #4
0039: PARAM2 #5
0040: CALL GlobalSym(11457) --> VReg VAR:0
0041: PARAM1 GlobalSym(268435465)
0042: PARAM2 VReg VAR:0
0043: CALL GlobalSym(11297)
0044: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0045: PARAM1 #6
0046: PARAM2 #7
0047: PARAM3 #8
0048: CALL GlobalSym(11458) --> VReg VAR:0
0049: PARAM1 GlobalSym(268435466)
0050: PARAM2 VReg VAR:0
0051: CALL GlobalSym(11297)
0052: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0053: PARAM1 #9
0054: PARAM2 #10
0055: PARAM3 #11
0056: PARAM4 #12
0057: CALL GlobalSym(11460) --> VReg VAR:0
0058: PARAM1 GlobalSym(268435467)
0059: PARAM2 VReg VAR:0
0060: CALL GlobalSym(11297)
0061: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0062: PARAM1 #13
0063: PARAM2 #14
0064: PARAM3 #15
0065: PARAM4 #16
0066: PARAM6 #18
0067: PARAM5 #17
0068: CALL GlobalSym(11462) --> VReg VAR:0
0069: PARAM1 GlobalSym(268435468)
0070: PARAM2 VReg VAR:0
0071: CALL GlobalSym(11297)
0072: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0073: PARAM1 GlobalSym(268435469)
0074: PARAM2 VReg VAR:1
0075: CALL GlobalSym(11297)
0076: RETURNVALUE #0
=== END IR BEFORE OPTIMIZATIONS ===
0000: VReg VAR:0 <-- #0
0001: VReg VAR:1 <-- #0
0002: FUNCPARAMVOID
0003: CALL GlobalSym(11451) --> VReg VAR:0
0004: PARAM1 GlobalSym(268435459)
0005: PARAM2 VReg VAR:0
0006: CALL GlobalSym(11297)
0007: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0008: FUNCPARAMVOID
0009: CALL GlobalSym(11452) --> VReg VAR:0
0010: PARAM1 GlobalSym(268435460)
0011: PARAM2 VReg VAR:0
0012: CALL GlobalSym(11297)
0013: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0014: PARAM1 #1
0015: CALL GlobalSym(11453) --> VReg VAR:0
0016: PARAM1 GlobalSym(268435461)
0017: PARAM2 VReg VAR:0
0018: CALL GlobalSym(11297)
0019: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0020: PARAM1 #10
0021: CALL GlobalSym(11454) --> VReg VAR:0
0022: PARAM1 GlobalSym(268435462)
0023: PARAM2 VReg VAR:0
0024: CALL GlobalSym(11297)
0025: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0026: PARAM1 #2
0027: CALL GlobalSym(11455) --> VReg VAR:0
0028: PARAM1 GlobalSym(268435463)
0029: PARAM2 VReg VAR:0
0030: CALL GlobalSym(11297)
0031: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0032: PARAM1 #3
0033: CALL GlobalSym(11456) --> VReg VAR:0
0034: PARAM1 GlobalSym(268435464)
0035: PARAM2 VReg VAR:0
0036: CALL GlobalSym(11297)
0037: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0038: PARAM1 #4
0039: PARAM2 #5
0040: CALL GlobalSym(11457) --> VReg VAR:0
0041: PARAM1 GlobalSym(268435465)
0042: PARAM2 VReg VAR:0
0043: CALL GlobalSym(11297)
0044: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0045: PARAM1 #6
0046: PARAM2 #7
0047: PARAM3 #8
0048: CALL GlobalSym(11458) --> VReg VAR:0
0049: PARAM1 GlobalSym(268435466)
0050: PARAM2 VReg VAR:0
0051: CALL GlobalSym(11297)
0052: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0053: PARAM1 #9
0054: PARAM2 #10
0055: PARAM3 #11
0056: PARAM4 #12
0057: CALL GlobalSym(11460) --> VReg VAR:0
0058: PARAM1 GlobalSym(268435467)
0059: PARAM2 VReg VAR:0
0060: CALL GlobalSym(11297)
0061: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0062: PARAM1 #13
0063: PARAM2 #14
0064: PARAM3 #15
0065: PARAM4 #16
0066: PARAM6 #18
0067: PARAM5 #17
0068: CALL GlobalSym(11462) --> VReg VAR:0
0069: PARAM1 GlobalSym(268435468)
0070: PARAM2 VReg VAR:0
0071: CALL GlobalSym(11297)
0072: VReg VAR:1 <-- VReg VAR:1 ADD VReg VAR:0
0073: PARAM1 GlobalSym(268435469)
0074: PARAM2 VReg VAR:1
0075: CALL GlobalSym(11297)
0076: RETURNVALUE #0
Adding live interval for VReg VAR:0, start=0 end=72 crosses_call=1 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg VAR:1, start=1 end=74 crosses_call=1 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:0, start=3 end=4 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:2, start=7 end=8 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:3, start=9 end=10 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:5, start=13 end=14 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:6, start=15 end=16 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:8, start=19 end=20 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:9, start=21 end=22 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:11, start=25 end=26 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:12, start=27 end=28 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:14, start=31 end=32 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:15, start=33 end=34 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:17, start=37 end=38 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:18, start=40 end=41 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:20, start=44 end=45 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:21, start=48 end=49 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:23, start=52 end=53 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:24, start=57 end=58 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:26, start=61 end=62 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:27, start=68 end=69 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg TMP:29, start=72 end=73 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 1
Adding live interval for VReg PAR:0, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Adding live interval for VReg PAR:1, start=0 end=1 crosses_call=0 addrtaken=0 reg_type=0, is_lvalue: 0
Interval 0 (0,1), VReg PAR:0 [int] --> R0
Interval 1 (0,1), VReg PAR:1 [int] --> R1
Interval 2 (0,72), VReg VAR:0 [int] --> R4
Interval 3 (1,74), VReg VAR:1 [int] --> R5
Interval 4 (3,4), VReg TMP:0 [int] --> R0
Interval 5 (7,8), VReg TMP:2 [int] --> R0
Interval 6 (9,10), VReg TMP:3 [int] --> R0
Interval 7 (13,14), VReg TMP:5 [int] --> R0
Interval 8 (15,16), VReg TMP:6 [int] --> R0
Interval 9 (19,20), VReg TMP:8 [int] --> R0
Interval 10 (21,22), VReg TMP:9 [int] --> R0
Interval 11 (25,26), VReg TMP:11 [int] --> R0
Interval 12 (27,28), VReg TMP:12 [int] --> R0
Interval 13 (31,32), VReg TMP:14 [int] --> R0
Interval 14 (33,34), VReg TMP:15 [int] --> R0
Interval 15 (37,38), VReg TMP:17 [int] --> R0
Interval 16 (40,41), VReg TMP:18 [int] --> R0
Interval 17 (44,45), VReg TMP:20 [int] --> R0
Interval 18 (48,49), VReg TMP:21 [int] --> R0
Interval 19 (52,53), VReg TMP:23 [int] --> R0
Interval 20 (57,58), VReg TMP:24 [int] --> R0
Interval 21 (61,62), VReg TMP:26 [int] --> R0
Interval 22 (68,69), VReg TMP:27 [int] --> R0
Interval 23 (72,73), VReg TMP:29 [int] --> R0