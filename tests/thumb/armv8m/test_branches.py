from . import utils 

def is_compatible_branch_instruction(a, b, branch_set):
    if a in branch_set and b in branch_set:
        return True
    return False 

def perform_test_for_branches(file, branches_set):
    output_file = utils.compile_code(file)
    disassembly_sut = utils.disassemble_code(output_file).splitlines()
    expected_file = utils.prepare_expect(file)
    disassembly_expected = utils.disassemble_code(expected_file).splitlines()
    sut = utils.cleanup_dissambly(disassembly_sut)
    expected = utils.cleanup_dissambly(disassembly_expected) 
    assert len(expected) > 0, "Expected disassembly is empty"
    
    for i in range(len(expected)):
        if (sut[i][1] != expected[i][1]):
            # try to determine if compatible instruction is being used
            if is_compatible_branch_instruction(sut[i][2], expected[i][2], branches_set):
                sut_label = sut[i][3].split("<")[-1][:-1]
                expected_label = expected[i][3].split("<")[-1][:-1]
                assert sut_label == expected_label, f"Mismatch at line {i}: {sut[i]} != {expected[i]}"   
                continue
        
        assert sut[i][1] == expected[i][1], f"Mismatch at line {i}: {sut[i]} != {expected[i]}"   


def test_bx():
    utils.perform_test_for_file("test_bx.S")
    
def test_bl():
    branch_set = ["bl", "bl.n", "bl.w", "blgt.w", "blgt.n"]
    perform_test_for_branches("test_bl.S", branch_set)

def test_blx():
    utils.perform_test_for_file("test_blx.S")


def test_b():
    branch_set = ["b", "b.n", "b.w", "bgt.w", "bgt.n"]
    perform_test_for_branches("test_b.S", branch_set)
    
def test_cbz():
    branch_set = ["cbz", "cbnz"]
    perform_test_for_branches("test_cbz.S", branch_set)